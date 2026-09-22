"""Tests for the /ingest endpoint with domain support."""
import pytest
import pytest_asyncio
import io


pytestmark = pytest.mark.asyncio


async def test_ingest_returns_items(client, headers, silent_wav):
    """POST /ingest should return items and a session_id."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["status"] in ("ok", "confirm_needed")
    assert body["count"] > 0
    assert len(body["items"]) > 0
    assert body["session_id"] is not None
    assert body["domain"] == "sports"


async def test_ingest_soccer_domain(client, headers, silent_wav):
    """Soccer domain should produce insight/action/event categories."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    categories = {item["category"] for item in body["items"]}
    assert categories & {"insight", "action", "event"}


async def test_ingest_music_domain(client, headers, silent_wav):
    """Music domain should produce items with music-related categories."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "music"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    assert body["domain"] == "music"
    assert body["count"] > 0


async def test_ingest_classroom_domain(client, headers, silent_wav):
    """Classroom domain should produce items."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "classroom"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    assert body["domain"] == "classroom"
    assert body["count"] > 0


async def test_ingest_unknown_domain_falls_back_to_general(client, headers, silent_wav):
    """An unknown domain header should fall back to 'general'."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "rugby"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    assert body["domain"] == "general"


async def test_ingest_default_domain_is_general(client, headers, silent_wav):
    """Omitting x-domain header should default to 'general'."""
    response = await client.post(
        "/ingest",
        headers=headers,
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    assert body["domain"] == "general"


async def test_ingest_items_have_required_fields(client, headers, silent_wav):
    """Each item in the response should have required fields."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "sports"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    for item in body["items"]:
        assert "item_id" in item
        assert "type" in item
        assert "category" in item
        assert "title" in item
        assert "confidence" in item


async def test_ingest_backward_compat_top_level_fields(client, headers, silent_wav):
    """Response should still include top-level item_id/type/title for firmware compat."""
    response = await client.post(
        "/ingest",
        headers={**headers, "x-domain": "general"},
        files={"audio": ("test.wav", io.BytesIO(silent_wav), "audio/wav")},
    )
    body = response.json()
    assert "item_id" in body
    assert "type" in body
    assert "title" in body
