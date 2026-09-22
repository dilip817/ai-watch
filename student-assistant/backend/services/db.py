import os
import aiosqlite
import logging
import time
import uuid
from datetime import datetime, timedelta
from typing import Optional

logger = logging.getLogger(__name__)

DB_PATH = os.getenv("DB_PATH", "student_assistant.db")

CREATE_SESSIONS_TABLE = """
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    domain TEXT NOT NULL DEFAULT 'general',
    title TEXT,
    transcript TEXT,
    item_count INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL
)
"""

CREATE_SESSIONS_INDEX = """
CREATE INDEX IF NOT EXISTS idx_sessions_device
ON sessions (device_id, created_at)
"""

CREATE_ITEMS_TABLE = """
CREATE TABLE IF NOT EXISTS items (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    type TEXT NOT NULL,
    category TEXT DEFAULT 'note',
    domain TEXT DEFAULT 'general',
    session_id TEXT,
    title TEXT NOT NULL,
    body TEXT,
    datetime TEXT,
    due_date TEXT,
    recurrence TEXT,
    subject TEXT,
    location TEXT,
    confidence REAL DEFAULT 1.0,
    synced INTEGER DEFAULT 0,
    sync_pending INTEGER DEFAULT 0,
    done INTEGER DEFAULT 0,
    paused INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL
)
"""

CREATE_ITEMS_INDEX = """
CREATE INDEX IF NOT EXISTS idx_items_device_dates
ON items (device_id, due_date, datetime)
"""

# New column migrations for items table
ITEMS_MIGRATIONS = [
    ("done", "INTEGER DEFAULT 0"),
    ("paused", "INTEGER DEFAULT 0"),
    ("category", "TEXT DEFAULT 'note'"),
    ("domain", "TEXT DEFAULT 'general'"),
    ("session_id", "TEXT"),
    ("recurrence", "TEXT"),
]


async def get_db() -> aiosqlite.Connection:
    db = await aiosqlite.connect(DB_PATH)
    db.row_factory = aiosqlite.Row
    return db


async def init_db():
    db = await get_db()
    try:
        await db.execute(CREATE_SESSIONS_TABLE)
        await db.execute(CREATE_SESSIONS_INDEX)
        await db.execute(CREATE_ITEMS_TABLE)
        await db.execute(CREATE_ITEMS_INDEX)
        # Migrations: add columns if missing (existing DBs)
        for col, col_def in ITEMS_MIGRATIONS:
            try:
                await db.execute(f"ALTER TABLE items ADD COLUMN {col} {col_def}")
                logger.info(f"Migrated: added '{col}' column to items table")
            except Exception:
                pass  # Column already exists
        await db.commit()
    finally:
        await db.close()


# ---------------------------------------------------------------------------
# Sessions
# ---------------------------------------------------------------------------

async def create_session(
    device_id: str,
    domain: str,
    transcript: Optional[str] = None,
    title: Optional[str] = None,
) -> str:
    """Create a new session and return its ID."""
    session_id = str(uuid.uuid4())
    db = await get_db()
    try:
        await db.execute(
            """INSERT INTO sessions (id, device_id, domain, title, transcript, item_count, created_at)
               VALUES (?, ?, ?, ?, ?, 0, ?)""",
            (session_id, device_id, domain, title, transcript, int(time.time())),
        )
        await db.commit()
        logger.info(f"Created session {session_id}: domain={domain}, device={device_id}")
    finally:
        await db.close()
    return session_id


async def update_session_item_count(session_id: str, count: int):
    """Update the item count on a session after items are inserted."""
    db = await get_db()
    try:
        await db.execute(
            "UPDATE sessions SET item_count = ? WHERE id = ?",
            (count, session_id),
        )
        await db.commit()
    finally:
        await db.close()


async def get_sessions_for_device(device_id: str, limit: int = 20) -> list[dict]:
    """Return recent sessions for a device, newest first."""
    db = await get_db()
    try:
        cursor = await db.execute(
            "SELECT * FROM sessions WHERE device_id = ? ORDER BY created_at DESC LIMIT ?",
            (device_id, limit),
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]
    finally:
        await db.close()


async def get_session_by_id(session_id: str) -> Optional[dict]:
    """Return a single session by ID."""
    db = await get_db()
    try:
        cursor = await db.execute("SELECT * FROM sessions WHERE id = ?", (session_id,))
        row = await cursor.fetchone()
        return dict(row) if row else None
    finally:
        await db.close()


