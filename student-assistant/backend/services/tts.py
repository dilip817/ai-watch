import os
import io
import struct
import logging

logger = logging.getLogger(__name__)

# Valid MPEG Audio Layer 3 frame: 128kbps, 44100Hz, stereo, padded
_FRAME_HEADER = bytes([0xFF, 0xFB, 0x90, 0x04])
_FRAME_BODY = b'\x00' * 414
_SILENT_FRAME = _FRAME_HEADER + _FRAME_BODY
SILENT_MP3 = _SILENT_FRAME * 20

# Edge TTS voice — natural-sounding, free
EDGE_VOICE = os.getenv("EDGE_TTS_VOICE", "en-US-AriaNeural")


async def synthesize_speech(text: str) -> bytes:
    mode = os.getenv("MODE", "mock")
    if mode == "mock":
        return SILENT_MP3

    # Try Edge TTS (free, no API key needed)
    try:
        import edge_tts

        communicate = edge_tts.Communicate(text, EDGE_VOICE)
        buf = io.BytesIO()
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                buf.write(chunk["data"])
        audio_data = buf.getvalue()
        if audio_data:
            logger.info(f"Edge TTS: {len(audio_data)} bytes for '{text[:50]}...'")
            return audio_data
        logger.warning("Edge TTS returned empty audio")
    except Exception as e:
        logger.error(f"Edge TTS failed: {e}", exc_info=True)

    # Fallback: ElevenLabs (if configured)
    import httpx

    api_key = os.getenv("ELEVENLABS_API_KEY")
    if api_key:
        voice_id = os.getenv("ELEVENLABS_VOICE_ID", "EXAVITQu4vr4xnSDxMaL")
        url = f"https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
        try:
            async with httpx.AsyncClient(timeout=10.0) as client:
                resp = await client.post(
                    url,
                    headers={
                        "xi-api-key": api_key,
                        "Content-Type": "application/json",
                        "Accept": "audio/mpeg",
                    },
                    json={
                        "text": text,
                        "model_id": "eleven_turbo_v2_5",
                        "voice_settings": {"stability": 0.5, "similarity_boost": 0.75},
                    },
                )
                if resp.status_code == 200:
                    return resp.content
                logger.error(f"ElevenLabs TTS error {resp.status_code}: {resp.text[:200]}")
        except Exception as e:
            logger.error(f"ElevenLabs TTS failed: {e}")

    # Last resort: silent MP3
    return SILENT_MP3


# Target format for ESP32 playback: 16kHz, 16-bit, mono PCM WAV
PLAYBACK_RATE = 16000
PLAYBACK_CHANNELS = 1
PLAYBACK_BITS = 16


def _make_wav_header(data_size: int) -> bytes:
    """Build a minimal WAV header for raw PCM data."""
    byte_rate = PLAYBACK_RATE * PLAYBACK_CHANNELS * (PLAYBACK_BITS // 8)
    block_align = PLAYBACK_CHANNELS * (PLAYBACK_BITS // 8)
    return struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + data_size,
        b"WAVE",
        b"fmt ",
        16,                  # fmt chunk size
        1,                   # PCM format
        PLAYBACK_CHANNELS,
        PLAYBACK_RATE,
        byte_rate,
        block_align,
        PLAYBACK_BITS,
        b"data",
        data_size,
    )


# Minimal silent WAV (0.5s of silence)
_SILENT_SAMPLES = PLAYBACK_RATE // 2
SILENT_WAV = _make_wav_header(_SILENT_SAMPLES * 2) + b"\x00" * (_SILENT_SAMPLES * 2)


async def synthesize_speech_wav(text: str) -> bytes:
    """Synthesize speech and return as 16kHz 16-bit mono WAV."""
    mp3_data = await synthesize_speech(text)

    # If it's our silent MP3 placeholder, return silent WAV
    if mp3_data == SILENT_MP3:
        return SILENT_WAV

    try:
        from pydub import AudioSegment

        audio = AudioSegment.from_mp3(io.BytesIO(mp3_data))
        audio = audio.set_frame_rate(PLAYBACK_RATE).set_channels(PLAYBACK_CHANNELS).set_sample_width(2)
        wav_buf = io.BytesIO()
        audio.export(wav_buf, format="wav")
        wav_data = wav_buf.getvalue()
        logger.info(f"MP3→WAV: {len(mp3_data)} → {len(wav_data)} bytes")
        return wav_data
    except ImportError:
        logger.error("pydub not installed — falling back to ffmpeg subprocess")
    except Exception as e:
        logger.error(f"pydub conversion failed: {e}")

    # Fallback: use ffmpeg directly
    try:
        import subprocess
        result = subprocess.run(
            ["ffmpeg", "-i", "pipe:0", "-ar", str(PLAYBACK_RATE), "-ac", "1",
             "-f", "wav", "-acodec", "pcm_s16le", "pipe:1"],
            input=mp3_data, capture_output=True, timeout=10,
        )
        if result.returncode == 0 and len(result.stdout) > 44:
            logger.info(f"ffmpeg MP3→WAV: {len(mp3_data)} → {len(result.stdout)} bytes")
            return result.stdout
    except Exception as e:
        logger.error(f"ffmpeg conversion failed: {e}")

    return SILENT_WAV
