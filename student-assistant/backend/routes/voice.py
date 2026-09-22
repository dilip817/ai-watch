"""
Unified /voice endpoint — handles both saving new items and answering questions.

The watch sends all voice input here. The backend:
1. Transcribes via Whisper
2. Classifies intent (save new items vs query/readback)
3. Executes the appropriate logic
4. Always returns 16kHz mono WAV audio so the watch just plays it

Response headers:
  X-Voice-Mode:  "save" | "query" | "empty"
  X-Item-Count:  N (only for save)
  X-Session-Id:  UUID (only for save)
  X-Narration:   spoken text (for query)
"""

import os
import hmac
import hashlib
import logging

from fastapi import APIRouter, UploadFile, File, Header, HTTPException, Response

from services.stt import transcribe_audio
from services.intent import extract_intent
from services.intent_router import classify_voice_intent
from services.date_parser import parse_date_range
from services.db import (
    insert_item, create_session, update_session_item_count,
    get_items_in_range, get_items_for_device,
)
from services.tts import synthesize_speech_wav, SILENT_WAV
from services.domains import get_domain

# Reuse narration helpers from query route
from routes.query import _narrate_items, _friendly_date, _friendly_time

router = APIRouter()
logger = logging.getLogger(__name__)

VALID_DOMAINS = {"sports", "music", "classroom", "general"}


def _verify_device(device_id: str, device_token: str) -> bool:
    mode = os.getenv("MODE", "mock")
    if mode == "mock":
        return True
    secret = os.getenv("DEVICE_SECRET", "")
    expected = hmac.new(
        secret.encode(), device_id.encode(), hashlib.sha256
    ).hexdigest()
    return hmac.compare_digest(expected, device_token)


def _build_save_narration(items: list[dict], domain: str) -> str:
    """Build a spoken confirmation for saved items."""
    if not items:
        return "Nothing was captured. Please try again."
    if len(items) == 1:
        title = items[0].get("title", "item")
        return f"Saved: {title}."
    titles = ", ".join(i.get("title", "") for i in items[:3])
    extra = f" and {len(items) - 3} more" if len(items) > 3 else ""
    return f"Saved {len(items)} items: {titles}{extra}."


def _build_readback_narration(transcript: str, items: list[dict]) -> str:
    """
    Build narration for a read-all or filtered query request.
    Groups by category: insights, actions, events.
    """
    lower = transcript.lower()

    # Filter by category if the user asked for a specific one
    if "insight" in lower:
        items = [i for i in items if i.get("category") == "insight"]
        label = "insights"
    elif "action" in lower:
        items = [i for i in items if i.get("category") == "action"]
        label = "actions"
    elif "event" in lower:
        items = [i for i in items if i.get("category") == "event"]
        label = "events"
    else:
        label = "items"

    if not items:
        return f"You have no {label} saved yet."

    active = [i for i in items if not i.get("done")]
    if not active:
        return f"All your {label} are marked as done."

    parts = [f"You have {len(active)} {label}."]
    for item in active[:5]:
        title = item.get("title", "")
        date = item.get("due_date") or (item.get("datetime", "")[:10] if item.get("datetime") else "")
        date_str = f", {_friendly_date(date)}" if date else ""
        parts.append(f"{title}{date_str}.")
    if len(active) > 5:
        parts.append(f"And {len(active) - 5} more.")

    return " ".join(parts)


