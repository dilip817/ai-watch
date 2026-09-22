import os
import hmac
import hashlib
import logging

from fastapi import APIRouter, UploadFile, File, Header, HTTPException, Response
from services.stt import transcribe_audio
from services.date_parser import parse_date_range
from services.db import get_items_in_range
from services.tts import synthesize_speech, synthesize_speech_wav, SILENT_MP3, SILENT_WAV

router = APIRouter()
logger = logging.getLogger(__name__)


def _verify_device(device_id: str, device_token: str) -> bool:
    mode = os.getenv("MODE", "mock")
    if mode == "mock":
        return True
    secret = os.getenv("DEVICE_SECRET", "")
    expected = hmac.new(
        secret.encode(), device_id.encode(), hashlib.sha256
    ).hexdigest()
    return hmac.compare_digest(expected, device_token)


def _friendly_date(date_str: str) -> str:
    """Convert '2026-03-30' to 'Sunday, March 30th'."""
    if not date_str:
        return ""
    try:
        from datetime import datetime as dt, timedelta
        d = dt.strptime(date_str[:10], "%Y-%m-%d")
        today = dt.now().replace(hour=0, minute=0, second=0, microsecond=0)
        diff = (d - today).days
        if diff == 0:
            relative = "today"
        elif diff == 1:
            relative = "tomorrow"
        elif diff == -1:
            relative = "yesterday"
        elif 2 <= diff <= 6:
            relative = f"this {d.strftime('%A')}"
        else:
            relative = None

        day = d.day
        suffix = "th" if 11 <= day <= 13 else {1: "st", 2: "nd", 3: "rd"}.get(day % 10, "th")
        full = f"{d.strftime('%A')}, {d.strftime('%B')} {day}{suffix}"
        return relative if relative else full
    except Exception:
        return date_str


def _friendly_time(datetime_str: str) -> str:
    """Convert '2026-03-30T10:00' to '10 AM'."""
    if not datetime_str or "T" not in datetime_str:
        return ""
    try:
        from datetime import datetime as dt
        d = dt.strptime(datetime_str[:16], "%Y-%m-%dT%H:%M")
        minute_part = f":{d.strftime('%M')}" if d.minute != 0 else ""
        hour = d.hour % 12 or 12
        ampm = "AM" if d.hour < 12 else "PM"
        return f"{hour}{minute_part} {ampm}"
    except Exception:
        return datetime_str


def _narrate_items(items: list[dict], transcript: str) -> str:
    if not items:
        return "You don't have anything scheduled for that time period."

    parts = []
    reminders = [i for i in items if i["type"] == "reminder"]
    events = [i for i in items if i["type"] == "event"]
    notes = [i for i in items if i["type"] == "note"]

    if reminders:
        parts.append(f"You have {len(reminders)} reminder{'s' if len(reminders) > 1 else ''}.")
        for r in reminders[:3]:
            due = f", due {_friendly_date(r['due_date'])}" if r.get("due_date") else ""
            parts.append(f"{r['title']}{due}.")

    if events:
        parts.append(f"You have {len(events)} event{'s' if len(events) > 1 else ''}.")
        for e in events[:3]:
            date_part = _friendly_date(e.get("due_date") or (e.get("datetime", "")[:10] if e.get("datetime") else ""))
            time_part = _friendly_time(e.get("datetime"))
            when = ""
            if date_part and time_part:
                when = f" on {date_part} at {time_part}"
            elif date_part:
                when = f" on {date_part}"
            elif time_part:
                when = f" at {time_part}"
            where = f" in {e['location']}" if e.get("location") else ""
            parts.append(f"{e['title']}{when}{where}.")

    if notes:
        parts.append(f"You also have {len(notes)} note{'s' if len(notes) > 1 else ''}.")

    return " ".join(parts)


@router.post("/query")
async def query_audio(
    audio: UploadFile = File(...),
    x_device_id: str = Header(default="demo-device"),
    x_device_token: str = Header(default=""),
    x_device_ts: str = Header(default=""),
):
    try:
        if not _verify_device(x_device_id, x_device_token):
            raise HTTPException(status_code=401, detail="Invalid device token")

        audio_bytes = await audio.read()
        if len(audio_bytes) > 15 * 1024 * 1024:
            msg = "That recording was too long. Please try a shorter question."
            audio_out = await synthesize_speech_wav(msg)
            return Response(content=audio_out, media_type="audio/wav",
                            headers={"X-Narration": msg})

        transcript = await transcribe_audio(audio_bytes, audio.filename or "audio.wav")
        logger.info(f"Query transcript: {transcript!r}")

        if not transcript or not transcript.strip():
            msg = "I didn't catch that. Could you try again?"
            audio_out = await synthesize_speech_wav(msg)
            return Response(content=audio_out, media_type="audio/wav",
                            headers={"X-Narration": msg})

        # Parse date range from question
        start_date, end_date = parse_date_range(transcript)
        logger.info(f"Date range: {start_date} → {end_date}")

        # Fetch matching items
        items = await get_items_in_range(x_device_id, start_date, end_date)
        logger.info(f"Found {len(items)} items for query")

        # Narrate
        narration = _narrate_items(items, transcript)

        # TTS → WAV (16kHz, 16-bit, mono for ESP32 playback)
        audio_out = await synthesize_speech_wav(narration)
        logger.info(f"Query response: {len(audio_out)} bytes WAV, narration: {narration[:100]}")
        return Response(
            content=audio_out,
            media_type="audio/wav",
            headers={"X-Narration": narration[:500]},
        )

    except HTTPException:
        raise  # Let auth errors propagate as-is
    except Exception as e:
        logger.error(f"Query pipeline error: {e}", exc_info=True)
        try:
            fallback = await synthesize_speech_wav(
                "Sorry, something went wrong. Please try again."
            )
        except Exception:
            fallback = SILENT_WAV
        return Response(content=fallback, media_type="audio/wav",
                        headers={"X-Narration": "Sorry, something went wrong."})
