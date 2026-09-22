# AI Watch — Multi-Domain Activity Assistant

**A voice-first activity assistant for students and young athletes, built on the Waveshare ESP32-S3 2.06" AMOLED watch.**

A soccer coach gives a post-game debrief — the watch records, transcribes, and extracts structured **insights** (growth areas), **actions** (drills to practice), and **calendar events** (upcoming games). The same works for music lessons, classroom instructions, or any other activity. Tap the domain badge on the home screen to switch context.

---

## Domains

| Domain | Use Case | Example Items Extracted |
|---|---|---|
| ⚽ Soccer | Coach post-game/practice talk | "Work on left foot" (insight), "50 free kicks daily" (action), "Game Saturday 10am" (event) |
| 🎵 Music | Teacher lesson/rehearsal debrief | "Tempo drift in measure 32" (insight), "Practice scales 15min daily" (action), "Recital Sunday 2pm" (event) |
| 📚 Classroom | Teacher instructions | "Area = π r²" (insight), "Read chapter 5 by Friday" (action), "Math test Friday" (event) |
| 📝 General | Any other activity | Free-form extraction across all categories |

---

## Item Categories

| Category | Maps to | Color | Description |
|---|---|---|---|
| insight | note | Amber | Observations, growth areas, key learnings |
| action | reminder | Purple | Tasks with deadlines or recurrence |
| event | event | Teal | Scheduled occurrences with date/time |
| note | note | Grey | Free-form notes without clear category |

---

## Hardware

| Component | Details |
|---|---|
| **MCU** | ESP32-S3 (dual-core Xtensa, 240 MHz) |
| **Display** | CO5300 1.43" AMOLED, 466×466 (cropped to 410×502), QSPI @80 MHz |
| **Touch** | FT3168 capacitive, I2C |
| **Microphone** | ES7210 4-channel ADC, I2S STD mode, 16 kHz mono |
| **Speaker** | ES8311 DAC, I2S TX, power amp on GPIO 46 |
| **Storage** | 32 MB QIO flash, 8 MB octal PSRAM, SD card (FAT) |
| **Connectivity** | WiFi 802.11 b/g/n |

### Pin Assignments

| Function | GPIO |
|---|---|
| Display QSPI CLK / CS | 11 / 12 |
| Display QSPI D0–D3 | 4, 5, 6, 7 |
| Display RST | 8 |
| Touch I2C SDA / SCL | 15 / 14 |
| Touch INT / RST | 38 / 9 |
| I2S BCLK / WS / MCLK | 41 / 45 / 16 |
| I2S DIN (mic → ESP32) | 42 |
| I2S DOUT (ESP32 → speaker) | 40 |
| Audio PA enable | 46 |
| Boot button | 0 |
| SD CLK / CMD / DAT0 | 2 / 1 / 3 |

---

## Architecture

```
┌─────────────────────────────────────────┐
│              ESP32-S3 Watch             │
│                                         │
│  Domain Badge ──tap──► Cycle Domain     │
│  Button (short) ──► Record ──► SD Queue ──► Upload ──► Backend
│  Button (long)  ──► Ask mode ──► Query  │         │
│              │                          │    ┌────┘
│              └─► Waveform UI            │    │
│                                         │    │
│  Session Screen ◄── Grouped Items ◄─────┘    │
│  (insight/action/event sections)             │
│  Home Screen ◄── Item List ◄── /items ◄──────┘
│  Speaker ◄── I2S TX ◄── WAV decode ◄── /query (TTS)
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│          Backend (Render.io)            │
│                                         │
│  /ingest  ──► Whisper STT ──► Domain-aware LLM ──► SQLite (session + items)
│  /query   ──► Whisper STT ──► Date Parse ──► Narrate ──► TTS (Edge)
│  /items   ──► SQLite query ──► JSON response
│  /sessions/{device_id} ──► Recent sessions
│  /sessions/{device_id}/latest ──► Latest session + items
│  /session/{session_id}/items ──► Items for session
│  /domains ──► Domain config (name, icon, color, categories)
└─────────────────────────────────────────┘
```

