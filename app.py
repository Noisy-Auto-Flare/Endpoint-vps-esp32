"""
Бэкенд-посредник между ESP32-CAM / microReader и Google AI Studio (Gemini API).

Как это работает:
  1) ESP32-CAM шлёт JPEG на POST /api/photo
     -> сервер отправляет его в Gemini (vision) через Google AI Studio
     -> текстовый ответ сохраняется как "последний результат"

   2) ESP32-C3 шлёт WAV-чанки на POST /api/voice-chunk (или WAV на POST /api/voice)
      -> сервер склеивает чанки в один WAV
      -> если GROQ_STT_ENABLED=true: Groq Whisper (расшифровка в текст) -> текст в Gemini;
         иначе (по умолчанию) аудио НАПРЯМУЮ в Gemini (модель понимает звук без
         отдельного шага транскрипции) с промптом "ответь на то, что услышал"
      -> текстовый ответ сохраняется как "последний результат"

  3) microReader опрашивает GET /api/latest
     -> получает {"ready": true/false, "text": "..."}
     -> после успешной отдачи результат помечается как забранный (consumed)

Все запросы (кроме /api/health) требуют заголовок:
  X-Device-Key: <твой секретный ключ, задаётся в .env>
Это чтобы посторонние не могли слать запросы на твой сервер и жечь баланс Google AI Studio.

Ключ Google AI Studio берётся тут: https://aistudio.google.com/apikey
"""

import argparse
import base64
import hmac
import os
import struct
import sys
import threading
import time

try:
    from dotenv import load_dotenv
    load_dotenv()
except ImportError:
    pass

import json
from flask import Flask, request, Response, jsonify
import requests

# ============================ CONFIG ================================

GOOGLE_API_KEY = os.environ.get("GOOGLE_API_KEY", "")
DEVICE_KEY = os.environ.get("DEVICE_KEY", "change-me")

GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.5-flash-lite")

GEMINI_BASE = "https://generativelanguage.googleapis.com/v1beta"

# --- STT (распознавание речи) через Groq Whisper, вкл/выкл тумблером ---
# Выкл (по умолчанию): аудио напрямую в отвечающую модель Gemini.
# Вкл: чанки -> склейка -> Groq Whisper (расшифровка в текст) -> Gemini (ответ).
# Резервный путь: если GROQ_STT_ENABLED=true, но GROQ_API_KEY не задан или Groq
# упал/вернул пусто — аудио сразу в Gemini.
GROQ_STT_ENABLED = os.environ.get("GROQ_STT_ENABLED", "false").strip().lower() in ("1", "true", "yes", "on")
GROQ_API_KEY = os.environ.get("GROQ_API_KEY", "")
GROQ_STT_MODEL = os.environ.get("GROQ_STT_MODEL", "whisper-large-v3")
GROQ_STT_LANGUAGE = os.environ.get("GROQ_STT_LANGUAGE", "ru")
GROQ_API_BASE = "https://api.groq.com/openai/v1"

# Системный промпт для голосовых запросов напрямую по аудио (можно менять под задачу)
VOICE_AUDIO_PROMPT = os.environ.get(
    "VOICE_AUDIO_PROMPT",
    "Ты — голосовой ассистент школьника на маленьком устройстве. "
    "Тебе присылают аудио с голосовым вопросом или заданием пользователя — слушай аудио внимательно. "
    "Отвечай ПОДРОБНО, строго в трёх частях по порядку: "
    "1) СНАЧАЛА опиши, как ты понял задание: перескажи, что именно услышал в аудио и что требуется сделать. "
    "2) ЗАТЕМ очень подробно объясни ход решения: почему ответ такой, а не другой, "
    "обоснуй каждый шаг простыми словами, как учитель для ученика. "
    "3) В КОНЦЕ чётко назови итоговый ответ. "
    "Никакой markdown-разметки и специальных символов (звёздочки, решётки, доллары запрещены) — "
    "только обычный текст, который удобно читать вслух или на маленьком экране. "
    "Если в аудио не задание, а просто вопрос — отвечай подробно и по существу."
)

