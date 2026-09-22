import re
from datetime import datetime, timedelta


def parse_date_range(text: str) -> tuple[str, str]:
    """Parse natural language date references into (start_iso, end_iso).
    Pure regex — no LLM calls. Default: next 7 days."""
    text = text.lower().strip()
    today = datetime.now().replace(hour=0, minute=0, second=0, microsecond=0)

    if re.search(r"\btoday\b", text):
        return today.strftime("%Y-%m-%d"), today.strftime("%Y-%m-%d")

    if re.search(r"\btomorrow\b", text):
        t = today + timedelta(days=1)
        return t.strftime("%Y-%m-%d"), t.strftime("%Y-%m-%d")

    if re.search(r"\byesterday\b", text):
        y = today - timedelta(days=1)
        return y.strftime("%Y-%m-%d"), y.strftime("%Y-%m-%d")

    if re.search(r"\bthis\s+week\b", text):
        start = today - timedelta(days=today.weekday())
        end = start + timedelta(days=6)
        return start.strftime("%Y-%m-%d"), end.strftime("%Y-%m-%d")

    if re.search(r"\bnext\s+week\b", text):
        start = today + timedelta(days=(7 - today.weekday()))
        end = start + timedelta(days=6)
        return start.strftime("%Y-%m-%d"), end.strftime("%Y-%m-%d")

    if re.search(r"\bthis\s+weekend\b", text):
        days_until_sat = (5 - today.weekday()) % 7
        sat = today + timedelta(days=days_until_sat)
        sun = sat + timedelta(days=1)
        return sat.strftime("%Y-%m-%d"), sun.strftime("%Y-%m-%d")

    if re.search(r"\bthis\s+month\b", text):
        start = today.replace(day=1)
        if today.month == 12:
            end = today.replace(year=today.year + 1, month=1, day=1) - timedelta(days=1)
        else:
            end = today.replace(month=today.month + 1, day=1) - timedelta(days=1)
        return start.strftime("%Y-%m-%d"), end.strftime("%Y-%m-%d")

    # Named weekdays: "on Monday", "this Tuesday", "next Friday"
    days_of_week = {
        "monday": 0, "tuesday": 1, "wednesday": 2, "thursday": 3,
        "friday": 4, "saturday": 5, "sunday": 6,
    }
    for day_name, day_num in days_of_week.items():
        if re.search(rf"\b{day_name}\b", text):
            days_ahead = (day_num - today.weekday()) % 7
            if days_ahead == 0:
                days_ahead = 7  # Next occurrence
            target = today + timedelta(days=days_ahead)
            return target.strftime("%Y-%m-%d"), target.strftime("%Y-%m-%d")

    # Default: next 7 days
    end = today + timedelta(days=7)
    return today.strftime("%Y-%m-%d"), end.strftime("%Y-%m-%d")