---

## Firmware (ESP-IDF 5.4, C)

### Source Files

| File | Purpose |
|---|---|
| `main.c` | App entry, button state machine (short press = ingest, long press = ask), upload/drain tasks, WiFi state callbacks, item action handler |
| `audio_pipeline.c/h` | I2S init (STD mode for ES7210 + ES8311), recording to WAV, WAV playback through speaker |
| `audio_capture.c/h` | BSP codec wrapper, WAV capture with RMS tracking, configurable digital gain (1.5×) on top of 42 dB hardware gain |
| `api_client.c/h` | HTTPS client — streaming multipart uploads to `/ingest`, `/query`; GET `/items`; PATCH/DELETE for item actions |
| `ui_screens.c/h` | LVGL 9.2 display driver (CO5300 QSPI, row-by-row chunked flush), FT3168 touch (new I2C driver), 7 screens |
| `wifi_manager.c/h` | NVS credential storage, STA connection with exponential backoff, SoftAP provisioning with WiFi scan + captive portal |
| `display_power.c/h` | AMOLED sleep/wake via MIPI DCS commands, 60s inactivity timer, activity signal from button/touch |
| `sd_queue.c/h` | SD card recording queue with manifest file, retry tracking, offline-first design |
| `minimp3.h` | Header-only MP3 decoder for query response playback |

### UI Screens

| # | Screen | Description |
|---|---|---|
| 1 | **Home** | Clock (bold, large), date, WiFi indicator, scrollable item cards with color-coded type bars, "X more >" link, "Done" button for finished items |
| 2 | **Recording** | Red dot + "REC", animated waveform bars (5 bars driven by RMS), elapsed timer, mode hint ("Ask mode — press to stop") |
| 3 | **Processing** | Animated dots, STT + Intent progress bars (ingest mode) or "Listening..." (query mode), answer text display |
| 4 | **Saved** | Type badge, title, date, confidence meter, auto-returns to home after 3s |
| 5 | **All Items** | Grouped by type (Events/Reminders/Notes) with section headers, tap-to-reveal action buttons (Done/Pause-Resume/Delete), back button |
| 6 | **Offline** | Queue count, SD card status |
| 7 | **Finished** | Completed items list, back button |
| 8 | **Provisioning** | WiFi setup instructions — AP SSID, URL (192.168.4.1), pulsing dot indicator |

### Key Technical Decisions

- **PSRAM for LVGL buffers** — Moved display buffers (2 × 32,800 bytes) to PSRAM, freeing ~65 KB internal RAM for mbedTLS hardware AES
- **Hardware AES** — `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` ensures TLS uses internal DMA RAM (software AES from PSRAM was ~400 bytes/sec — unusable)
- **Streaming uploads** — WAV files sent in 2 KB chunks via `esp_http_client_open/write` to avoid buffering entire file in RAM
- **BSP codec integration** — Uses `waveshare/esp32_s3_touch_amoled_2_06` BSP for proper ES7210/ES8311 initialization via `esp_codec_dev`
- **New I2C driver** — Migrated from legacy `driver/i2c.h` to `driver/i2c_master.h` to avoid conflict with BSP
- **Button debounce** — `btn_used_for_stop` flag prevents button release after recording stop from starting a new recording
- **Display wake guard** — Button press that wakes display is consumed without triggering recording

### Build Configuration

```
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
CONFIG_MBEDTLS_HARDWARE_AES=y
CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_PARTITION_TABLE_CUSTOM=y
```

Partition layout: 3 MB app + ~13 MB FAT storage on 32 MB flash.

---

## Backend (Python, FastAPI)

**Deployed on:** Render.io (free tier, cold starts 30–60s)
**URL:** `https://student-assistant-api.onrender.com`

### Endpoints

