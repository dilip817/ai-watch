"""Tests for session endpoints."""
import pytest
import io

pytestmark = pytest.mark.asyncio


async def _ingest(client, headers, silent_wav, domain="sports"):
    """Helper to ingest audio and return the response body."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": domain},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    assert response.status_code == 200
    return response.json()


async def test_sessions_list(client, headers, silent_wav):
    """GET /sessions/{device_id} should return a list after ingest."""
    ingest_body = await _ingest(client, headers, silent_wav, "sports")
    device_id = "test-device"

    response = await client.get(f"/sessions/{device_id}", headers=headers)
    assert response.status_code == 200
    sessions = response.json()
    assert isinstance(sessions, list)
    assert len(sessions) >= 1

    # Most recent session should match what we just ingested
    latest = sessions[0]
    assert latest["id"] == ingest_body["session_id"]
    assert latest["domain"] == "sports"
    assert latest["item_count"] > 0


async def test_sessions_latest(client, headers, silent_wav):
    """GET /sessions/{device_id}/latest should return session with items."""
    await _ingest(client, headers, silent_wav, "music")
    device_id = "test-device"

    response = await client.get(f"/sessions/{device_id}/latest", headers=headers)
    assert response.status_code == 200
    body = response.json()
    assert "session" in body
    assert "items" in body
    assert isinstance(body["items"], list)
    assert len(body["items"]) > 0
    assert body["session"]["domain"] == "music"


async def test_session_items(client, headers, silent_wav):
    """GET /session/{session_id}/items should return items for that session."""
    ingest_body = await _ingest(client, headers, silent_wav, "classroom")
    session_id = ingest_body["session_id"]

    response = await client.get(f"/session/{session_id}/items", headers=headers)
    assert response.status_code == 200
    body = response.json()
    assert "session" in body
    assert "items" in body
    assert body["session"]["id"] == session_id
    assert body["session"]["domain"] == "classroom"
    for item in body["items"]:
        assert item["session_id"] == session_id
        assert item["domain"] == "classroom"


async def test_sessions_latest_not_found(client, headers):
    """GET /sessions/nonexistent-device/latest should return 404."""
    response = await client.get("/sessions/nonexistent-device-xyz/latest")
    assert response.status_code == 404


async def test_session_items_not_found(client, headers):
    """GET /session/bad-id/items should return 404."""
    response = await client.get("/session/00000000-0000-0000-0000-000000000000/items")
    assert response.status_code == 404


async def test_multiple_sessions_preserved(client, headers, silent_wav):
    """Ingesting twice should create two distinct sessions."""
    body1 = await _ingest(client, headers, silent_wav, "sports")
    body2 = await _ingest(client, headers, silent_wav, "music")

    assert body1["session_id"] != body2["session_id"]

    response = await client.get("/sessions/test-device", headers=headers)
    sessions = response.json()
    session_ids = {s["id"] for s in sessions}
    assert body1["session_id"] in session_ids
    assert body2["session_id"] in session_ids