@router.post("/voice")
async def voice_unified(
    audio: UploadFile = File(...),
    x_device_id: str = Header(default="demo-device"),
    x_device_token: str = Header(default=""),
    x_device_ts: str = Header(default=""),
    x_domain: str = Header(default="general"),
):
    """
    Unified voice endpoint. Classifies intent and routes to save or query.
    Always returns 16kHz mono WAV audio + metadata headers.
    """
    if not _verify_device(x_device_id, x_device_token):
        raise HTTPException(status_code=401, detail="Invalid device token")

    domain = x_domain.lower().strip()
    if domain not in VALID_DOMAINS:
        domain = "general"

    audio_bytes = await audio.read()
    if len(audio_bytes) > 15 * 1024 * 1024:
        wav = await synthesize_speech_wav("That recording was too long. Please try a shorter message.")
        return Response(wav, media_type="audio/wav", headers={"X-Voice-Mode": "error"})

    # --- STT ---
    try:
        transcript = await transcribe_audio(audio_bytes, audio.filename or "audio.wav")
    except Exception as e:
        logger.error(f"STT failed: {e}", exc_info=True)
        wav = await synthesize_speech_wav("I couldn't understand that. Please try again.")
        return Response(wav, media_type="audio/wav", headers={"X-Voice-Mode": "error"})

    logger.info(f"Voice transcript [{domain}]: {transcript!r}")

    if not transcript or not transcript.strip():
        return Response(SILENT_WAV, media_type="audio/wav", headers={"X-Voice-Mode": "empty"})

    # --- Classify intent ---
    mode = classify_voice_intent(transcript)
    logger.info(f"Voice intent: {mode!r}")

    if mode == "save":
        # ---- SAVE: extract items and persist ----
        try:
            intents = await extract_intent(transcript, domain=domain)
        except Exception as e:
            logger.error(f"Intent extraction failed: {e}", exc_info=True)
            intents = [{"type": "note", "category": "note", "title": transcript[:60],
                        "body": transcript, "confidence": 0.3}]

        session_id = None
        try:
            session_id = await create_session(x_device_id, domain, transcript)
        except Exception as e:
            logger.warning(f"Session creation failed: {e}")

        saved = []
        for intent in intents:
            try:
                item_id = await insert_item(
                    device_id=x_device_id,
                    type_=intent.get("type", "note"),
                    title=intent.get("title", transcript[:60]),
                    body=intent.get("body"),
                    datetime_=intent.get("datetime"),
                    due_date=intent.get("due_date"),
                    subject=intent.get("subject"),
                    location=intent.get("location"),
                    confidence=intent.get("confidence", 1.0),
                    category=intent.get("category", "note"),
                    domain=domain,
                    session_id=session_id,
                    recurrence=intent.get("recurrence"),
                )
                saved.append({"item_id": item_id, **intent})
            except Exception as e:
                logger.error(f"DB insert failed: {e}", exc_info=True)

        if session_id and saved:
            await update_session_item_count(session_id, len(saved))

        narration = _build_save_narration(saved, domain)
        logger.info(f"Voice save: {len(saved)} items — {narration!r}")

        try:
            wav = await synthesize_speech_wav(narration)
        except Exception:
            wav = SILENT_WAV

        headers = {
            "X-Voice-Mode": "save",
            "X-Item-Count": str(len(saved)),
        }
        if session_id:
            headers["X-Session-Id"] = session_id

        return Response(wav, media_type="audio/wav", headers=headers)

    else:
        # ---- QUERY: look up items and narrate ----
        lower = transcript.lower()

        # Detect "read all" vs date-specific query
        read_all_signals = ["read", "all", "everything", "show", "list", "my insights",
                            "my actions", "my events", "my notes"]
        is_read_all = any(s in lower for s in read_all_signals) and "?" not in transcript

        if is_read_all:
            items = await get_items_for_device(x_device_id)
            narration = _build_readback_narration(transcript, items)
        else:
            start_date, end_date = parse_date_range(transcript)
            items = await get_items_in_range(x_device_id, start_date, end_date)
            narration = _narrate_items(items, transcript)

        logger.info(f"Voice query: {narration[:100]!r}")

        try:
            wav = await synthesize_speech_wav(narration)
        except Exception:
            wav = SILENT_WAV

        return Response(wav, media_type="audio/wav", headers={
            "X-Voice-Mode": "query",
            "X-Narration": narration[:500],
        })
