"""
Soccer Coach Test Case — Post-Game Debrief

WATCH TEST SCRIPT (speak this into the watch):
===============================================
  "Alright team, good effort today. A few things to work on.
   First, our defensive line was too high in the second half —
   we need to stay compact and drop back when they counter.
   Alex, your left foot positioning needs a lot of work, keep
   your plant foot pointing at the target.
   Everyone needs to practice their weak foot — I want fifty
   touches each day minimum.
   We'll do a set-piece drill every practice this week.
   Remember, our next game is Saturday at ten AM at Riverside Park.
   Tuesday practice is at five thirty PM at the school field.
   Great job on the pressing though — that was much better
   than last week."

EXPECTED EXTRACTION (soccer domain):
======================================
  INSIGHTS (category=insight, type=note):
    - "Defensive line too high in second half — stay compact on counter"
    - "Alex's left foot positioning needs work — plant foot pointing at target"
    - "Good pressing — improved from last week"

  ACTIONS (category=action, type=reminder):
    - "Practice weak foot 50 touches daily"  [recurrence=daily]
    - "Set-piece drill every practice this week"  [recurrence=every practice]

  EVENTS (category=event, type=event):
    - "Game — Saturday 10:00 AM at Riverside Park"
    - "Practice — Tuesday 5:30 PM at school field"

HOW TO TEST ON THE WATCH:
===========================
  1. Tap domain badge → select "Soccer"
  2. Short press button → watch shows recording screen
  3. Read the script above clearly (about 30-40 seconds)
  4. Short press button again to stop
  5. Watch processes → shows session summary screen with 3 sections:
       INSIGHTS | ACTIONS | EVENTS
  6. Verify all 7 items are extracted correctly

AUTOMATED BACKEND TEST (mock mode — no API key needed):
=========================================================
  Run: pytest tests/test_soccer_scenario.py -v
"""

import pytest
import io
import struct

pytestmark = pytest.mark.asyncio

SOCCER_SCRIPT = (
    "Alright team, good effort today. A few things to work on. "
    "First, our defensive line was too high in the second half — "
    "we need to stay compact and drop back when they counter. "
    "Alex, your left foot positioning needs a lot of work, keep "
    "your plant foot pointing at the target. "
    "Everyone needs to practice their weak foot — I want fifty "
    "touches each day minimum. "
    "We'll do a set-piece drill every practice this week. "
    "Remember, our next game is Saturday at ten AM at Riverside Park. "
    "Tuesday practice is at five thirty PM at the school field. "
    "Great job on the pressing though — that was much better than last week."
)

EXPECTED_CATEGORIES = {"insight", "action", "event"}
EXPECTED_MIN_ITEMS = 3  # at least one of each category
EXPECTED_DOMAIN = "sports"


def make_wav(duration_secs: int = 1) -> bytes:
    """Generate a minimal valid WAV file."""
    sample_rate, channels, bits = 16000, 1, 16
    num_samples = sample_rate * duration_secs
    data_size = num_samples * channels * (bits // 8)
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_size, b"WAVE",
        b"fmt ", 16, 1, channels, sample_rate,
        sample_rate * channels * (bits // 8),
        channels * (bits // 8), bits,
        b"data", data_size,
    )
    return header + b"\x00" * data_size


# ---------------------------------------------------------------------------
# 1. Ingest with soccer domain
# ---------------------------------------------------------------------------

async def test_soccer_ingest_returns_session(client, headers):
    """Ingesting audio in soccer domain should return a session_id."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["status"] in ("ok", "confirm_needed"), f"Unexpected status: {body['status']}"
    assert body["domain"] == EXPECTED_DOMAIN
    assert body["session_id"] is not None, "No session_id returned"
    assert body["count"] >= EXPECTED_MIN_ITEMS, (
        f"Expected at least {EXPECTED_MIN_ITEMS} items, got {body['count']}"
    )


async def test_soccer_ingest_has_all_categories(client, headers):
    """Soccer ingest should return insight, action, and event categories."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    found_categories = {item["category"] for item in body["items"]}
    missing = EXPECTED_CATEGORIES - found_categories
    assert not missing, (
        f"Missing categories: {missing}. Got: {found_categories}"
    )


async def test_soccer_insight_items_are_notes(client, headers):
    """Insight items should have type='note'."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    insights = [i for i in body["items"] if i["category"] == "insight"]
    assert insights, "No insight items extracted"
    for item in insights:
        assert item["type"] == "note", (
            f"Insight '{item['title']}' has type '{item['type']}', expected 'note'"
        )


async def test_soccer_action_items_are_reminders(client, headers):
    """Action items should have type='reminder'."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    actions = [i for i in body["items"] if i["category"] == "action"]
    assert actions, "No action items extracted"
    for item in actions:
        assert item["type"] == "reminder", (
            f"Action '{item['title']}' has type '{item['type']}', expected 'reminder'"
        )


async def test_soccer_event_items_have_dates(client, headers):
    """Event items should have type='event' and a date."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    events = [i for i in body["items"] if i["category"] == "event"]
    assert events, "No event items extracted"
    for item in events:
        assert item["type"] == "event", (
            f"Event '{item['title']}' has type '{item['type']}', expected 'event'"
        )
        has_date = item.get("due_date") or item.get("datetime")
        assert has_date, f"Event '{item['title']}' has no date"


# ---------------------------------------------------------------------------
# 2. Session is stored and retrievable
# ---------------------------------------------------------------------------

async def test_soccer_session_stored_and_retrievable(client, headers):
    """After ingest, session should be fetchable via /sessions endpoint."""
    ingest_resp = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    session_id = ingest_resp.json()["session_id"]

    session_resp = await client.get(f"/session/{session_id}/items")
    assert session_resp.status_code == 200
    body = session_resp.json()
    assert body["session"]["id"] == session_id
    assert body["session"]["domain"] == "sports"
    assert len(body["items"]) >= EXPECTED_MIN_ITEMS


async def test_soccer_session_items_linked_to_session(client, headers):
    """All items returned from /session/{id}/items should have the same session_id."""
    ingest_resp = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    session_id = ingest_resp.json()["session_id"]

    session_resp = await client.get(f"/session/{session_id}/items")
    items = session_resp.json()["items"]
    for item in items:
        assert item["session_id"] == session_id, (
            f"Item '{item['title']}' has session_id={item['session_id']}, expected {session_id}"
        )
        assert item["domain"] == "sports"


# ---------------------------------------------------------------------------
# 3. /sessions/latest reflects the soccer session
# ---------------------------------------------------------------------------

async def test_soccer_latest_session(client, headers):
    """Latest session should be the soccer one just ingested."""
    await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("coach_talk.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    resp = await client.get("/sessions/test-device/latest")
    assert resp.status_code == 200
    body = resp.json()
    assert body["session"]["domain"] == "sports"
    assert body["session"]["item_count"] >= EXPECTED_MIN_ITEMS
    categories = {i["category"] for i in body["items"]}
    assert categories & EXPECTED_CATEGORIES


# ---------------------------------------------------------------------------
# 4. Domain config for soccer
# ---------------------------------------------------------------------------

async def test_soccer_domain_config(client):
    """Soccer domain config should have correct icon and categories."""
    resp = await client.get("/domains")
    domains = {d["id"]: d for d in resp.json()}
    assert "sports" in domains
    sports = domains["sports"]
    
    assert sports["name"] == "Sports"
    assert "insight" in sports["categories"]
    assert "action" in sports["categories"]
    assert "event" in sports["categories"]