# Системный промпт для голосовых запросов по готовой расшифровке (путь через Groq STT)
VOICE_TRANSCRIPT_PROMPT = os.environ.get(
    "VOICE_TRANSCRIPT_PROMPT",
    "Ты — голосовой ассистент школьника на маленьком устройстве. "
    "Тебе присылают расшифровку голосового вопроса или задания пользователя текстом. "
    "Отвечай ПОДРОБНО, строго в трёх частях по порядку: "
    "1) СНАЧАЛА опиши, как ты понял задание: перескажи, что именно сказано в расшифровке и что требуется сделать. "
    "2) ЗАТЕМ очень подробно объясни ход решения: почему ответ такой, а не другой, "
    "обоснуй каждый шаг простыми словами, как учитель для ученика. "
    "3) В КОНЦЕ чётко назови итоговый ответ. "
    "Никакой markdown-разметки и специальных символов (звёздочки, решётки, доллары запрещены) — "
    "только обычный текст, который удобно читать вслух или на маленьком экране. "
    "Если в расшифровке не задание, а просто вопрос — отвечай подробно и по существу."
)

# Системный промпт для фото-запросов
PHOTO_SYSTEM_PROMPT = os.environ.get(
    "PHOTO_SYSTEM_PROMPT",
    "Если на фото какое-то задание, задача, то выведи, какие условия ты распознал, подробно и с объяснениями реши это задание без markdown-разметки и любых специальных символов. "
    "Если на фото что-то кроме заданий, то опиши, что ты видишь"
)

# Голосовые сессии ESP32-C3: накопление чанков по 5 секунд,
# склейка в один WAV и отправка в Gemini при приходе финального чанка.
VOICE_SESSION_TTL = int(os.environ.get("VOICE_SESSION_TTL", "600"))          # секунд: сессия без финального чанка сгорает
VOICE_MAX_CHUNKS = int(os.environ.get("VOICE_MAX_CHUNKS", "20"))            # максимум чанков на сессию (20 * 5 сек = 100 сек записи)
VOICE_CHUNK_MAX_BYTES = int(os.environ.get("VOICE_CHUNK_MAX_BYTES", str(1 * 1024 * 1024)))
VOICE_MAX_SESSIONS = int(os.environ.get("VOICE_MAX_SESSIONS", "50"))
PHOTO_MAX_BYTES = int(os.environ.get("PHOTO_MAX_BYTES", str(2 * 1024 * 1024)))
VOICE_MAX_BYTES = int(os.environ.get("VOICE_MAX_BYTES", str(2 * 1024 * 1024)))

# HTTP body caps (env-overridable, sane defaults)
MAX_CONTENT_LENGTH = int(os.environ.get("MAX_CONTENT_LENGTH", str(8 * 1024 * 1024)))
PHOTO_FRAME_MAX_BYTES = int(os.environ.get("PHOTO_FRAME_MAX_BYTES", str(2 * 1024 * 1024)))
PHOTOS_MAX_IMAGES = int(os.environ.get("PHOTOS_MAX_IMAGES", "10"))
PHOTOS_MAX_TOTAL_B64 = int(os.environ.get("PHOTOS_MAX_TOTAL_B64", str(6 * 1024 * 1024)))

# =====================================================================

app = Flask(__name__)
app.json.ensure_ascii = False   # отключает экранирование Unicode — иначе русский текст придёт как \uXXXX
app.config["MAX_CONTENT_LENGTH"] = MAX_CONTENT_LENGTH

# Два слота ответов (фото + голос): каждый {ready,text,source,created_at,seq}.
# /api/latest отдаёт newest ready и потребляет только его.
_state_lock = threading.Lock()
_answer_seq = 0
_photo_answer = {"ready": False, "text": "", "source": None, "created_at": 0, "seq": 0}
_voice_answer = {"ready": False, "text": "", "source": None, "created_at": 0, "seq": 0}

# Честный answer_ready: true только если слот стал ready после последнего failed commit.
_last_commit_failed = False
_last_commit_failed_at = 0.0

# voice_busy: true пока обрабатывается голосовой финал.
_voice_busy_lock = threading.Lock()
_voice_busy = False

# Накопление голосовых чанков от ESP32-C3: session_id -> {"chunks": {index: bytes}, "created_at": float}
# Чанки хранятся по X-Chunk-Index, на финале сортируются по индексу.
_voice_sessions_lock = threading.Lock()
_voice_sessions = {}

