import os
import logging
import itertools

logger = logging.getLogger(__name__)

MOCK_TRANSCRIPTS = itertools.cycle([
    "Remind me to finish my math homework by tomorrow",
    "I have a science fair on Wednesday at 10am in the school gym",
    "Note to self: ideas for my book report on Charlotte's Web",
    "Don't forget spelling test on Friday",
    "Soccer practice is Thursday at 4:30 at the sports field",
    "Remember to bring my permission slip for the field trip next Monday",
])


async def transcribe_audio(audio_bytes: bytes, filename: str = "audio.wav") -> str:
    mode = os.getenv("MODE", "mock")
    if mode == "mock":
        transcript = next(MOCK_TRANSCRIPTS)
        logger.info(f"[mock] STT → {transcript!r}")
        return transcript

    import openai

    api_key = os.getenv("OPENAI_API_KEY")
    if not api_key:
        logger.error("OPENAI_API_KEY not set, cannot transcribe")
        return ""

    client = openai.AsyncOpenAI(api_key=api_key)
    try:
        response = await client.audio.transcriptions.create(
            model="whisper-1",
            file=(filename, audio_bytes),
            response_format="text",
        )
        transcript = response.strip()
        logger.info(f"STT → {transcript!r}")
        return transcript
    except Exception as e:
        logger.error(f"Whisper STT failed: {e}", exc_info=True)
        raise
