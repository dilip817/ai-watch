import os
import hmac
import hashlib
import logging

from fastapi import APIRouter, UploadFile, File, Header, HTTPException
from models import IngestResponse, IngestItemResult
from services.stt import transcribe_audio
from services.intent import extract_intent
from services.db import insert_item, create_session, update_session_item_count

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


@router.post("/ingest", response_model=IngestResponse)
async def ingest_audio(
    audio: UploadFile = File(...),
    x_device_id: str = Header(default="demo-device"),
    x_device_token: str = Header(default=""),
    x_device_ts: str = Header(default=""),
    x_domain: str = Header(default="general"),
):
    if not _verify_device(x_device_id, x_device_token):
        raise HTTPException(status_code=401, detail="Invalid device token")

    # Normalize and validate domain
    domain = x_domain.lower().strip() if x_domain else "general"
    if domain not in VALID_DOMAINS:
        domain = "general"

    audio_bytes = await audio.read()
    if len(audio_bytes) > 15 * 1024 * 1024:
        raise HTTPException(status_code=413, detail="Audio file too large (max 15MB)")

    # STT
    try:
        transcript = await transcribe_audio(audio_bytes, audio.filename or "audio.wav")
    except Exception as e:
        logger.error(f"STT failed: {e}", exc_info=True)
        return IngestResponse(status="error", transcript=None)

    logger.info(f"Transcript [{domain}]: {transcript!r}")

    if not transcript or not transcript.strip():
        return IngestResponse(status="empty")

    # Create a session to group all items from this recording
    try:
        session_id = await create_session(
            device_id=x_device_id,
            domain=domain,
            transcript=transcript,
            title=None,  # auto-title will be derived from first item
        )
    except Exception as e:
        logger.error(f"Session creation failed: {e}", exc_info=True)
        session_id = None

    # Domain-aware intent extraction (returns list of intents)
    try:
        intents = await extract_intent(transcript, domain=domain)
    except Exception as e:
        logger.error(f"Intent extraction failed: {e}", exc_info=True)
        intents = [{
            "type": "note",
            "category": "note",
            "title": transcript[:60],
            "body": transcript,
            "confidence": 0.3,
        }]

    # Insert each item into DB, linked to the session
    saved_items: list[IngestItemResult] = []
    for intent in intents:
        confidence = intent.get("confidence", 1.0)
        item_type = intent.get("type", "note")
        category = intent.get("category") or item_type
        title = intent.get("title") or transcript[:60]

        try:
            item_id = await insert_item(
                device_id=x_device_id,
                type_=item_type,
                title=title,
                body=intent.get("body"),
                datetime_=intent.get("datetime"),
                due_date=intent.get("due_date"),
                subject=intent.get("subject"),
                location=intent.get("location"),
                confidence=confidence,
                category=category,
                domain=domain,
                session_id=session_id,
                recurrence=intent.get("recurrence"),
            )
            saved_items.append(IngestItemResult(
                item_id=item_id,
                type=item_type,
                category=category,
                title=title,
                due_date=intent.get("due_date"),
                datetime=intent.get("datetime"),
                recurrence=intent.get("recurrence"),
                confidence=confidence,
            ))
        except Exception as e:
            logger.error(f"DB insert failed for '{title}': {e}", exc_info=True)

    if not saved_items:
        return IngestResponse(status="error", transcript=transcript)

    # Update session with final item count
    if session_id:
        try:
            await update_session_item_count(session_id, len(saved_items))
        except Exception as e:
            logger.warning(f"Failed to update session item count: {e}")

    # Overall status: ok if all items have reasonable confidence
    min_conf = min(item.confidence for item in saved_items)
    status = "ok" if min_conf >= 0.6 else "confirm_needed"

    # Top-level fields from first item (backward compatibility with firmware)
    first = saved_items[0]

    return IngestResponse(
        status=status,
        item_id=first.item_id,
        type=first.type,
        title=first.title,
        due_date=first.due_date,
        datetime=first.datetime,
        confidence=first.confidence,
        transcript=transcript,
        session_id=session_id,
        domain=domain,
        items=saved_items,
        count=len(saved_items),
    )