# Стейджинг фото-кадров v2 (один глобальный, как слоты ответов — одна связка устройств).
# Короткие нажатия складывают кадры сюда, удержание коммитит всё в Gemini разом.
PHOTO_STAGING_CAP = int(os.environ.get("PHOTO_STAGING_CAP", "20"))
PHOTO_BYTES_CAP = int(os.environ.get("PHOTO_BYTES_CAP", str(12 * 1024 * 1024)))
PHOTO_STAGING_TTL = int(os.environ.get("PHOTO_STAGING_TTL", "1800"))
_photo_lock = threading.Lock()
_photo_commit_lock = threading.Lock()  # не более одного коммита одновременно (гонка двух commit)
_photo_staging: list = []       # список (jpeg_bytes, enqueued_at)
_photo_status = "idle"          # idle | staged | sent_to_llm | txt_ready | consumed | error


def _sweep_photo_staging_locked(now: float):
    global _photo_status
    if not _photo_staging:
        return
    kept = [(b, t) for (b, t) in _photo_staging if now - t <= PHOTO_STAGING_TTL]
    if len(kept) != len(_photo_staging):
        dropped = len(_photo_staging) - len(kept)
        _photo_staging[:] = kept
        if not _photo_staging:
            _photo_status = "idle"
        print(f"[photo] sweep expired dropped={dropped} staged={len(_photo_staging)} ttl={PHOTO_STAGING_TTL}", flush=True)


def _set_voice_busy(value: bool):
    global _voice_busy
    with _voice_busy_lock:
        _voice_busy = value


def _get_voice_busy() -> bool:
    with _voice_busy_lock:
        return _voice_busy


@app.errorhandler(413)
def _too_large(e):
    size = request.content_length or 0
    print(f"[http] 413 path={request.path} bytes={size}", flush=True)
    return jsonify({"error": "request too large"}), 413


def require_device_key():
    key = request.headers.get("X-Device-Key", "")
    return hmac.compare_digest(key, DEVICE_KEY)


def _is_voice_source(source: str) -> bool:
    return source.startswith("voice")


def set_latest_result(text: str, source: str):
    global _answer_seq
    now = time.time()
    with _state_lock:
        _answer_seq += 1
        slot = _voice_answer if _is_voice_source(source) else _photo_answer
        slot["ready"] = True
        slot["text"] = text
        slot["source"] = source
        slot["created_at"] = now
        slot["seq"] = _answer_seq
    print(f"[answer] source={source} len={len(text)} preview={text[:100]!r}", flush=True)


def _newest_ready_slot_locked():
    best_name = None
    best = None
    for name, slot in (("photo", _photo_answer), ("voice", _voice_answer)):
        if slot["ready"]:
            if best is None or (slot["created_at"], slot["seq"]) > (best["created_at"], best["seq"]):
                best_name = name
                best = slot
    return best_name, best


def _honest_answer_ready_locked() -> bool:
    _, best = _newest_ready_slot_locked()
    if best is None:
        return False
    if _last_commit_failed and _last_commit_failed_at > best["created_at"]:
        return False
    return True


def call_gemini(system_prompt: str, parts: list) -> str:
    """
    parts - список частей контента, например:
      [{"text": "..."}] или [{"text": "..."}, {"inline_data": {"mime_type": "...", "data": "<base64>"}}]
    """
    url = f"{GEMINI_BASE}/models/{GEMINI_MODEL}:generateContent"
    resp = requests.post(
        url,
        params={"key": GOOGLE_API_KEY},
        headers={"Content-Type": "application/json"},
        json={
            "system_instruction": {"parts": [{"text": system_prompt}]},
            "contents": [{"role": "user", "parts": parts}],
        },
        timeout=60,
    )
    resp.raise_for_status()
    data = resp.json()

    candidates = data.get("candidates", [])
    if not candidates:
        raise RuntimeError(f"Пустой ответ от Gemini: {data}")

    content_parts = candidates[0].get("content", {}).get("parts", [])
    # Берём ТОЛЬКО финальный текст: части с "thought": true — это внутренние
    # размышления модели, пользователю они не нужны.
    text_chunks = [
        p.get("text", "") for p in content_parts
        if "text" in p and not p.get("thought")
    ]
    answer = "".join(text_chunks).strip()
    if not answer:
        raise RuntimeError(f"Пустой ответ от Gemini: {data}")
    return answer


