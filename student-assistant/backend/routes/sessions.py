import logging

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse

from services.db import (
    get_sessions_for_device,
    get_session_by_id,
    get_items_for_session,
)

router = APIRouter()
logger = logging.getLogger(__name__)


@router.get("/sessions/{device_id}/latest")
async def get_latest_session(device_id: str):
    """Return the most recent session with all its items."""
    try:
        sessions = await get_sessions_for_device(device_id, limit=1)
        if not sessions:
            raise HTTPException(status_code=404, detail="No sessions found")
        session = sessions[0]
        items = await get_items_for_session(session["id"])
        return {"session": session, "items": items}
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Failed to get latest session: {e}", exc_info=True)
        return JSONResponse(status_code=500, content={"error": "Failed to retrieve session"})


@router.get("/sessions/{device_id}")
async def list_sessions(device_id: str, limit: int = 20):
    """Return recent sessions for a device (no items, just metadata)."""
    try:
        sessions = await get_sessions_for_device(device_id, limit=min(limit, 50))
        return sessions
    except Exception as e:
        logger.error(f"Failed to list sessions: {e}", exc_info=True)
        return JSONResponse(status_code=500, content={"error": "Failed to retrieve sessions"})


@router.get("/session/{session_id}/items")
async def get_session_items(session_id: str):
    """Return all items belonging to a specific session."""
    try:
        session = await get_session_by_id(session_id)
        if not session:
            raise HTTPException(status_code=404, detail="Session not found")
        items = await get_items_for_session(session_id)
        return {"session": session, "items": items}
    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Failed to get session items: {e}", exc_info=True)
        return JSONResponse(status_code=500, content={"error": "Failed to retrieve session items"})
