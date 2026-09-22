"""
Classroom Teacher Test Case — End-of-Class Instructions

WATCH TEST SCRIPT (speak this into the watch):
===============================================
  "Okay class, before you leave, a few important things.
   Today we covered the water cycle — remember the three stages
   are evaporation, condensation, and precipitation.
   Also, photosynthesis uses sunlight, water, and carbon dioxide
   to produce glucose and oxygen — that formula will be on the test.
   For homework, read chapter six in your science textbook and
   answer questions one through five at the end of the chapter —
   that's due tomorrow.
   You also need to finish your volcano project — it's due next Friday.
   Don't forget, we have a spelling test this Thursday morning.
   The science fair is on Wednesday the twenty-third at two PM
   in the school gymnasium — parents are welcome.
   Finally, extra credit is available — write a one-page summary
   of any ecosystem of your choice, due by end of next week."

EXPECTED EXTRACTION (classroom domain):
=========================================
  INSIGHTS (category=insight, type=note):
    - "Water cycle has three stages: evaporation, condensation, precipitation"
    - "Photosynthesis: sunlight + water + CO₂ → glucose + oxygen (on test)"

  ACTIONS (category=action, type=reminder):
    - "Read chapter 6 and answer questions 1–5"       [due=tomorrow, subject=Science]
    - "Finish volcano project"                         [due=next Friday, subject=Science]
    - "Study for spelling test"                        [due=Thursday, subject=English]
    - "Extra credit: one-page ecosystem summary"       [due=end of next week]

  EVENTS (category=event, type=event):
    - "Spelling test — Thursday morning"
    - "Science fair — Wed 23rd, 2:00 PM, school gymnasium"

HOW TO TEST ON THE WATCH:
===========================
  1. Tap domain badge → select "Class"
  2. Short press button → recording screen appears
  3. Read the script above clearly (about 35–45 seconds)
  4. Short press button again to stop
  5. Watch processes → shows session summary with 3 sections:
       INSIGHTS | ACTIONS | EVENTS
  6. Verify items appear in the correct sections

AUTOMATED BACKEND TEST:
=========================
  Run: pytest tests/test_classroom_scenario.py -v
"""

import pytest
import io
import struct

pytestmark = pytest.mark.asyncio

CLASSROOM_SCRIPT = (
    "Okay class, before you leave, a few important things. "
    "Today we covered the water cycle — remember the three stages "
    "are evaporation, condensation, and precipitation. "
    "Also, photosynthesis uses sunlight, water, and carbon dioxide "
    "to produce glucose and oxygen — that formula will be on the test. "
    "For homework, read chapter six in your science textbook and "
    "answer questions one through five at the end of the chapter — "
    "that's due tomorrow. "
    "You also need to finish your volcano project — it's due next Friday. "
    "Don't forget, we have a spelling test this Thursday morning. "
    "The science fair is on Wednesday the twenty-third at two PM "
    "in the school gymnasium — parents are welcome. "
    "Finally, extra credit is available — write a one-page summary "
    "of any ecosystem of your choice, due by end of next week."
)

EXPECTED_CATEGORIES = {"insight", "action", "event"}
EXPECTED_MIN_ITEMS = 3   # one of each category (mock returns exactly 3; real LLM returns more)
EXPECTED_DOMAIN = "classroom"


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
# 1. Basic ingest with classroom domain
# ---------------------------------------------------------------------------

async def test_classroom_ingest_returns_session(client, headers):
    """Ingesting audio in classroom domain should return a session_id."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["status"] in ("ok", "confirm_needed"), f"Unexpected status: {body['status']}"
    assert body["domain"] == EXPECTED_DOMAIN
    assert body["session_id"] is not None, "No session_id returned"
    assert body["count"] >= EXPECTED_MIN_ITEMS, (
        f"Expected at least {EXPECTED_MIN_ITEMS} items, got {body['count']}"
    )


async def test_classroom_ingest_has_all_categories(client, headers):
    """Classroom ingest should return insight, action, and event categories."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    found = {item["category"] for item in body["items"]}
    missing = EXPECTED_CATEGORIES - found
    assert not missing, f"Missing categories: {missing}. Got: {found}"


# ---------------------------------------------------------------------------
# 2. Category-specific assertions
# ---------------------------------------------------------------------------

async def test_classroom_insight_items_are_notes(client, headers):
    """Insight items (key concepts) should have type='note'."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    insights = [i for i in body["items"] if i["category"] == "insight"]
    assert insights, "No insight items extracted — expected key concepts from lesson"
    for item in insights:
        assert item["type"] == "note", (
            f"Insight '{item['title']}' has type='{item['type']}', expected 'note'"
        )
        assert item["title"], "Insight has empty title"


async def test_classroom_action_items_are_reminders(client, headers):
    """Action items (homework/assignments) should have type='reminder'."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    actions = [i for i in body["items"] if i["category"] == "action"]
    assert actions, "No action items extracted — expected homework assignments"
    for item in actions:
        assert item["type"] == "reminder", (
            f"Action '{item['title']}' has type='{item['type']}', expected 'reminder'"
        )