def merge_wav_chunks(chunks: list) -> bytes:
    """
    Склеивает несколько WAV-файлов (16 кГц, 16 бит, моно) в один WAV.

    Каждый чанк с ESP32-C3 — полноценный WAV-файл с заголовком 44 байта.
    Вырезаем data-секцию каждого чанка и собираем единый WAV с новым заголовком.
    """
    if not chunks:
        raise RuntimeError("empty chunks")

    data_parts = []
    total_data_size = 0
    for chunk in chunks:
        if len(chunk) <= 44:
            continue  # пустой финальный маркер или заголовок без данных
        data_parts.append(chunk[44:])  # отрезаем заголовок
        total_data_size += len(chunk) - 44

    if total_data_size == 0:
        raise RuntimeError("chunks contain no audio data")

    sample_rate = 16000
    num_channels = 1
    bits_per_sample = 16
    byte_rate = sample_rate * num_channels * bits_per_sample // 8
    block_align = num_channels * bits_per_sample // 8

    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        36 + total_data_size,
        b"WAVE",
        b"fmt ",
        16,                       # subchunk1 size
        1,                        # audio format: PCM
        num_channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        total_data_size,
    )
    return header + b"".join(data_parts)


def transcribe_groq(wav_bytes: bytes) -> str:
    """
    Расшифровывает WAV в текст через Groq Whisper (OpenAI-совместимый API).

    Формат от прошивки (16 кГц, 16 бит, моно) Whisper принимает как есть.
    Бросает исключение при любой ошибке — вызывающий код уходит в резервный путь.
    """
    resp = requests.post(
        f"{GROQ_API_BASE}/audio/transcriptions",
        headers={"Authorization": f"Bearer {GROQ_API_KEY}"},
        files={"file": ("audio.wav", wav_bytes, "audio/wav")},
        data={
            "model": GROQ_STT_MODEL,
            "language": GROQ_STT_LANGUAGE,
            "response_format": "json",
        },
        timeout=60,
    )
    resp.raise_for_status()
    return (resp.json().get("text") or "").strip()


def answer_voice(wav_bytes: bytes):
    """
    Единая STT-развилка для голоса: Groq Whisper -> текст -> Gemini,
    иначе склеенное/сырое аудио напрямую в Gemini.

    Используется и /api/voice, и финалом /api/voice-chunk, чтобы тумблер
    GROQ_STT_ENABLED действовал одинаково на обоих путях (аудит M3).
    Возвращает (answer, stt_used, transcript); ошибки Gemini пробрасывает выше.
    """
    stt_used = False
    transcript = ""
    if GROQ_STT_ENABLED and GROQ_API_KEY:
        try:
            groq_t0 = time.monotonic()
            transcript = transcribe_groq(wav_bytes)
            groq_ms = int((time.monotonic() - groq_t0) * 1000)
            if transcript:
                stt_used = True
                print(f"[stt] groq ok, model={GROQ_STT_MODEL} ms={groq_ms} len={len(transcript)} "
                      f"preview={transcript[:100]!r}", flush=True)
            else:
                print("[stt] groq вернул пустой текст; fallback на Gemini audio", flush=True)
        except (requests.RequestException, ValueError) as e:
            print(f"[stt] groq упал err_class={type(e).__name__}; fallback на Gemini audio", flush=True)
    elif GROQ_STT_ENABLED:
        print("[stt] GROQ_STT_ENABLED=true, но GROQ_API_KEY не задан; fallback на Gemini audio", flush=True)
    if not stt_used:
        print(f"[stt] gemini_direct transcript_len=0 bytes={len(wav_bytes)}", flush=True)

    stt_tag = "groq" if stt_used else "gemini_direct"
    t0 = time.monotonic()
    try:
        if stt_used:
            answer = call_gemini(
                VOICE_TRANSCRIPT_PROMPT,
                [{"text": f"Голосовое сообщение пользователя (расшифровка речи):\n{transcript}"}],
            )
        else:
            audio_b64 = base64.b64encode(wav_bytes).decode("utf-8")
            answer = call_gemini(
                VOICE_AUDIO_PROMPT,
                [
                    {"inline_data": {"mime_type": "audio/wav", "data": audio_b64}},
                ],
            )
    except (requests.RequestException, RuntimeError) as e:
        dt_ms = int((time.monotonic() - t0) * 1000)
        print(f"[gemini] voice fail err_class={type(e).__name__} ms={dt_ms} status=502 stt={stt_tag}", flush=True)
        raise
    dt_ms = int((time.monotonic() - t0) * 1000)
    print(f"[gemini] voice ok ms={dt_ms} stt={stt_tag} bytes={len(wav_bytes)}", flush=True)
    return answer, stt_used, transcript


