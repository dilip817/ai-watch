import os
import json
import logging

from services.domains import build_system_prompt

logger = logging.getLogger(__name__)


def _mock_items_for_domain(transcript: str, domain: str) -> list[dict]:
    """Return domain-appropriate mock items for testing."""
    base = {"body": transcript, "confidence": 0.85, "datetime": None, "due_date": None,
            "recurrence": None, "subject": None, "location": None}
    if domain == "sports":
        return [
            {**base, "type": "note", "category": "insight",
             "title": "Work on left foot positioning", "confidence": 0.9},
            {**base, "type": "reminder", "category": "action",
             "title": "Practice 50 free kicks daily", "recurrence": "daily", "confidence": 0.85},
            {**base, "type": "event", "category": "event",
             "title": "Game Saturday 10am", "datetime": "2026-04-18T10:00",
             "due_date": "2026-04-18", "location": "City Park", "confidence": 0.95},
        ]
    elif domain == "music":
        return [
            {**base, "type": "note", "category": "insight",
             "title": "Tempo drifted in measure 32 — stay with the metronome"},
            {**base, "type": "reminder", "category": "action",
             "title": "Practice scales 15 min daily", "recurrence": "daily"},
            {**base, "type": "event", "category": "event",
             "title": "Recital Sunday 2pm", "datetime": "2026-04-20T14:00",
             "due_date": "2026-04-20", "location": "Town Hall"},
        ]
    elif domain == "classroom":
        return [
            {**base, "type": "note", "category": "insight",
             "title": "Remember: area of circle = π r²", "subject": "Math"},
            {**base, "type": "reminder", "category": "action",
             "title": "Read chapter 5 by Friday", "due_date": "2026-04-17", "subject": "English"},
            {**base, "type": "event", "category": "event",
             "title": "Math test Friday", "datetime": "2026-04-17T09:00",
             "due_date": "2026-04-17", "subject": "Math"},
        ]
    else:
        return [{
            "type": "note",
            "category": "note",
            "title": transcript[:60],
            "body": transcript,
            "confidence": 0.5,
            "datetime": None, "due_date": None, "recurrence": None,
            "subject": None, "location": None,
        }]


async def extract_intent(transcript: str, domain: str = "general") -> list[dict]:
    """Extract one or more intents from a transcript. Always returns a list.

    Args:
        transcript: The transcribed speech.
        domain: Activity domain — 'soccer', 'music', 'classroom', or 'general'.

    Returns:
        List of extracted item dicts with keys: type, category, title, body,
        datetime, due_date, recurrence, subject, location, confidence.
    """
    mode = os.getenv("MODE", "mock")
    if mode == "mock":
        items = _mock_items_for_domain(transcript, domain)
        for item in items:
            logger.info(f"[mock] Intent → {item['category']}: {item['title']}")
        return items

    import openai

    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        logger.error("OPENAI_API_KEY not set")
        return [{
            "type": "note",
            "category": "note",
            "title": transcript[:60],
            "body": transcript,
            "confidence": 0.3,
            "datetime": None, "due_date": None, "recurrence": None,
            "subject": None, "location": None,
        }]

    system_prompt = build_system_prompt(domain)
    client = openai.AsyncOpenAI(api_key=api_key)
    raw = ""
    try:
        response = await client.chat.completions.create(
            model="gpt-4o-mini",
            max_tokens=1200,
            temperature=0,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": transcript},
            ],
        )

        raw = response.choices[0].message.content.strip()
        logger.debug(f"Raw intent response: {raw}")

        # Strip markdown fences if present
        if raw.startswith("```"):
            lines = raw.split("\n")
            lines = [line for line in lines if not line.startswith("```")]
            raw = "\n".join(lines)

        result = json.loads(raw)

        # Normalize: LLM may return a single object instead of an array
        if isinstance(result, dict):
            result = [result]
        elif not isinstance(result, list):
            result = [{
                "type": "note", "category": "note",
                "title": transcript[:60], "body": transcript, "confidence": 0.3,
            }]

        # Backfill missing category field (graceful degradation)
        for item in result:
            if "category" not in item or not item.get("category"):
                item["category"] = item.get("type", "note")
            for field in ("recurrence", "subject", "location", "body", "datetime", "due_date"):
                item.setdefault(field, None)
            logger.info(
                f"Intent[{domain}] → {item.get('category')}: "
                f"{item.get('title')} (conf={item.get('confidence')})"
            )

        return result

    except json.JSONDecodeError as e:
        logger.warning(f"JSON parse error from intent LLM: {e}. Raw: {raw!r}")
        return [{
            "type": "note", "category": "note",
            "title": transcript[:60], "body": transcript, "confidence": 0.3,
            "datetime": None, "due_date": None, "recurrence": None,
            "subject": None, "location": None,
        }]
    except Exception as e:
        logger.error(f"Intent extraction failed: {e}", exc_info=True)
        raise
