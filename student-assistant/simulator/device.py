#!/usr/bin/env python3
"""Device simulator for the Student Assistant ESP32.

Mimics the ESP32 device: generates HMAC auth, records audio or uses
a silent WAV, POSTs to /ingest and /query, plays back audio responses.

Usage:
    python device.py --demo                  # 3 ingests then a query
    python device.py --record 5              # Record 5s from mic, ingest
    python device.py --file audio.wav        # Ingest from file
    python device.py --query                 # Record question, get audio answer
    python device.py --items                 # Show all DB items
    python device.py --backend http://host:8000  # Custom backend URL
"""

import argparse
import hashlib
import hmac
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

import httpx

DEVICE_ID = "demo-device"
DEVICE_SECRET = os.getenv("DEVICE_SECRET", "change-me-32-chars")
DEFAULT_BACKEND = "http://localhost:8000"


def make_token(device_id: str, secret: str) -> str:
    return hmac.new(secret.encode(), device_id.encode(), hashlib.sha256).hexdigest()


def make_headers() -> dict:
    return {
        "x-device-id": DEVICE_ID,
        "x-device-token": make_token(DEVICE_ID, DEVICE_SECRET),
        "x-device-ts": str(int(time.time())),
    }


def make_silent_wav(duration_s: float = 1.0, sample_rate: int = 16000) -> bytes:
    """Generate a silent WAV file in memory."""
    num_samples = int(sample_rate * duration_s)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(b"\x00\x00" * num_samples)
    return buf.getvalue()


def record_from_mic(duration_s: float) -> bytes:
    """Record from the microphone using sounddevice."""
    try:
        import sounddevice as sd
        import numpy as np
    except ImportError:
        print("sounddevice not installed. Using silent WAV instead.")
        print("  pip install sounddevice numpy")
        return make_silent_wav(duration_s)

    sample_rate = 16000
    print(f"Recording {duration_s}s from microphone...")
    try:
        audio = sd.rec(
            int(duration_s * sample_rate),
            samplerate=sample_rate,
            channels=1,
            dtype="int16",
        )
        sd.wait()
        print("Recording complete.")
    except Exception as e:
        print(f"Mic recording failed: {e}. Using silent WAV.")
        return make_silent_wav(duration_s)

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(audio.tobytes())
    return buf.getvalue()


def load_file(path: str) -> bytes:
    if not os.path.exists(path):
        print(f"File not found: {path}")
        sys.exit(1)
    with open(path, "rb") as f:
        data = f.read()
    print(f"Loaded {len(data)} bytes from {path}")
    return data


def play_mp3(data: bytes):
    """Play MP3 audio using available system player, with proper cleanup."""
    if len(data) < 10:
        print("(Audio response too small to play — likely silent/mock)")
        return

    # Use a proper temp file with cleanup
    tmp_fd, tmp_path = tempfile.mkstemp(suffix=".mp3", prefix="sa_response_")
    try:
        os.write(tmp_fd, data)
        os.close(tmp_fd)

        for player in ["afplay", "mpg123", "ffplay -nodisp -autoexit"]:
            cmd = player.split() + [tmp_path]
            try:
                subprocess.run(cmd, capture_output=True, timeout=15)
                return
            except FileNotFoundError:
                continue
            except subprocess.TimeoutExpired:
                print(f"(Player {player.split()[0]} timed out)")
                continue
        print(f"(No audio player found — saved response to {tmp_path})")
        return  # Don't delete if no player found — user can play manually
    finally:
        # Clean up temp file (unless we want to keep it)
        try:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)
        except OSError:
            pass


def do_ingest(backend: str, audio_bytes: bytes):
    print(f"\n--- INGEST → {backend}/ingest ---")
    headers = make_headers()
    files = {"audio": ("recording.wav", audio_bytes, "audio/wav")}
    try:
        resp = httpx.post(f"{backend}/ingest", headers=headers, files=files, timeout=30)
        if resp.status_code == 200:
            data = resp.json()
            print(json.dumps(data, indent=2))
            return data
        else:
            print(f"Error {resp.status_code}: {resp.text[:500]}")
            return None
    except httpx.TimeoutException:
        print("Error: Request timed out (30s)")
        return None
    except httpx.ConnectError:
        print(f"Error: Cannot connect to {backend}")
        return None
    except Exception as e:
        print(f"Error: {e}")
        return None