# ---------------------------------------------------------------------
# POST /api/photo — принимает JPEG (сырые байты в теле запроса)
# ---------------------------------------------------------------------
@app.route("/api/photo", methods=["POST"])
def handle_photo():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    image_bytes = request.get_data()
    print(f"[photo] ingress bytes={len(image_bytes)}", flush=True)
    if not image_bytes:
        return jsonify({"error": "empty body, expected raw JPEG bytes"}), 400
    if len(image_bytes) > PHOTO_MAX_BYTES:
        print(f"[photo] 413 bytes={len(image_bytes)} cap={PHOTO_MAX_BYTES}", flush=True)
        return jsonify({"error": "photo too large"}), 413

    image_b64 = base64.b64encode(image_bytes).decode("utf-8")

    t0 = time.monotonic()
    try:
        answer = call_gemini(
            PHOTO_SYSTEM_PROMPT,
            [
                {"text": "Проанализируй это фото."},
                {"inline_data": {"mime_type": "image/jpeg", "data": image_b64}},
            ],
        )
    except (requests.RequestException, RuntimeError) as e:
        dt_ms = int((time.monotonic() - t0) * 1000)
        print(f"[gemini] photo fail err_class={type(e).__name__} ms={dt_ms} status=502", flush=True)
        return jsonify({"error": "gemini request failed"}), 502
    dt_ms = int((time.monotonic() - t0) * 1000)
    print(f"[gemini] photo ok ms={dt_ms} bytes={len(image_bytes)}", flush=True)

    set_latest_result(answer, source="photo")
    return jsonify({"ok": True, "text": answer})


# ---------------------------------------------------------------------
# POST /api/photos — принимает СЕРИЮ фото: JSON {"images": ["<base64_1>", ...]}
# ---------------------------------------------------------------------
@app.route("/api/photos", methods=["POST"])
def handle_photos():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    payload = request.get_json(silent=True)
    if not payload or not isinstance(payload.get("images"), list) or not payload["images"]:
        return jsonify({"error": 'expected JSON body: {"images": ["<base64>", ...]}'}), 400

    images_b64 = payload["images"]
    print(f"[photos] ingress n={len(images_b64)}", flush=True)
    if not all(isinstance(s, str) for s in images_b64):
        return jsonify({"error": 'expected JSON body: {"images": ["<base64>", ...]}'}), 400
    if len(images_b64) > PHOTOS_MAX_IMAGES:
        print(f"[photos] 413 n={len(images_b64)} cap={PHOTOS_MAX_IMAGES}", flush=True)
        return jsonify({"error": "too many images"}), 413
    total_b64 = sum(len(s) for s in images_b64 if isinstance(s, str))
    if total_b64 > PHOTOS_MAX_TOTAL_B64:
        print(f"[photos] 413 total_b64={total_b64} cap={PHOTOS_MAX_TOTAL_B64}", flush=True)
        return jsonify({"error": "images too large"}), 413

    parts = [{"text": "Проанализируй эту серию из нескольких фото (сделаны одно за другим)."}]
    for img_b64 in images_b64:
        parts.append({"inline_data": {"mime_type": "image/jpeg", "data": img_b64}})

    t0 = time.monotonic()
    try:
        answer = call_gemini(PHOTO_SYSTEM_PROMPT, parts)
    except (requests.RequestException, RuntimeError) as e:
        dt_ms = int((time.monotonic() - t0) * 1000)
        print(f"[gemini] photos fail err_class={type(e).__name__} ms={dt_ms} n={len(images_b64)} status=502", flush=True)
        return jsonify({"error": "gemini request failed"}), 502
    dt_ms = int((time.monotonic() - t0) * 1000)
    print(f"[gemini] photos ok ms={dt_ms} n={len(images_b64)}", flush=True)

    set_latest_result(answer, source="photos_burst")
    return jsonify({"ok": True, "text": answer})


# ---------------------------------------------------------------------
# POST /api/voice — принимает WAV (сырые байты в теле запроса)
# ---------------------------------------------------------------------
@app.route("/api/voice", methods=["POST"])
def handle_voice():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    audio_bytes = request.get_data()
    print(f"[voice] ingress bytes={len(audio_bytes)}", flush=True)
    if not audio_bytes:
        return jsonify({"error": "empty body, expected raw WAV bytes"}), 400
    if len(audio_bytes) > VOICE_MAX_BYTES:
        print(f"[voice] 413 bytes={len(audio_bytes)} cap={VOICE_MAX_BYTES}", flush=True)
        return jsonify({"error": "voice too large"}), 413

    try:
        answer, _stt_used, _transcript = answer_voice(audio_bytes)
    except (requests.RequestException, RuntimeError):
        return jsonify({"error": "gemini request failed"}), 502

    set_latest_result(answer, source="voice")
    return jsonify({"ok": True, "text": answer})