| Method | Path | Description |
|---|---|---|
| `POST` | `/ingest` | Upload WAV → Whisper STT → Domain-aware LLM → save session + items to SQLite |
| `POST` | `/query` | Upload WAV question → STT → date parsing → item lookup → TTS narration → WAV response |
| `GET` | `/items/{device_id}` | Fetch active (non-done) items as JSON |
| `GET` | `/sessions/{device_id}` | List recent sessions (last 20, newest first) |
| `GET` | `/sessions/{device_id}/latest` | Most recent session with all its items |
| `GET` | `/session/{session_id}/items` | All items for a specific session |
| `GET` | `/domains` | List all supported domains (name, icon, color, categories) |
| `PATCH` | `/items/{item_id}/toggle` | Mark item done/undone |
| `PATCH` | `/items/{item_id}/pause` | Pause/unpause item |
| `DELETE` | `/items/{item_id}` | Delete item |
| `GET` | `/health` | Server health check |

### Services

| Service | Implementation |
|---|---|
| **STT** | OpenAI Whisper API (`whisper-1`) |
| **Intent Extraction** | OpenAI GPT-4o-mini — domain-aware prompts extract category, type, title, due_date, recurrence |
| **Domain Config** | `services/domains.py` — 4 domains with tailored LLM system prompts |
| **Sessions** | SQLite sessions table — groups all items from one recording |
| **TTS** | Edge TTS (free, `en-US-AriaNeural`) → MP3 → 16 kHz mono WAV |
| **Database** | SQLite with aiosqlite |
| **Date Parsing** | Regex-based natural language parser |

### Authentication

HMAC-SHA256 token: `HMAC(SECRET_KEY, device_id)` sent as `X-Device-Token` header.

---

## WiFi Provisioning Flow

First boot (no saved credentials):

1. Watch shows provisioning screen: "Setup required — Connect to: StudentDevice-A4B2"
2. User connects phone to the ESP32's SoftAP network
3. Opens browser → `192.168.4.1`
4. Modern single-page app loads, scans available WiFi networks
5. User taps their network from the list, enters password
6. ESP32 saves credentials to NVS, reboots, connects to home WiFi
7. Watch transitions to home screen

Uses `WIFI_MODE_APSTA` so WiFi scanning works while AP is active.

---

## Voice Flows

### Ingest Flow (short press)
```
Short press → Recording screen → [speak] → Press to stop
  → Processing screen (STT ████░ / Intent ████░)
  → Upload WAV to /ingest (streaming 2KB chunks)
  → Backend: Whisper STT → GPT intent extraction
  → Saved screen ("Event: Piano Recital — 2026-04-01")
  → Home screen refreshes with new item
```

### Ask/Query Flow (long press ≥ 800ms)
```
Long press → Recording screen ("Ask mode") → [ask question] → Press to stop
  → Processing screen ("Listening...")
  → Upload WAV to /query
  → Backend: Whisper STT → date parse → item lookup → TTS narration
  → WAV response streamed back → played through speaker
  → Home screen
```

### Offline Recording
```
No WiFi → Record to SD card → SD queue tracks pending files
  → WiFi reconnects → Drain task auto-uploads (30s polling active, 10min sleeping)
```

---

## Display Power Management

- **Auto-sleep:** 60 seconds of inactivity → AMOLED Sleep In (0x10) + Display Off (0x28)
- **Wake triggers:** Boot button press, touchscreen tap
- **Wake guard:** First button press after sleep is consumed (no accidental recording)
- **Reduced polling:** Drain task polls every 10 minutes while display is off (vs 30s when active)

---

## Problems Solved