def do_query(backend: str, audio_bytes: bytes):
    print(f"\n--- QUERY → {backend}/query ---")
    headers = make_headers()
    files = {"audio": ("question.wav", audio_bytes, "audio/wav")}
    try:
        resp = httpx.post(f"{backend}/query", headers=headers, files=files, timeout=30)
        content_type = resp.headers.get("content-type", "")
        if resp.status_code == 200 and "audio" in content_type:
            print(f"Got audio response: {len(resp.content)} bytes")
            play_mp3(resp.content)
        elif resp.status_code == 200:
            # Unexpected content type but still 200
            print(f"Got non-audio response ({content_type}): {resp.text[:200]}")
        else:
            print(f"Error {resp.status_code}: {resp.text[:500]}")
    except httpx.TimeoutException:
        print("Error: Request timed out (30s)")
    except httpx.ConnectError:
        print(f"Error: Cannot connect to {backend}")
    except Exception as e:
        print(f"Error: {e}")


def do_items(backend: str):
    print(f"\n--- ITEMS → {backend}/items/{DEVICE_ID} ---")
    try:
        resp = httpx.get(f"{backend}/items/{DEVICE_ID}", timeout=10)
        if resp.status_code == 200:
            items = resp.json()
            print(f"\n{len(items)} items for {DEVICE_ID}:\n")
            for item in items:
                badge = {"reminder": "📝", "event": "📅", "note": "📌"}.get(
                    item["type"], "❓"
                )
                due = item.get("due_date") or item.get("datetime") or ""
                conf = f" (conf={item.get('confidence', '?')})" if item.get("confidence") else ""
                print(f"  {badge} [{item['type']:>8}] {item['title']:<30} {due}{conf}")
        else:
            print(f"Error {resp.status_code}: {resp.text[:500]}")
    except Exception as e:
        print(f"Error: {e}")


def do_demo(backend: str):
    print("=== DEMO MODE: 3 ingests + 1 query ===\n")
    for i in range(3):
        print(f"\n[Ingest {i+1}/3]")
        wav = make_silent_wav(2.0)
        result = do_ingest(backend, wav)
        if result:
            print(f"  → {result.get('status')}: {result.get('type')} — {result.get('title')}")
        time.sleep(0.5)

    print("\n[Query: 'What's due this week?']")
    wav = make_silent_wav(2.0)
    do_query(backend, wav)

    print("\n--- Final items ---")
    do_items(backend)

    print("\n=== Demo complete ===")


def main():
    parser = argparse.ArgumentParser(description="Student Assistant Device Simulator")
    parser.add_argument("--record", type=float, metavar="SECS", help="Record from mic")
    parser.add_argument("--file", type=str, metavar="PATH", help="Load audio file")
    parser.add_argument("--query", action="store_true", help="Ask a question")
    parser.add_argument("--items", action="store_true", help="List all items")
    parser.add_argument("--demo", action="store_true", help="Run demo sequence")
    parser.add_argument(
        "--backend", default=DEFAULT_BACKEND, help="Backend URL (default: %(default)s)"
    )
    args = parser.parse_args()

    # Health check
    try:
        resp = httpx.get(f"{args.backend}/health", timeout=5)
        health = resp.json()
        print(f"Backend: {args.backend} — mode={health['mode']}")
    except httpx.ConnectError:
        print(f"Cannot connect to backend at {args.backend}")
        print("Start it with: cd backend && uvicorn main:app --reload --port 8000")
        sys.exit(1)
    except Exception as e:
        print(f"Cannot reach backend at {args.backend}: {e}")
        sys.exit(1)

    if args.demo:
        do_demo(args.backend)
    elif args.items:
        do_items(args.backend)
    elif args.query:
        if args.record:
            wav = record_from_mic(args.record)
        elif args.file:
            wav = load_file(args.file)
        else:
            wav = make_silent_wav(2.0)
        do_query(args.backend, wav)
    else:
        if args.record:
            wav = record_from_mic(args.record)
        elif args.file:
            wav = load_file(args.file)
        else:
            wav = make_silent_wav(2.0)
        do_ingest(args.backend, wav)


if __name__ == "__main__":
    main()
