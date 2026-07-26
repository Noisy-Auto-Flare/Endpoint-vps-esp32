"""
Бэкенд-посредник между ESP32-CAM / microReader и Google AI Studio (Gemini API).

Как это работает:
  1) ESP32-CAM шлёт JPEG на POST /api/photo
     -> сервер отправляет его в Gemini (vision) через Google AI Studio
     -> текстовый ответ сохраняется как "последний результат"

  2) microReader (с микрофоном) шлёт WAV на POST /api/voice
     -> сервер отправляет аудио НАПРЯМУЮ в Gemini (модель понимает звук без
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
import os
import threading
import time

from flask import Flask, request, jsonify
import requests

# ============================ CONFIG ================================

GOOGLE_API_KEY = os.environ.get("GOOGLE_API_KEY", "")
DEVICE_KEY = os.environ.get("DEVICE_KEY", "change-me")

GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.1-flash-lite")

GEMINI_BASE = "https://generativelanguage.googleapis.com/v1beta"

# Системный промпт для голосовых запросов (можно менять под задачу)
VOICE_SYSTEM_PROMPT = os.environ.get(
    "VOICE_SYSTEM_PROMPT",
    "Ты — голосовой ассистент на маленьком устройстве с экраном 128x64 (8 строк по 21 символу). "
    "Тебе присылают аудио с голосовым сообщением пользователя. Ответь на то, что он сказал/спросил. "
    "Отвечай кратко, по существу, без markdown-разметки, желательно уложиться в 2-3 коротких предложения."
)

# Системный промпт для фото-запросов
PHOTO_SYSTEM_PROMPT = os.environ.get(
    "PHOTO_SYSTEM_PROMPT",
    "Опиши, что изображено на фото, кратко и по существу, без markdown-разметки. "
    "Уложись в 2-3 коротких предложения, ответ будет показан на маленьком монохромном экране."
)

# =====================================================================

app = Flask(__name__)

# Простое общее хранилище "последнего результата" (для одного устройства/связки).
# Защищено локом, потому что параллельные запросы теоретически возможны.
_state_lock = threading.Lock()
_state = {
    "ready": False,
    "text": "",
    "source": None,      # "photo" или "voice"
    "created_at": 0,
}


def require_device_key():
    key = request.headers.get("X-Device-Key", "")
    return key == DEVICE_KEY


def set_latest_result(text: str, source: str):
    with _state_lock:
        _state["ready"] = True
        _state["text"] = text
        _state["source"] = source
        _state["created_at"] = time.time()


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
    text_chunks = [p.get("text", "") for p in content_parts if "text" in p]
    return "".join(text_chunks).strip()


# ---------------------------------------------------------------------
# POST /api/photo — принимает JPEG (сырые байты в теле запроса)
# ---------------------------------------------------------------------
@app.route("/api/photo", methods=["POST"])
def handle_photo():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    image_bytes = request.get_data()
    if not image_bytes:
        return jsonify({"error": "empty body, expected raw JPEG bytes"}), 400

    image_b64 = base64.b64encode(image_bytes).decode("utf-8")

    try:
        answer = call_gemini(
            PHOTO_SYSTEM_PROMPT,
            [
                {"text": "Проанализируй это фото."},
                {"inline_data": {"mime_type": "image/jpeg", "data": image_b64}},
            ],
        )
    except (requests.RequestException, RuntimeError) as e:
        return jsonify({"error": f"gemini request failed: {e}"}), 502

    set_latest_result(answer, source="photo")
    return jsonify({"ok": True, "text": answer})


# ---------------------------------------------------------------------
# POST /api/voice — принимает WAV (сырые байты в теле запроса)
# ---------------------------------------------------------------------
@app.route("/api/voice", methods=["POST"])
def handle_voice():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    audio_bytes = request.get_data()
    if not audio_bytes:
        return jsonify({"error": "empty body, expected raw WAV bytes"}), 400

    audio_b64 = base64.b64encode(audio_bytes).decode("utf-8")

    try:
        answer = call_gemini(
            VOICE_SYSTEM_PROMPT,
            [
                {"inline_data": {"mime_type": "audio/wav", "data": audio_b64}},
            ],
        )
    except (requests.RequestException, RuntimeError) as e:
        return jsonify({"error": f"gemini request failed: {e}"}), 502

    set_latest_result(answer, source="voice")
    return jsonify({"ok": True, "text": answer})


# ---------------------------------------------------------------------
# GET /api/latest — опрос устройством, есть ли готовый ответ
# ---------------------------------------------------------------------
@app.route("/api/latest", methods=["GET"])
def get_latest():
    if not require_device_key():
        return jsonify({"error": "unauthorized"}), 401

    with _state_lock:
        result = dict(_state)
        # После того как устройство забрало результат - помечаем как неготовый,
        # чтобы одно и то же сообщение не показывалось повторно при следующем опросе.
        if _state["ready"]:
            _state["ready"] = False

    return jsonify(result)


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
        print("ВНИМАНИЕ: переменная окружения GOOGLE_API_KEY не задана!")

    app.run(host="0.0.0.0", port=args.port)
