"""Shared test fixtures for AI Watch backend tests."""
import os
import tempfile
import pytest
import pytest_asyncio
from httpx import AsyncClient, ASGITransport

# Force mock mode so tests never need real API keys
os.environ["MODE"] = "mock"
os.environ["DEVICE_SECRET"] = "change-me-32-chars"

# Use a temp file DB per test session (aiosqlite :memory: doesn't share across connections)
_tmp_db = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
_tmp_db.close()
os.environ["DB_PATH"] = _tmp_db.name

import services.db as db_module

# Patch db module so it reads the env var on import
db_module.DB_PATH = _tmp_db.name

from main import app
from services.db import init_db


@pytest_asyncio.fixture(autouse=True)
async def setup_db():
    """Initialize the database before each test."""
    # Re-read DB_PATH in case it was patched
    db_module.DB_PATH = os.environ["DB_PATH"]
    await init_db()
    yield


@pytest_asyncio.fixture
async def client(setup_db):
    """Async HTTP client pointed at the FastAPI app."""
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as ac:
        yield ac


@pytest.fixture
def headers():
    """Default device headers (mock mode skips HMAC check)."""
    return {
        "x-device-id": "test-device",
        "x-device-token": "mock-token",
        "x-device-ts": "1700000000",
    }


@pytest.fixture
def silent_wav() -> bytes:
    """Minimal valid WAV file (44-byte header + 0 samples)."""
    import struct
    data_size = 0
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + data_size, b"WAVE",
        b"fmt ", 16, 1, 1, 16000, 32000, 2, 16,
        b"data", data_size,
    )
    return header