async def get_items_for_session(session_id: str) -> list[dict]:
    """Return all items belonging to a session."""
    db = await get_db()
    try:
        cursor = await db.execute(
            "SELECT * FROM items WHERE session_id = ? ORDER BY created_at ASC",
            (session_id,),
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]
    finally:
        await db.close()


# ---------------------------------------------------------------------------
# Items
# ---------------------------------------------------------------------------

async def insert_item(
    device_id: str,
    type_: str,
    title: str,
    body: Optional[str] = None,
    datetime_: Optional[str] = None,
    due_date: Optional[str] = None,
    subject: Optional[str] = None,
    location: Optional[str] = None,
    confidence: float = 1.0,
    category: Optional[str] = None,
    domain: str = "general",
    session_id: Optional[str] = None,
    recurrence: Optional[str] = None,
) -> str:
    item_id = str(uuid.uuid4())
    db = await get_db()
    try:
        await db.execute(
            """INSERT INTO items (id, device_id, type, category, domain, session_id,
               title, body, datetime, due_date, recurrence, subject, location,
               confidence, created_at)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
            (
                item_id,
                device_id,
                type_,
                category or type_,  # default category matches type
                domain,
                session_id,
                title,
                body,
                datetime_,
                due_date,
                recurrence,
                subject,
                location,
                confidence,
                int(time.time()),
            ),
        )
        await db.commit()
        logger.info(f"Inserted item {item_id}: {type_}/{category} — {title}")
    finally:
        await db.close()
    return item_id


async def get_items_for_device(device_id: str) -> list[dict]:
    db = await get_db()
    try:
        cursor = await db.execute(
            "SELECT * FROM items WHERE device_id = ? ORDER BY created_at DESC",
            (device_id,),
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]
    finally:
        await db.close()


async def delete_item(item_id: str) -> bool:
    """Delete an item by ID. Returns True if deleted."""
    db = await get_db()
    try:
        cursor = await db.execute("DELETE FROM items WHERE id = ?", (item_id,))
        await db.commit()
        deleted = cursor.rowcount > 0
        if deleted:
            logger.info(f"Deleted item {item_id}")
        return deleted
    finally:
        await db.close()


async def toggle_item_done(item_id: str) -> Optional[dict]:
    """Toggle the done status of an item. Returns updated item or None."""
    db = await get_db()
    try:
        await db.execute(
            "UPDATE items SET done = CASE WHEN done = 1 THEN 0 ELSE 1 END WHERE id = ?",
            (item_id,),
        )
        await db.commit()
        cursor = await db.execute("SELECT * FROM items WHERE id = ?", (item_id,))
        row = await cursor.fetchone()
        if row:
            logger.info(f"Toggled item {item_id} done={dict(row)['done']}")
            return dict(row)
        return None
    finally:
        await db.close()


async def toggle_item_paused(item_id: str) -> Optional[dict]:
    """Toggle the paused status of an item. Returns updated item or None."""
    db = await get_db()
    try:
        await db.execute(
            "UPDATE items SET paused = CASE WHEN paused = 1 THEN 0 ELSE 1 END WHERE id = ?",
            (item_id,),
        )
        await db.commit()
        cursor = await db.execute("SELECT * FROM items WHERE id = ?", (item_id,))
        row = await cursor.fetchone()
        if row:
            logger.info(f"Toggled item {item_id} paused={dict(row)['paused']}")
            return dict(row)
        return None
    finally:
        await db.close()


async def get_items_in_range(
    device_id: str, start_date: str, end_date: str
) -> list[dict]:
    """Fetch items matching a date range.

    Matches items where:
      - due_date falls within [start_date, end_date], OR
      - datetime (truncated to date) falls within range, OR
      - type is 'note' (notes have no dates, always included in results)
    """
    db = await get_db()
    try:
        cursor = await db.execute(
            """SELECT * FROM items WHERE device_id = ?
               AND (
                   (due_date IS NOT NULL AND due_date BETWEEN ? AND ?)
                   OR (datetime IS NOT NULL AND substr(datetime, 1, 10) BETWEEN ? AND ?)
                   OR (due_date IS NULL AND datetime IS NULL AND type = 'note')
               )
               ORDER BY COALESCE(due_date, substr(datetime, 1, 10), '9999-12-31') ASC""",
            (device_id, start_date, end_date, start_date, end_date),
        )
        rows = await cursor.fetchall()
        return [dict(row) for row in rows]
    finally:
        await db.close()
