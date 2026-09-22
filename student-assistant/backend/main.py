import os
import logging
from contextlib import asynccontextmanager

from dotenv import load_dotenv
from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel

from models import HealthResponse
from routes.ingest import router as ingest_router
from routes.query import router as query_router
from routes.sessions import router as sessions_router
from routes.voice import router as voice_router
from services.db import init_db, get_items_for_device, delete_item, toggle_item_done, toggle_item_paused
from services.tts import synthesize_speech_wav, SILENT_WAV
from services.domains import get_all_domains

load_dotenv()

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info(f"Starting AI Watch backend (mode={os.getenv('MODE', 'mock')})")
    await init_db()
    yield
    logger.info("Shutting down")


app = FastAPI(title="AI Watch — Multi-Domain Activity Assistant", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(ingest_router)
app.include_router(query_router)
app.include_router(sessions_router)
app.include_router(voice_router)


@app.get("/health", response_model=HealthResponse)
async def health():
    return HealthResponse(
        status="ok",
        mode=os.getenv("MODE", "mock"),
    )


@app.get("/domains")
async def list_domains():
    """Return all supported activity domains with metadata for display."""
    return get_all_domains()


@app.get("/items/{device_id}")
async def get_items(device_id: str):
    try:
        items = await get_items_for_device(device_id)
        return items
    except Exception as e:
        logger.error(f"Failed to get items: {e}", exc_info=True)
        return JSONResponse(
            status_code=500,
            content={"error": "Failed to retrieve items"},
        )


@app.patch("/items/{item_id}/toggle")
async def toggle_done(item_id: str):
    try:
        item = await toggle_item_done(item_id)
        if item is None:
            return JSONResponse(status_code=404, content={"error": "Item not found"})
        return item
    except Exception as e:
        logger.error(f"Failed to toggle item: {e}", exc_info=True)
        return JSONResponse(status_code=500, content={"error": "Failed to toggle item"})


@app.patch("/items/{item_id}/pause")
async def toggle_paused(item_id: str):
    try:
        item = await toggle_item_paused(item_id)
        if item is None:
            return JSONResponse(status_code=404, content={"error": "Item not found"})
        return item
    except Exception as e:
        logger.error(f"Failed to toggle pause: {e}", exc_info=True)
        return JSONResponse(status_code=500, content={"error": "Failed to toggle pause"})


@app.delete("/items/{item_id}")
async def remove_item(item_id: str):
    try:
        deleted = await delete_item(item_id)
        if not deleted:
            return JSONResponse(status_code=404, content={"error": "Item not found"})
        return {"status": "deleted", "id": item_id}
    except Exception as e:
        logger.error(f"Failed to delete item: {e}", exc_info=True)
        return JSONResponse(status_code=500, content={"error": "Failed to delete item"})


class TtsRequest(BaseModel):
    text: str


@app.post("/tts")
async def tts_endpoint(body: TtsRequest):
    """Lightweight TTS endpoint — accepts JSON {text}, returns 16kHz mono WAV."""
    text = (body.text or "").strip()
    if not text:
        return Response(content=SILENT_WAV, media_type="audio/wav")

    try:
        logger.info(f"TTS request: {text!r}")
        wav_data = await synthesize_speech_wav(text)
        logger.info(f"TTS response: {len(wav_data)} bytes WAV")
        return Response(content=wav_data, media_type="audio/wav")
    except Exception as e:
        logger.error(f"TTS failed: {e}", exc_info=True)
        return Response(content=SILENT_WAV, media_type="audio/wav")
