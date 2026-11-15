# AGENTS.md

Monorepo for "Шпора": one Flask backend and three ESP devices. The backend is the only
stateful service; devices are deliberately dumb. Comments, logs and UI strings are Russian —
keep that style. `README.md` documents the product, `docs/FLASHING.md` documents the
verified flashing procedures.

## Layout

- `app.py` — Flask backend: photo staging, voice chunks, lesson recording, two answer slots.
- `tests/test_backend.py` — stdlib `unittest` suite (Flask test client + mocks).
- `esp32-cam/ESP32_CAM_recorder/` — AI Thinker ESP32-CAM firmware (silent, XGA/Q10).
- `esp32-c3/microphone/` — ESP32-C3 SuperMini mic firmware (5 s WAV chunks).
- `microreader/firmware/a-microReader-v1.3/` — current reader firmware.
- `microreader/firmware/a-microReader-v1.2/` — frozen legacy; do not "sync" changes into it.
- `lessons/` — runtime output of lesson mode (gitignored; never commit).

## Commands

```powershell
# backend
python -m unittest discover -s tests -v          # full suite; no network, sub-second
python -m unittest tests.test_backend.PhotoFrameTest.test_frame_ok_staged_count -v
python app.py --port 8080                        # local dev; dotenv loads .env

# firmware build/upload (arduino-cli is NOT on PATH here; a copy was run from %TEMP%.
# Installed cores: esp8266 3.1.2, esp32 3.3.11 under %LOCALAPPDATA%\Arduino15)
arduino-cli compile --fqbn "esp8266:esp8266:generic:eesz=4M2M,baud=921600" microreader/firmware/a-microReader-v1.3
arduino-cli upload  --fqbn "esp8266:esp8266:generic:eesz=4M2M,baud=921600" -p COMx microreader/firmware/a-microReader-v1.3
arduino-cli upload  --fqbn "esp32:esp32:esp32cam:PartitionScheme=huge_app" -p COMx esp32-cam/ESP32_CAM_recorder
arduino-cli upload  --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" -p COMx esp32-c3/microphone

# after changing only constants, force a fresh build (stale-cache symptom:
# "No changed sectors found, verifying if data is in flash")
arduino-cli compile --clean --upload -p COMx --fqbn "..." <sketch-dir>
```

- `esptool.exe` ships with the ESP32 core at
  `%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe`.
- A busy COM port (`PermissionError(13)`) is almost always an open Arduino IDE Serial Monitor.
- ESP8266 through the CH340 programmer: use **115200**; 921600 fails with `Invalid head of packet`.
  ESP32/C3 handle 921600.

## Wire contracts (breaking these silently breaks devices)

- `/api/latest` and `/api/photo-status` are **compact** JSON (`separators=(",", ":")`,
  `ensure_ascii=False`). Firmware parses them with `indexOf` and exact substrings such as
  `"ready":true` / `"answer_ready":true`. Never pretty-print; never reorder existing keys —
  append new keys at the end.
- Current `/api/photo-status` key order: `staged, photo_status, answer_ready, voice_busy,
  voice_mode, lesson_chunks, cam_ip, voice_recording`.
- Every route except `/api/health` requires `X-Device-Key`. Firmware sends it on every request.
- Legacy `/api/photo`, `/api/photos`, `/api/voice` must stay (unflashed CAM / curl).
- ESP32-CAM sends `X-Device-Ip` both on boot (`POST /api/device-ip`) and on every photo request;
  the reader shows it as `cam_ip`.
- Answer text is truncated server-side at `MAX_ANSWER_CHARS` (8000) before it reaches devices.

## Backend state is RAM-only and single-process

- In memory: photo staging, two answer slots, voice sessions, lesson sessions, commit mutex,
  voice mode, `cam_ip`, voice activity. **gunicorn must stay `--workers 1`**; a second worker
  silently split-brains all of it. Threads give concurrency (`--threads 8 --timeout 150`).
- Any restart wipes staging/answers/sessions and resets voice mode to `qa`. The reader shows
  `Режим: ...?` until it re-confirms the mode.
- Photo flow: short press → `POST /api/photo-frame`; long hold → `POST /api/photos/commit`.
  Staging caps: 20 frames / 12 MB / TTL 1800 s. Concurrent commit → `429`; Gemini failure →
  `502` and frames are kept for retry.
