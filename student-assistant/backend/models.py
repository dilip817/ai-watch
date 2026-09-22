from pydantic import BaseModel
from typing import Optional, List


class Item(BaseModel):
    id: str
    device_id: str
    type: str  # reminder | event | note
    category: Optional[str] = None  # insight | action | event | note
    domain: Optional[str] = "general"  # soccer | music | classroom | general
    session_id: Optional[str] = None
    title: str
    body: Optional[str] = None
    datetime: Optional[str] = None
    due_date: Optional[str] = None
    recurrence: Optional[str] = None  # daily | weekly | every practice | null
    subject: Optional[str] = None
    location: Optional[str] = None
    confidence: float = 1.0
    synced: int = 0
    sync_pending: int = 0
    done: int = 0
    paused: int = 0
    created_at: int = 0


class Session(BaseModel):
    id: str
    device_id: str
    domain: str
    title: Optional[str] = None
    transcript: Optional[str] = None
    item_count: int = 0
    created_at: int = 0


class SessionWithItems(BaseModel):
    session: Session
    items: List[Item]


class IngestItemResult(BaseModel):
    item_id: str
    type: str
    category: Optional[str] = None
    title: str
    due_date: Optional[str] = None
    datetime: Optional[str] = None
    recurrence: Optional[str] = None
    confidence: float = 1.0


class IngestResponse(BaseModel):
    status: str  # ok | confirm_needed | empty | error
    # Top-level fields for backward compatibility (first item)
    item_id: Optional[str] = None
    type: Optional[str] = None
    title: Optional[str] = None
    due_date: Optional[str] = None
    datetime: Optional[str] = None
    confidence: Optional[float] = None
    transcript: Optional[str] = None
    # Session fields
    session_id: Optional[str] = None
    domain: Optional[str] = None
    # Multi-item fields
    items: List[IngestItemResult] = []
    count: int = 0


class HealthResponse(BaseModel):
    status: str
    mode: str


class DomainInfo(BaseModel):
    id: str
    name: str
    icon: str
    color: str
    categories: List[str]
