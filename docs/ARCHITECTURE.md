# Student Assistant — Architecture

Voice-first homework/schedule tracker for kids, running on a Waveshare ESP32-S3 2.06" AMOLED watch with a FastAPI backend deployed on Render.

## System diagram

```mermaid
flowchart TB
    subgraph User["👦 User"]
        Voice["Voice input<br/>(button press)"]
    end

    subgraph Watch["⌚ ESP32-S3 Watch (Waveshare 2.06'' AMOLED)"]
        direction TB

        subgraph HW["Hardware"]
            MIC["ES7210 ADC<br/>(I2S mic)"]
            SPK["ES8311 DAC<br/>+ speaker"]
            DISP["CO5300 AMOLED<br/>410x502 QSPI"]
            TOUCH["FT3168 touch"]
            PMU["AXP2101 PMIC<br/>(battery %)"]
            SD["SD card<br/>(offline queue)"]
            BTN["Boot button<br/>GPIO0"]
        end

        subgraph FW["Firmware (ESP-IDF + LVGL 9.3)"]
            direction TB
            UI["ui_screens<br/>• home • record<br/>• saved • splash"]
            SPLASH["splash_screen<br/>(Cue logo anim)"]
            AUDIO["audio_pipeline<br/>• record WAV<br/>• stream playback"]
            API["api_client<br/>(HTTP + cJSON)"]
            WIFI["wifi_manager<br/>(multi-cred NVS,<br/>APSTA provisioning)"]
            BATT["battery_monitor<br/>(AXP2101 poll 30s)"]
            QUEUE["SD queue<br/>(atomic manifest)"]
            MAIN["main.c<br/>orchestration"]
        end

        BTN --> MAIN
        MIC --> AUDIO
        AUDIO --> SPK
        DISP --> UI
        DISP --> SPLASH
        TOUCH --> UI
        PMU --> BATT
        BATT --> UI
        MAIN --> UI
        MAIN --> AUDIO
        MAIN --> API
        MAIN --> QUEUE
        QUEUE --> SD
        WIFI --> API
    end

    subgraph Cloud["☁️ Backend (FastAPI on Render)"]
        direction TB
        INGEST["/ingest<br/>(multipart WAV)"]
        QUERY["/query<br/>(returns audio/mpeg)"]
        ITEMS["/items, /items/range<br/>done, paused, delete"]

        subgraph SVC["Services"]
            STT["stt.py<br/>(Whisper)"]
            INTENT["intent.py<br/>(LLM → JSON array<br/>of items)"]
            DATES["date parser<br/>(pure regex)"]
            TTS["tts.py<br/>(streaming MP3)"]
            DB[("SQLite<br/>items table")]
        end

        INGEST --> STT --> INTENT --> DATES --> DB
        INGEST --> TTS
        QUERY --> DB
        QUERY --> TTS
        ITEMS --> DB
    end

    Voice --> MIC
    SPK -.audio out.-> User

    API <== "HTTPS<br/>WAV up / JSON + MP3 down" ==> INGEST
    API <== "HTTPS<br/>query up / MP3 stream down" ==> QUERY
    API <== "HTTPS<br/>CRUD" ==> ITEMS

    classDef hw fill:#2a4d3a,stroke:#4ade80,color:#fff
    classDef fw fill:#1e3a5f,stroke:#60a5fa,color:#fff
    classDef cloud fill:#3a2a4d,stroke:#c084fc,color:#fff
    class MIC,SPK,DISP,TOUCH,PMU,SD,BTN hw
    class UI,SPLASH,AUDIO,API,WIFI,BATT,QUEUE,MAIN fw
    class INGEST,QUERY,ITEMS,STT,INTENT,DATES,TTS,DB cloud
```

## Key data flows

### 1. Ingest flow (add homework / event / note)

```
BTN press → record WAV via ES7210
          → POST /ingest (multipart)
          → Whisper STT
          → LLM intent extraction → JSON array (1..N items)
          → regex date parser
          → insert into SQLite
          → response: { items[], count } + streaming MP3 confirmation
          → firmware streams MP3 to ES8311 as chunks arrive
          → UI shows "saved" screen (in parallel with TTS fetch)
```

### 2. Query flow (ask what's due)

```
BTN long-press → record WAV
              → POST /query
              → Whisper STT → date range extraction
              → SQLite SELECT in range
              → TTS summary → streaming MP3
              → firmware plays directly to speaker
              → return to home (no Processing screen flash)
```

### 3. Offline resilience

```
No WiFi → SD queue (atomic .tmp + rename manifest)
drain task → retries on WiFi reconnect → /ingest
```

### 4. Boot sequence

```
Power on → splash_screen (Cue logo, ~3.1s)
        → wifi_manager loads NVS creds (up to 5)
        → picks strongest known network by RSSI
        → battery_task starts (30s poll)
        → home screen
```

## Hardware pin reference

| Peripheral | Pins |
|---|---|
| Display CO5300 QSPI | CS=12, SCK=11, D0–D3=4/5/6/7, RST=8 (410×502 @ 80 MHz) |
| I2S audio | MCLK=16, BCLK=41, WS=45, DIN=42, DOUT=40, PA=46 |
| Audio codecs | ES8311 (DAC) + ES7210 (ADC) on I2C SDA=15, SCL=14 |
| Touch FT3168 | I2C SDA=15, SCL=14, INT=38, RST=9 |
| SD card (SDMMC) | CLK=2, CMD=1, DATA=3, CS=17 |
| Power (AXP2101) | Shared I2C bus, addr 0x34, battery % at reg 0xA4 |
| Button | Boot GPIO 0 |

## Architectural decisions

- **Raw LVGL 9.3 only** — no ESP-Brookesia framework.
- **Mock mode** runs the full pipeline with zero API keys (transcript saved as note).
- **`/query` always returns `audio/mpeg`**, streamed.
- **Date parser is pure regex**, never LLM — deterministic and fast.
- **SD queue** uses atomic manifest writes (`.tmp` + rename) for crash safety.
- **Backward-compatible API**: ingest response has `items[]` + `count`; legacy single-item fields retained as fallback so old firmware and new backend interoperate.
- **Streaming TTS**: firmware opens the codec on the first HTTP chunk and feeds PCM directly, eliminating full-response buffering.
- **Multi-WiFi**: up to 5 credentials in NVS; picks strongest known network by RSSI on boot.
