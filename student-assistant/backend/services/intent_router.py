"""
Intent router — classifies voice input as 'save' (new item) or 'query' (question/readback).

Uses a fast heuristic classifier to avoid an extra LLM call.
For ambiguous inputs it defaults to 'query' so the user gets a spoken response
either way (the query path narrates whatever is relevant).
"""

# Words that strongly signal a question or readback request
_QUERY_SIGNALS = [
    "what", "which", "when", "where", "who",
    "do i have", "do i", "have i", "is there",
    "read", "read my", "read all", "tell me", "show me",
    "list", "any events", "any reminders", "any notes",
    "upcoming", "what's", "whats", "how many",
]

# Words that strongly signal saving/recording new content
_SAVE_SIGNALS = [
    "remind me", "remember", "note", "don't forget", "make a note",
    "add", "save", "record", "write down",
    "we covered", "teacher said", "coach said",
    "practice is", "game is", "game on", "game at",
    "meeting", "appointment", "homework",
]


def classify_voice_intent(transcript: str) -> str:
    """
    Returns 'save' or 'query' based on the transcript.

    'save'  → the user is recording a new item (note, reminder, event insight)
    'query' → the user is asking a question or requesting readback of existing items
    """
    if not transcript:
        return "query"

    lower = transcript.lower().strip()

    # Score-based: count signal matches
    query_score = sum(1 for signal in _QUERY_SIGNALS if signal in lower)
    save_score = sum(1 for signal in _SAVE_SIGNALS if signal in lower)

    # Question mark is a strong query signal
    if "?" in transcript:
        query_score += 2

    # First word heuristics
    first_word = lower.split()[0] if lower.split() else ""
    if first_word in {"what", "when", "where", "which", "who", "how", "do", "is", "are", "read", "tell"}:
        query_score += 1
    if first_word in {"remind", "remember", "note", "add", "save"}:
        save_score += 1

    if query_score > save_score:
        return "query"
    if save_score > 0:
        return "save"

    # Default to query — user gets a spoken response either way
    return "query"