- Voice chunks are stored by `X-Chunk-Index`; the final chunk merges and calls Gemini
  (or Groq first when `GROQ_STT_ENABLED=true`, with automatic fallback to direct audio).
- `/api/latest` returns the newest ready answer from either slot and **consumes only that slot**.
  Calling it during debugging destroys the pending answer.
- Lesson mode (`POST /api/voice-mode {"mode":"lesson"}`) appends chunks to
  `lessons/lesson_<YYYY-MM-DD_HH-MM-SS>_<sid>.wav` + `.json` and **never calls Gemini**.
- `voice_recording` is true while the last voice chunk is younger than `VOICE_ACTIVE_WINDOW`
  (12 s); it is cleared by the final chunk.
- Startup guard (`GOOGLE_API_KEY` present, `DEVICE_KEY` not a placeholder) runs only under
  `__main__`. Keep it there: tests and gunicorn import the module and must not exit.

## Firmware gotchas

- **microReader v1.3**: Menu 1 hides `answer_*.txt`; Menu 2 lists them **newest first** (max 40,
  oldest evicted); Menu 3 is a fixed 6-row screen (toggle, mode, frames, recording/lesson,
  answer/notice, CAM IP). When an answer becomes ready it saves silently, turns WiFi off and
  auto-returns to Menu 2. Poll 2 s; HTTP 10 s (voice-mode 3 s).
- **microReader flashing** (the part everyone gets wrong): use the ESP32-CAM-MB as USB-UART with
  the CAM module **removed**; wires `GND↔GND`, `TX→RXD`, `RX→TXD` (swap TX/RX if no sync);
  **never 5 V**; jumper **DTR–GND = GPIO0**; power from the reader battery. Remove the jumper and
  power-cycle after flashing. Full erase: `esptool erase-flash`, then
  `write-flash 0x0 a-microReader-v1.3.bin --flash-mode dout --flash-freq 40m --flash-size 4MB`.
- **ESP32-CAM**: silent by design — the only LED signal is one long + one short blink ~2 s after
  WiFi connects. It caches the last frame in PSRAM, serves `GET /last_frame`, and its page shows
  the frame only by button (auto-load would block the single-threaded web server). An open
  `/stream` blocks every other request until the tab is closed. Capture is XGA 1024×768, JPEG q10.
- **ESP32-C3**: the record button is the **external GPIO3** button to GND; BOOT/GPIO9 is ignored
  by firmware (still a hardware strap for flashing). Blue LED is solid for the whole recording,
  off at stop; 2/3/5 blink codes only at session end. One recording = one session = one lesson
  WAV, up to 2 h; 3 upload retries per chunk; SPIFFS `/c*.wav` cleaned on boot.
- Devices are HTTP-only **by owner decision**; do not add TLS to them. Protect the device key.

## Deploy

- VPS alias `vps-cz` (user `admin`), directory `~/shpora-backend`, systemd unit
  `shpora-backend`, gunicorn on `$PORT` from `.env` (currently 42444). The VPS has no git
  checkout; `.env` is local and must be preserved.
- Deploy = copy the file, restart, verify:
  ```powershell
  scp app.py vps-cz:~/shpora-backend/app.py
  ssh vps-cz 'sudo systemctl restart shpora-backend'
  ssh vps-cz 'cd ~/shpora-backend && PORT=$(grep "^PORT=" .env | cut -d= -f2) && KEY=$(grep "^DEVICE_KEY=" .env | cut -d= -f2-) && curl -s http://127.0.0.1:$PORT/api/health'
  ```
- Restarting mid-lesson loses the in-flight recording remainder.

## Conventions

- Conventional commits, atomic per component: `feat(backend): ...`, `fix(microreader): ...`,
  `docs: ...`. Do not push unless asked.
- Russian comments/logs/UI; short, operational messages; no markdown in Gemini prompts
  (the display cannot render it).
- `config.h` (CAM/C3), `.env`, `*.bin`, `.omo/`, `lessons/` are gitignored. Never commit real
  credentials. `config.example.h` and `.env.example` are the only templates.
- The reader firmware keeps `answer_N.txt` numbering first-free; deleting files can create gaps.