# ---------------------------------------------------------------------
# POST /api/voice-chunk — принимает WAV-чанки записи по 5 секунд от ESP32-C3
# Заголовки:
#   X-Session-Id:   уникальный ID сессии (одна сессия = одна запись целиком)
#   X-Chunk-Index:  0-based номер чанка (порядок склейки)
#   X-Final:        "true" на последнем чанке — после него сервер склеивает всё
#                   и отправляет в Gemini
# ---------------------------------------------------------------------
@app.route("/api/voice-chunk", methods=["POST"])
def handle_voice_chunk():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    session_id = request.headers.get("X-Session-Id", "").strip()
    chunk_index = request.headers.get("X-Chunk-Index", "0").strip()
    is_final = request.headers.get("X-Final", "false").strip().lower() == "true"

    if not session_id:
        return jsonify({"error": "missing X-Session-Id header"}), 400

    try:
        chunk_index = int(chunk_index)
    except ValueError:
        return jsonify({"error": "X-Chunk-Index must be an integer"}), 400

    if chunk_index < 0:
        return jsonify({"error": "X-Chunk-Index must be >= 0"}), 400

    audio_bytes = request.get_data()
    sid_tail = session_id[-6:] if len(session_id) > 6 else session_id
    if len(audio_bytes) > VOICE_CHUNK_MAX_BYTES:
        print(f"[voice] 413 sid_tail={sid_tail} bytes={len(audio_bytes)} cap={VOICE_CHUNK_MAX_BYTES}", flush=True)
        return jsonify({"error": "chunk too large"}), 413
    if is_final:
        # Пустой финальный маркер допустим: кнопка стоп была нажата МЕЖДУ чанками
        # (во время отправки предыдущего) — склеиваем всё накопленное ранее.
        if audio_bytes and len(audio_bytes) < 44:
            return jsonify({"error": "invalid final chunk, expected raw WAV bytes"}), 400
    elif not audio_bytes or len(audio_bytes) <= 44:
        return jsonify({"error": "empty body, expected raw WAV bytes"}), 400

    print(f"[voice] chunk sid_tail={sid_tail} idx={chunk_index} bytes={len(audio_bytes)} final={is_final}", flush=True)

    # Очистка просроченных сессий (чтобы память не текла)
    now = time.time()
    with _voice_sessions_lock:
        expired = [sid for sid, s in _voice_sessions.items() if now - s["created_at"] > VOICE_SESSION_TTL]
        for sid in expired:
            del _voice_sessions[sid]
        if expired:
            print(f"[voice] evict n={len(expired)} ttl={VOICE_SESSION_TTL}", flush=True)

        session = _voice_sessions.get(session_id)
        if session is None:
            if len(_voice_sessions) >= VOICE_MAX_SESSIONS:
                print(f"[voice] 413 sessions={len(_voice_sessions)} cap={VOICE_MAX_SESSIONS} sid_tail={sid_tail}", flush=True)
                return jsonify({"error": "too many sessions"}), 413
            session = {"chunks": {}, "created_at": now}
            _voice_sessions[session_id] = session

        # Защита от разрастания: сбрасываем сессию, если пришло слишком много чанков.
        # Повтор уже известного индекса — перезапись, а не рост (см. warn ниже).
        if chunk_index not in session["chunks"] and len(session["chunks"]) >= VOICE_MAX_CHUNKS:
            n = len(session["chunks"])
            del _voice_sessions[session_id]
            print(f"[voice] 413 sid_tail={sid_tail} chunks={n} cap={VOICE_MAX_CHUNKS}", flush=True)
            return jsonify({"error": "too many chunks, session reset"}), 413

        if audio_bytes:
            if chunk_index in session["chunks"]:
                print(f"[voice] warn sid_tail={sid_tail} duplicate idx={chunk_index} overwrite", flush=True)
            session["chunks"][chunk_index] = audio_bytes

        if not is_final:
            return jsonify({"ok": True, "chunks_received": len(session["chunks"])})

        # Финальный чанк: забираем накопленное в порядке индексов и очищаем сессию
        ordered_keys = sorted(session["chunks"].keys())
        chunks = [session["chunks"][i] for i in ordered_keys]
        if ordered_keys:
            expected = list(range(ordered_keys[-1] + 1))
            missing = [i for i in expected if i not in session["chunks"]]
        else:
            expected = []
            missing = []
        del _voice_sessions[session_id]

    if missing:
        print(f"[voice] merge fail reason=missing chunks sid_tail={sid_tail} expected={expected} got={ordered_keys}", flush=True)
        return jsonify({"error": f"missing chunks: expected {expected} got {ordered_keys}"}), 400

    _set_voice_busy(True)
    try:
        t_merge = time.monotonic()
        try:
            merged_audio = merge_wav_chunks(chunks)
        except RuntimeError as e:
            print(f"[voice] merge fail reason={e} sid_tail={sid_tail} n={len(chunks)}", flush=True)
            return jsonify({"error": f"failed to merge chunks: {e}"}), 400
        merge_ms = int((time.monotonic() - t_merge) * 1000)
        print(f"[voice] merge n={len(chunks)} bytes={len(merged_audio)} ms={merge_ms} sid_tail={sid_tail}", flush=True)

        try:
            answer, stt_used, transcript = answer_voice(merged_audio)
        except (requests.RequestException, RuntimeError):
            return jsonify({"error": "gemini request failed"}), 502

        set_latest_result(answer, source="voice_stt" if stt_used else "voice_chunks")
        return jsonify({"ok": True, "final": True,
                        "stt": "groq" if stt_used else "gemini_direct",
                        "transcript": transcript,
                        "text": answer})
    finally:
        _set_voice_busy(False)


