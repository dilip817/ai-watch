# Student Assistant

Voice-first homework and schedule tracker for kids, built on the Waveshare ESP32-S3 2.06" AMOLED.

## Architecture

```
[ESP32-S3 Device] --audio--> [Python Backend] --STT/Intent/TTS--> [APIs]
                                    |
                                [SQLite DB]
```

A child speaks into the device to record homework reminders, calendar events, and notes. The backend processes audio through STT → intent extraction → DB storage. The child can ask questions and hear spoken answers back.

## Hardware

- **Board**: Waveshare ESP32-S3 Touch AMOLED 2.06"
- **Display**: CO5300 AMOLED, 410×502, QSPI @80MHz
- **Audio**: ES8311 DAC + ES7210 ADC, I2S on GPIO 41/45/40/42
- **Touch**: FT3168, I2C on GPIO 15/14
- **SD Card**: SDMMC on GPIO 2/1/3
- **Power**: AXP2101 PMIC

## Quick Start

### Backend (mock mode — no API keys needed)

```bash
cd backend
cp .env.example .env
pip install -r requirements.txt
uvicorn main:app --reload --port 8000
```

### Web UI

```bash
cd ui
python -m http.server 3000
# Open http://localhost:3000
```

### Simulator

```bash
cd simulator
python device.py --demo      # 3 ingests + 1 query
python device.py --items     # List all items
python device.py --record 5  # Record 5s from mic
python device.py --query     # Ask a question
```

### Firmware

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

## Project Structure

```
student-assistant/
├── backend/          Python FastAPI backend
│   ├── main.py       App entry, routes
│   ├── models.py     Pydantic models
│   ├── routes/       /ingest and /query endpoints
│   └── services/     STT, intent, TTS, DB, date parser
├── simulator/        ESP32 device simulator
├── ui/               Browser UI (single HTML file)
└── firmware/         ESP-IDF firmware
    └── main/         C source files
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check, returns mode |
| `/ingest` | POST | Upload audio → STT → intent → save |
| `/query` | POST | Upload question → fetch items → TTS response |
| `/items/{device_id}` | GET | List all items for device |

## Mock Mode

Set `MODE=mock` in `.env` (default). All API calls return hardcoded cycling responses. No API keys required. Full pipeline runs end-to-end.