| Problem | Root Cause | Fix |
|---|---|---|
| Blank screen | SPI transfer too large for QSPI | Chunked flush in 8-row segments |
| Purple/garbled display | RGB565 byte order wrong for CO5300 | Corrected byte swapping |
| Upload fails at 4–6 KB | Software AES from PSRAM (~400 B/s) | Moved LVGL buffers to PSRAM, re-enabled hardware AES |
| I2S read timeout (ret=263) | TDM mode conflicts with STD TX | Switched to STD stereo mode (matching BSP) |
| Whisper hallucinations | ES7210 in TDM mode, ESP32 reading STD | Set ES7210 reg 0x12=0x00 (standard I2S) |
| Boot crash (I2C conflict) | Legacy I2C driver + BSP new driver | Migrated to `i2c_master.h` + `bsp_i2c_get_handle()` |
| VAD auto-triggering | RMS-based VAD too sensitive | Removed VAD, button-only recording |
| Stack overflow in audio task | 6 KB local buffers on 8 KB stack | Made buffers static |
| Watchdog crash on provisioning | `lv_obj_set_style_opa` takes 3 args, anim passes 2 | Added proper 2-arg wrapper |
| Recording restarts on stop | Button release after stop triggers new recording | `btn_used_for_stop` flag |
| Query audio not playing | 767 KB response > 512 KB buffer, single allocation | Streaming 2 KB chunks, 1 MB PSRAM buffer |
| Provisioning screen overwritten | `app_main` always shows home screen last | Skip home screen if in provisioning state |

---

## Repository Structure

```
ai-watch-main/
├── student-assistant/
│   ├── firmware/
│   │   ├── main/
│   │   │   ├── main.c              # App entry, button logic, domain state, session navigation
│   │   │   ├── api_client.c/h      # Backend HTTPS client (domain header, session_id parsing)
│   │   │   ├── audio_pipeline.c/h  # I2S, recording, playback
│   │   │   ├── audio_capture.c/h   # BSP codec wrapper
│   │   │   ├── ui_screens.c/h      # LVGL UI (8 screens incl. domain badge + session screen)
│   │   │   ├── wifi_manager.c/h    # WiFi + provisioning
│   │   │   ├── display_power.c/h   # AMOLED sleep/wake
│   │   │   ├── sd_queue.c/h        # Offline queue
│   │   │   └── minimp3.h           # MP3 decoder
│   │   ├── sdkconfig.defaults
│   │   ├── partitions.csv
│   │   └── CMakeLists.txt
│   ├── backend/
│   │   ├── main.py                 # FastAPI app
│   │   ├── models.py               # Pydantic models (Session, Item with domain/category)
│   │   ├── pytest.ini              # Test config (asyncio_mode=auto)
│   │   ├── requirements.txt
│   │   ├── routes/
│   │   │   ├── ingest.py           # /ingest endpoint (domain header, session creation)
│   │   │   ├── sessions.py         # /sessions and /session endpoints (new)
│   │   │   └── query.py            # /query endpoint
│   │   ├── services/
│   │   │   ├── domains.py          # Domain config + LLM prompt templates (new)
│   │   │   ├── stt.py              # Whisper STT
│   │   │   ├── intent.py           # Domain-aware LLM intent extraction
│   │   │   ├── tts.py              # Edge TTS
│   │   │   ├── db.py               # SQLite (sessions + items tables)
│   │   │   └── date_parser.py      # NL date parsing
│   │   └── tests/
│   │       ├── conftest.py         # Pytest fixtures
│   │       ├── test_ingest.py      # Ingest + domain tests
│   │       ├── test_sessions.py    # Session endpoint tests
│   │       └── test_domains.py     # Domains endpoint tests
│   └── ui/
│       └── index.html              # Web simulator (domain badge + session summary panel)
├── PROJECT_SUMMARY.md
└── render.yaml
```

---

## Deployment

| Component | URL / Location |
|---|---|
| **Backend** | https://student-assistant-api.onrender.com |
| **GitHub** | https://github.com/satishkch/AI-Watch-Assistant |
| **Device ID** | `demo-device` (hardcoded, shared across all watches) |

### Building & Flashing

```bash
cd student-assistant/firmware
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

---

## Future Work

- [ ] Unique device ID per watch (derived from MAC address)
- [ ] Speaker audio quality improvements (sample rate matching, volume control)
- [ ] Battery level display on home screen
- [ ] Haptic feedback on button press (if hardware supports it)
- [ ] OTA firmware updates
- [ ] Multiple WiFi network memory
- [ ] Parent companion app (phone) for managing items