# ---------------------------------------------------------------------
# GET /api/latest — опрос устройством, есть ли готовый ответ
# ---------------------------------------------------------------------
@app.route("/api/latest", methods=["GET"])
def get_latest():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    with _state_lock:
        # Честный ready: после провала коммита старый ответ не отдаём,
        # пока не придёт свежий (тот же гейт, что в photo-status).
        _, best = _newest_ready_slot_locked()
        if best is None or not _honest_answer_ready_locked():
            result = {"ready": False, "text": "", "source": None, "created_at": 0}
        else:
            result = {"ready": True, "text": best["text"], "source": best["source"],
                      "created_at": best["created_at"]}
            best["ready"] = False

    if result.get("ready") and result.get("source") in ("photo", "photos_burst", "photos_commit"):
        global _photo_status
        with _photo_lock:
            _photo_status = "consumed"
        print(f"[photo] latest отдан, source={result.get('source')} status=consumed", flush=True)

    response_text = json.dumps(result, ensure_ascii=False, separators=(",", ":"))
    return Response(response_text, mimetype='application/json')


# ---------------------------------------------------------------------
# POST /api/photo-frame — принять 1 JPEG в staging (без отправки в Gemini)
# ---------------------------------------------------------------------
@app.route("/api/photo-frame", methods=["POST"])
def handle_photo_frame():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    image_bytes = request.get_data()
    if not image_bytes:
        return jsonify({"error": "empty body, expected raw JPEG bytes"}), 400
    if len(image_bytes) > PHOTO_FRAME_MAX_BYTES:
        print(f"[photo] 413 frame bytes={len(image_bytes)} cap={PHOTO_FRAME_MAX_BYTES}", flush=True)
        return jsonify({"error": "frame too large"}), 413

    global _photo_status
    with _photo_lock:
        _sweep_photo_staging_locked(time.time())
        if len(_photo_staging) >= PHOTO_STAGING_CAP:
            print(f"[photo] staging full bytes={len(image_bytes)} staged={len(_photo_staging)} cap={PHOTO_STAGING_CAP}", flush=True)
            return jsonify({"error": "staging full"}), 413
        current_bytes = sum(len(b) for (b, _) in _photo_staging)
        if current_bytes + len(image_bytes) > PHOTO_BYTES_CAP:
            print(f"[photo] 413 bytes cap current={current_bytes} ingress={len(image_bytes)} cap={PHOTO_BYTES_CAP}", flush=True)
            return jsonify({"error": "staging bytes full"}), 413
        _photo_staging.append((image_bytes, time.time()))
        staged = len(_photo_staging)
        _photo_status = "staged"

    print(f"[photo] frame ingress bytes={len(image_bytes)} staged={staged}", flush=True)
    return jsonify({"ok": True, "staged": staged})