async def test_classroom_event_items_are_events_with_dates(client, headers):
    """Event items (tests, fair) should have type='event' and a date."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    events = [i for i in body["items"] if i["category"] == "event"]
    assert events, "No event items extracted — expected spelling test and science fair"
    for item in events:
        assert item["type"] == "event", (
            f"Event '{item['title']}' has type='{item['type']}', expected 'event'"
        )
        has_date = item.get("due_date") or item.get("datetime")
        assert has_date, f"Event '{item['title']}' has no date"


async def test_classroom_all_items_have_required_fields(client, headers):
    """Every extracted item must have item_id, type, category, title, confidence."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = response.json()
    for item in body["items"]:
        assert item.get("item_id"), f"Item missing item_id: {item}"
        assert item.get("type") in ("note", "reminder", "event"), f"Invalid type: {item}"
        assert item.get("category") in ("insight", "action", "event", "note"), f"Invalid category: {item}"
        assert item.get("title"), f"Item has empty title: {item}"
        assert isinstance(item.get("confidence"), float), f"Item missing confidence: {item}"


# ---------------------------------------------------------------------------
# 3. Session storage and retrieval
# ---------------------------------------------------------------------------

async def test_classroom_session_stored_correctly(client, headers):
    """Session created for classroom ingest should have correct domain and item count."""
    ingest_resp = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    body = ingest_resp.json()
    session_id = body["session_id"]
    item_count = body["count"]

    session_resp = await client.get(f"/session/{session_id}/items")
    assert session_resp.status_code == 200
    session_body = session_resp.json()

    assert session_body["session"]["domain"] == "classroom"
    assert session_body["session"]["item_count"] == item_count
    assert len(session_body["items"]) == item_count


async def test_classroom_session_items_have_correct_domain(client, headers):
    """All items in the classroom session should have domain='classroom'."""
    ingest_resp = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    session_id = ingest_resp.json()["session_id"]

    session_resp = await client.get(f"/session/{session_id}/items")
    items = session_resp.json()["items"]
    for item in items:
        assert item["domain"] == "classroom", (
            f"Item '{item['title']}' has domain='{item['domain']}', expected 'classroom'"
        )
        assert item["session_id"] == session_id


async def test_classroom_latest_session(client, headers):
    """Latest session should reflect the classroom ingest."""
    await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class_instructions.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    resp = await client.get("/sessions/test-device/latest")
    assert resp.status_code == 200
    body = resp.json()
    assert body["session"]["domain"] == "classroom"
    assert body["session"]["item_count"] >= EXPECTED_MIN_ITEMS
    categories = {i["category"] for i in body["items"]}
    assert categories & EXPECTED_CATEGORIES, (
        f"No expected categories in latest session. Got: {categories}"
    )


# ---------------------------------------------------------------------------
# 4. Classroom does not bleed into other domains
# ---------------------------------------------------------------------------

async def test_classroom_domain_isolated_from_sports(client, headers):
    """A classroom session should not be returned when querying a soccer session."""
    # Ingest soccer first
    soccer_resp = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("soccer.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    soccer_session_id = soccer_resp.json()["session_id"]

    # Ingest classroom second
    classroom_resp = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("class.wav", io.BytesIO(make_wav()), "audio/wav")},
    )
    classroom_session_id = classroom_resp.json()["session_id"]

    assert soccer_session_id != classroom_session_id

    # Items in soccer session should not have domain='classroom'
    soccer_items_resp = await client.get(f"/session/{soccer_session_id}/items")
    for item in soccer_items_resp.json()["items"]:
        assert item["domain"] == "sports", (
            f"Soccer session contains item with domain='{item['domain']}'"
        )

    # Items in classroom session should not have domain='sports'
    class_items_resp = await client.get(f"/session/{classroom_session_id}/items")
    for item in class_items_resp.json()["items"]:
        assert item["domain"] == "classroom", (
            f"Classroom session contains item with domain='{item['domain']}'"
        )


# ---------------------------------------------------------------------------
# 5. Domain config
# ---------------------------------------------------------------------------

async def test_classroom_domain_config(client):
    """Classroom domain config should have correct metadata."""
    resp = await client.get("/domains")
    domains = {d["id"]: d for d in resp.json()}
    assert "classroom" in domains
    classroom = domains["classroom"]
    assert classroom["icon"] == "📚"
    assert classroom["name"] == "Classroom"
    assert "insight" in classroom["categories"]
    assert "action" in classroom["categories"]
    assert "event" in classroom["categories"]
