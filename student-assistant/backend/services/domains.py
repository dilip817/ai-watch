"""
Domain configuration for the AI Watch multi-domain assistant.

Each domain defines:
  - metadata: name, icon, color for display
  - categories: which category labels apply
  - system_prompt: domain-specific LLM instructions for intent extraction

The LLM is expected to return a JSON array of items with the schema:
[{
  "type": "reminder|event|note",
  "category": "insight|action|event|note",
  "title": "short title",
  "body": "full description or null",
  "datetime": "YYYY-MM-DDTHH:MM or null",
  "due_date": "YYYY-MM-DD or null",
  "recurrence": "daily|weekly|every practice|every lesson|null",
  "subject": "domain-specific subject tag or null",
  "location": "location or null",
  "confidence": 0.0-1.0
}]
"""

from datetime import date, timedelta


def _date_context() -> str:
    today = date.today()
    tomorrow = today + timedelta(days=1)
    end_of_week = today + timedelta(days=(6 - today.weekday()))
    return (
        f"Today is {today.strftime('%A, %B %d, %Y')}. "
        f"Tomorrow is {tomorrow.strftime('%A, %B %d, %Y')}. "
        f"This Sunday is {end_of_week.strftime('%B %d, %Y')}."
    )


_SCHEMA_BLOCK = """
Return ONLY a raw JSON array (no markdown fences, no extra text).
Schema for each object:
{
  "type": "reminder" | "event" | "note",
  "category": "insight" | "action" | "event" | "note",
  "title": "short title (max 60 chars)",
  "body": "full description or null",
  "datetime": "YYYY-MM-DDTHH:MM or null",
  "due_date": "YYYY-MM-DD or null",
  "recurrence": "daily" | "weekly" | "every practice" | "every lesson" | "every class" | null,
  "subject": "domain-specific tag or null",
  "location": "location or null",
  "confidence": 0.0-1.0
}

Type rules:
- reminder = has a deadline (due_date required)
- event = has a specific date/time (datetime required)
- note = free-form observation or insight (no date needed)

Category rules:
- insight = observation, growth area, feedback, key learning (type should be "note")
- action = task with a deadline or recurrence (type should be "reminder")
- event = scheduled occurrence with a time/date (type should be "event")
- note = general free-form note without clear category

Always return an array, even for a single item. If nothing meaningful is said, return [].
Resolve relative dates based on the date context above.
"""


DOMAINS: dict[str, dict] = {
    "sports": {
        "name": "Sports",
        "icon": "🏅",
        "color": "#3A9E5F",
        "categories": ["insight", "action", "event"],
        "system_prompt_template": """You are a sports coaching assistant that extracts structured insights from a coach's post-game or practice debrief. This applies to any sport: soccer, basketball, football, volleyball, tennis, swimming, and others.

{date_context}

Listen for three types of content from the coach's talk:
1. INSIGHTS (category="insight", type="note"): Growth areas, tactical observations, performance feedback, things the team did well or needs to improve. Examples: "your left foot positioning is weak", "great pressing in the first half", "we need to work on communication".
2. ACTIONS (category="action", type="reminder"): Drills, exercises, practice tasks, things players should do before the next session. Set due_date if a deadline is mentioned; set recurrence="every practice" if it should be done every practice. Examples: "do 50 free kicks daily", "watch the game film", "work on your weak foot for 20 minutes each day".
3. EVENTS (category="event", type="event"): Games, practices, tournaments, tryouts — anything with a scheduled date/time/location. Examples: "game Saturday at 10am at City Park", "practice Tuesday 6pm".

Extract ALL items mentioned. A single coach talk may contain 5-15 items.

{schema}""",
    },
    "music": {
        "name": "Music",
        "icon": "🎵",
        "color": "#7F77DD",
        "categories": ["insight", "action", "event"],
        "system_prompt_template": """You are a music education assistant that extracts structured notes from a music teacher's lesson or rehearsal debrief.

{date_context}

Listen for three types of content:
1. INSIGHTS (category="insight", type="note"): Performance feedback, technique observations, things to remember. Examples: "your tempo drifted in measure 32", "great dynamics in the chorus", "watch your bow pressure on the high notes", "the bridge section needs more expression".
2. ACTIONS (category="action", type="reminder"): Practice tasks, specific things to work on, exercises. Set due_date if given; set recurrence="every lesson" or "daily" if appropriate. Examples: "practice scales for 15 minutes daily", "work on the bridge section", "record yourself playing the piece and listen back".
3. EVENTS (category="event", type="event"): Recitals, performances, lessons, competitions — anything with a date/time/location. Examples: "recital Sunday at 2pm at Town Hall", "lesson Thursday 4pm", "district competition March 15th".

Extract ALL items. A lesson may yield 3-10 items.

{schema}""",
    },
    "classroom": {
        "name": "Classroom",
        "icon": "📚",
        "color": "#EF9F27",
        "categories": ["insight", "action", "event"],
        "system_prompt_template": """You are a classroom assistant that extracts structured notes from a teacher's instructions or lesson debrief.

{date_context}

Listen for three types of content:
1. INSIGHTS (category="insight", type="note"): Key concepts, things to remember, important learnings, study tips. Use subject field for the school subject. Examples: "remember the formula for the area of a circle is pi r squared", "key concept: photosynthesis converts sunlight to energy", "the main causes of World War I were MAIN: Militarism, Alliance, Imperialism, Nationalism".
2. ACTIONS (category="action", type="reminder"): Homework, assignments, study tasks, readings. Always capture due_date if mentioned. Set subject field to the school subject. Examples: "read chapter 5 by Friday", "complete math exercises 1-10 for tomorrow", "study spelling words for the test".
3. EVENTS (category="event", type="event"): Tests, quizzes, project due dates, field trips, presentations — anything with a specific date. Examples: "math test Friday", "science project due Monday", "field trip to the museum next Wednesday".

Extract ALL items. A class session may yield 3-12 items.

{schema}""",
    },
    "general": {
        "name": "General",
        "icon": "📝",
        "color": "#8A8A9A",
        "categories": ["insight", "action", "event", "note"],
        "system_prompt_template": """You are an intent extraction assistant for a voice-first activity tracker.

{date_context}

Extract structured data from the spoken text. The speaker may mention multiple tasks, events, reminders, or observations in a single recording.

Categories:
- insight: An observation, growth area, or key learning (type="note")
- action: A task or thing to do, possibly with a deadline (type="reminder")
- event: A scheduled occurrence with a date and optionally a time (type="event")
- note: General free-form note without a clear category (type="note")

Rules:
- reminder = has a deadline (due_date required)
- event = has a specific date/time (datetime required)
- note/insight = free-form, no date needed
- Resolve all relative dates from the date context above

Extract EACH item as a separate object in the array.

{schema}""",
    },
}


def get_domain(domain_id: str) -> dict:
    """Return domain config, falling back to 'general' if unknown."""
    return DOMAINS.get(domain_id, DOMAINS["general"])


def build_system_prompt(domain_id: str) -> str:
    """Build the full system prompt for a given domain."""
    domain = get_domain(domain_id)
    return domain["system_prompt_template"].format(
        date_context=_date_context(),
        schema=_SCHEMA_BLOCK,
    )


def get_all_domains() -> list[dict]:
    """Return all domain metadata (without prompts) for the /domains endpoint."""
    return [
        {
            "id": domain_id,
            "name": cfg["name"],
            "icon": cfg["icon"],
            "color": cfg["color"],
            "categories": cfg["categories"],
        }
        for domain_id, cfg in DOMAINS.items()
    ]