# ---------------------------------------------------------------------
# POST /api/photos/commit — отправить все staged-кадры в Gemini разом
# ---------------------------------------------------------------------
@app.route("/api/photos/commit", methods=["POST"])
def handle_photos_commit():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    global _photo_status
    if not _photo_commit_lock.acquire(blocking=False):
        return jsonify({"error": "commit already in progress"}), 429
    try:
        with _photo_lock:
            _sweep_photo_staging_locked(time.time())
            if not _photo_staging:
                return jsonify({"error": "no staged frames"}), 400
            frames = [b for (b, _) in _photo_staging]
            committed = len(frames)
            _photo_status = "sent_to_llm"

        print(f"[photo] commit, frames={len(frames)} status=sent_to_llm", flush=True)

        parts: list = [{"text": "Проанализируй эту серию из нескольких фото (сделаны одно за другим)."}]
        for frame in frames:
            parts.append({"inline_data": {"mime_type": "image/jpeg",
                                          "data": base64.b64encode(frame).decode("utf-8")}})

        t0 = time.monotonic()
        try:
            answer = call_gemini(PHOTO_SYSTEM_PROMPT, parts)
        except (requests.RequestException, RuntimeError) as e:
            dt_ms = int((time.monotonic() - t0) * 1000)
            with _photo_lock:
                _photo_status = "error"
            global _last_commit_failed, _last_commit_failed_at
            with _state_lock:
                _last_commit_failed = True
                _last_commit_failed_at = time.time()
                stale = _photo_answer["ready"] or _voice_answer["ready"]
                stale_source = _photo_answer.get("source") if _photo_answer["ready"] else _voice_answer.get("source")
            if stale:
                print(f"[photo] warn commit failed, stale answer kept source={stale_source}", flush=True)
            print(f"[gemini] commit fail err_class={type(e).__name__} ms={dt_ms} status=502", flush=True)
            return jsonify({"error": "gemini request failed"}), 502
        dt_ms = int((time.monotonic() - t0) * 1000)
        print(f"[gemini] commit ok ms={dt_ms} frames={len(frames)}", flush=True)

        total_bytes = sum(len(f) for f in frames)
        set_latest_result(answer, source="photos_commit")
        with _state_lock:
            _last_commit_failed = False
        with _photo_lock:
            del _photo_staging[:committed]
            _photo_status = "txt_ready"

        print(f"[photo] commit ok, frames={len(frames)} bytes={total_bytes} ms={dt_ms} status=txt_ready", flush=True)
        return jsonify({"ok": True, "frames": len(frames), "text": answer})
    finally:
        _photo_commit_lock.release()


# ---------------------------------------------------------------------
# GET /api/photo-status — лёгкий опрос стейджинга для microReader
# ---------------------------------------------------------------------
@app.route("/api/photo-status", methods=["GET"])
def get_photo_status():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    with _photo_lock:
        staged = len(_photo_staging)
        status = _photo_status
    with _state_lock:
        answer_ready = _honest_answer_ready_locked()
    voice_busy = _get_voice_busy()

    response_text = json.dumps({"staged": staged, "photo_status": status, "answer_ready": answer_ready, "voice_busy": voice_busy},
                               ensure_ascii=False, separators=(",", ":"))
    return Response(response_text, mimetype='application/json')


# ---------------------------------------------------------------------
@app.route("/api/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "model": GEMINI_MODEL})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("PORT", 8080)),
        help="Порт, на котором слушает сервер (по умолчанию из .env PORT, иначе 8080)",
    )
    args = parser.parse_args()

    if not GOOGLE_API_KEY:
        sys.exit("GOOGLE_API_KEY is not set")
    if DEVICE_KEY in ("", "change-me"):
        sys.exit("DEVICE_KEY is not set (placeholder value)")

    print(f"[startup] model={GEMINI_MODEL} groq_stt={GROQ_STT_ENABLED} max_content_length={MAX_CONTENT_LENGTH} "
          f"photo_max_bytes={PHOTO_MAX_BYTES} voice_max_bytes={VOICE_MAX_BYTES} "
          f"voice_max_sessions={VOICE_MAX_SESSIONS} voice_max_chunks={VOICE_MAX_CHUNKS} "
          f"voice_session_ttl={VOICE_SESSION_TTL}", flush=True)

    # ВАЖНО: threaded=True обязателен! Обработка финального чанка занимает
    # 10-25 с (Groq STT + Gemini). Без потоков однопоточный dev-сервер
    # сериализует запросы: следующие чанки ждут в очереди, у ESP32
    # срабатывают таймауты и рвутся соединения посреди загрузки.
    app.run(host="0.0.0.0", port=args.port, threaded=True)
