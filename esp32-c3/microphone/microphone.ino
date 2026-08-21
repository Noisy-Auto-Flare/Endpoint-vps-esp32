/*
 * ESP32-C3 SuperMini + MAX4466 — голосовой микрофон для проекта "Шпора"
 *
 * Схема работы (чанковая запись):
 *   1) Кнопка (GPIO9, BOOT) — старт/стоп записи
 *   2) Запись идёт чанками по 5 секунд в SPIFFS (WAV 16 кГц, 16 бит, моно)
 *   3) Каждый полный чанк сразу отправляется на сервер:
 *        POST /api/voice-chunk  + заголовки X-Session-Id / X-Chunk-Index / X-Final
 *   4) Стоп (или лимит 30 сек) — последний неполный чанк уходит с X-Final: true,
 *      сервер склеивает все чанки сессии в один WAV и отправляет в Gemini
 *   5) Ответ забирает microReader через GET /api/latest (как обычно)
 *
 * Пины:
 *   GPIO4  — OUT микрофона MAX4466 (ADC1_CH4)
 *   GPIO9  — кнопка (BOOT, INPUT_PULLUP)
 *   GPIO8  — встроенный синий LED (активный LOW)
 *
 * Питание микрофона: VCC → 3V3 (или 5V), GND → GND, OUT → GPIO4
 */

#include <WiFi.h>
#include <SPIFFS.h>
#include "config.h"

// ============================ ПИНЫ ============================

#define MIC_PIN        4
#define BUTTON_PIN     9
#define LED_PIN        8

// ========================= ПАРАМЕТРЫ ==========================

#define SAMPLE_RATE       16000
#define CHUNK_SECONDS     5          // длительность одного чанка
#define MAX_SECONDS       30         // жёсткий лимит записи
#define SAMPLES_PER_CHUNK (SAMPLE_RATE * CHUNK_SECONDS)   // 80000 сэмплов = 160000 байт
#define MAX_SAMPLES       (SAMPLE_RATE * MAX_SECONDS)     // 480000 сэмплов

// LED на SuperMini светится при LOW
#define LED_ON()   digitalWrite(LED_PIN, LOW)
#define LED_OFF()  digitalWrite(LED_PIN, HIGH)

// =========================== СТАТУС ===========================

volatile bool isRecording = false;
volatile bool needFinalize = false;   // кнопка стоп нажата
volatile bool maxTimeReached = false; // лимит времени вышел

File audioFile;
String sessionId;
int chunkIndex = 0;
unsigned long samplesInChunk = 0;
unsigned long totalSamples = 0;

WiFiClient httpClient;

// ======================== WAV-ЗАГОЛОВОК ========================

void writeWavHeader(File &file, uint32_t dataSize) {
  uint32_t fileSize = dataSize + 36;
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t numChannels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  uint16_t blockAlign = numChannels * bitsPerSample / 8;

  file.seek(0);
  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&fileSize, 4);
  file.write((const uint8_t*)"WAVE", 4);
  file.write((const uint8_t*)"fmt ", 4);

  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1;
  file.write((uint8_t*)&subchunk1Size, 4);
  file.write((uint8_t*)&audioFormat, 2);
  file.write((uint8_t*)&numChannels, 2);
  file.write((uint8_t*)&sampleRate, 4);
  file.write((uint8_t*)&byteRate, 4);
  file.write((uint8_t*)&blockAlign, 2);
  file.write((uint8_t*)&bitsPerSample, 2);

  file.write((const uint8_t*)"data", 4);
  file.write((uint8_t*)&dataSize, 4);
}

// =========================== КНОПКА ===========================

void IRAM_ATTR buttonISR() {
  static unsigned long lastPress = 0;
  if (millis() - lastPress < 400) return;   // антидребезг
  lastPress = millis();

  if (!isRecording) {
    isRecording = true;
    needFinalize = false;
    maxTimeReached = false;
    samplesInChunk = 0;
    totalSamples = 0;
    chunkIndex = 0;
    sessionId = String(millis()) + String(random(1000, 9999));
    Serial.println(">>> Запись НАЧАЛАСЬ, сессия: " + sessionId);
  } else {
    isRecording = false;
    needFinalize = true;
    Serial.println(">>> Запись ОСТАНОВЛЕНА");
  }
}

// ====================== ПАРСИНГ ОТВЕТА ======================

// Достаёт значение строкового поля из JSON-ответа сервера.
// Простая ручная распаковка (как в microReader): кавычки, \n и \\ понимает.
String extractJsonStr(const String &json, const char *key) {
  String pat = String("\"") + key + "\":\"";
  int start = json.indexOf(pat);
  if (start < 0) return "";
  start += pat.length();
  String out = "";
  for (int i = start; i < (int)json.length(); i++) {
    char c = json[i];
    if (c == '\\') {                    // экранированный символ
      if (i + 1 < (int)json.length()) {
        char n = json[++i];
        if (n == 'n') out += '\n';
        else if (n == 't') out += '\t';
        else out += n;                  // \" \\ и прочее -> сам символ
      }
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }
  return out;
}

// ===================== ЧТЕНИЕ ОТВЕТА =====================

// Читает полный HTTP-ответ от сервера и печатает отчёт.
// expectReport=true — финальный запрос: печатаем путь распознавания,
// транскрипт записи и ответ нейронки. Возвращает true при HTTP 200.
bool readResponseAndReport(bool expectReport) {
  // Ждём ПЕРВЫЙ БАЙТ: финал обрабатывается 10-25 с (Groq + Gemini)
  unsigned long startMs = millis();
  while (!httpClient.available() && httpClient.connected() &&
         millis() - startMs < 20000) {
    delay(10);
  }

  // Читаем всё остальное: пока идут данные или сокет жив
  String body = "";
  unsigned long lastData = millis();
  uint8_t rbuf[512];
  while (httpClient.connected() || httpClient.available()) {
    if (httpClient.available()) {
      int n = httpClient.read(rbuf, sizeof(rbuf));
      if (n > 0) {
        body.concat((const char*)rbuf, n);
        lastData = millis();
      } else {
        delay(5);              // read() вернул -1 — данных в этот раз нет
      }
      continue;
    }
    if (millis() - lastData > 3000) break;   // тишина 3 с после потока — всё
    delay(10);
  }
  httpClient.stop();

  // Статус ищем по всему телу ("HTTP/1.1 200 OK") — без привязки к префиксу
  bool ok = (body.indexOf(" 200 ") >= 0);
  Serial.printf("Ответ за %lu мс, тело %u байт -> %s\n",
                millis() - startMs, body.length(), ok ? "OK" : "ОШИБКА");

  if (!ok) {
    Serial.print("[debug] первые байты: ");
    Serial.println(body.length() ? body.substring(0, 80) : String("(пусто)"));
    return false;
  }

  if (expectReport) {
    String sttPath    = extractJsonStr(body, "stt");
    String transcript = extractJsonStr(body, "transcript");
    String answerText = extractJsonStr(body, "text");

    Serial.println("========================================");
    Serial.println("Путь распознавания: " +
                   (sttPath.length() ? sttPath : String("(нет данных)")));
    Serial.println("--- ЧТО РАСПОЗНАНО ИЗ ЗАПИСИ ---");
    Serial.println(transcript.length() ? transcript : "(пусто)");
    if (answerText.length()) {
      Serial.println("--- ОТВЕТ НЕЙРОНКИ (первые 400 симв.) ---");
      if (answerText.length() > 400) {
        Serial.println(answerText.substring(0, 400) + "...");
      } else {
        Serial.println(answerText);
      }
    }
    Serial.println("========================================");
  }

  return true;
}

// ===================== ОТПРАВКА ЧАНКА =====================

// Отправляет файл /chunk.wav как raw body (не base64) на /api/voice-chunk.
// ВАЖНО: файл открывается заново внутри — к моменту вызова он уже
// закрыт после записи (writeWavHeader + close).
// Возвращает true при HTTP 200.
bool sendChunk(bool isFinal) {
  File f = SPIFFS.open("/chunk.wav", "r");
  if (!f) {
    Serial.println("ОШИБКА: не удалось открыть /chunk.wav для отправки");
    return false;
  }
  uint32_t fileSize = f.size();

  Serial.printf("Отправка чанка %d (%s), %lu байт...\n",
                chunkIndex, isFinal ? "FINAL" : "chunk", fileSize);

  if (!httpClient.connect(BACKEND_HOST, BACKEND_PORT, 5000)) {
    Serial.println("ОШИБКА: не удалось подключиться к серверу");
    f.close();
    return false;
  }
  // ВАЖНО: слабый Wi-Fi + большой чанк -> блокирующая отправка дольше 5 с.
  // Без этого сокет рвётся по внутреннему таймауту прямо посреди загрузки.
  httpClient.setTimeout(30000);

  // --- HTTP-заголовки ---
  httpClient.printf("POST /api/voice-chunk HTTP/1.1\r\n");
  httpClient.printf("Host: %s:%d\r\n", BACKEND_HOST, BACKEND_PORT);
  httpClient.printf("X-Device-Key: %s\r\n", BACKEND_DEVICE_KEY);
  httpClient.printf("X-Session-Id: %s\r\n", sessionId.c_str());
  httpClient.printf("X-Chunk-Index: %d\r\n", chunkIndex);
  httpClient.printf("X-Final: %s\r\n", isFinal ? "true" : "false");
  httpClient.printf("Content-Type: audio/wav\r\n");
  httpClient.printf("Content-Length: %lu\r\n", fileSize);
  httpClient.printf("Connection: close\r\n\r\n");

  // --- Стрим WAV-байтов из файла (с контролем каждой записи) ---
  f.seek(0);
  uint8_t buf[4096];
  size_t totalSent = 0;
  bool sendFail = false;
  unsigned long upStart = millis();
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    size_t written = httpClient.write(buf, n);
    totalSent += written;
    delay(1);   // отдаём слот WiFi-стеку: без этого длинная блокирующая
                // отправка может уронить lwIP (assert xQueueSemaphoreTake)
    if (written != (size_t)n) {
      Serial.printf("ОШИБКА отправки: записано %u из %d байт (итого %u/%u)\n",
                    (unsigned)written, n, (unsigned)totalSent, (unsigned)fileSize);
      sendFail = true;
      break;
    }
  }
  httpClient.flush();
  f.close();

  if (sendFail || totalSent != fileSize) {
    Serial.printf("Загрузка оборвалась: отправлено %u из %u байт за %lu мс\n",
                  (unsigned)totalSent, (unsigned)fileSize, millis() - upStart);
    Serial.print("[debug] состояние сокета connected=");
    Serial.println(httpClient.connected());
    httpClient.stop();
    return false;
  }

  // --- Читаем ответ и печатаем отчёт (транскрипт/ответ — только для финала) ---
  return readResponseAndReport(isFinal);
}

// Сессия завершилась МЕЖДУ чанками (стоп нажат во время отправки предыдущего):
// уведомляем сервер пустым финальным маркером, чтобы он склеил накопленное.
void sendFinalMarkerOnly() {
  Serial.println("Стоп между чанками — отправляю финальный маркер...");

  if (!httpClient.connect(BACKEND_HOST, BACKEND_PORT, 5000)) {
    Serial.println("ОШИБКА: не удалось подключиться к серверу");
    blinkLed(2, 100, 100);
    return;
  }
  httpClient.setTimeout(30000);

  httpClient.printf("POST /api/voice-chunk HTTP/1.1\r\n");
  httpClient.printf("Host: %s:%d\r\n", BACKEND_HOST, BACKEND_PORT);
  httpClient.printf("X-Device-Key: %s\r\n", BACKEND_DEVICE_KEY);
  httpClient.printf("X-Session-Id: %s\r\n", sessionId.c_str());
  httpClient.printf("X-Chunk-Index: %d\r\n", chunkIndex);
  httpClient.printf("X-Final: true\r\n");
  httpClient.printf("Content-Type: audio/wav\r\n");
  httpClient.printf("Content-Length: 0\r\n");
  httpClient.printf("Connection: close\r\n\r\n");

  // Тот же полный отчёт: транскрипт накопленного + ответ нейронки
  if (readResponseAndReport(true)) {
    blinkLed(3, 120, 120);   // успех
    Serial.println("=== Запись завершена (маркер), всё отправлено ===");
  } else {
    blinkLed(2, 100, 100);   // ошибка
    Serial.println("Маркер не прошёл");
  }
}

// ====================== ИНДИКАЦИЯ LED ======================

void blinkLed(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    LED_ON();
    delay(onMs);
    LED_OFF();
    delay(offMs);
  }
}

// ========================== SETUP ==========================

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== ESP32-C3 Voice Chunks ===");
  Serial.printf("Свободная память при старте: %d байт\n", ESP.getFreeHeap());

  pinMode(LED_PIN, OUTPUT);
  LED_OFF();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  if (!SPIFFS.begin(true)) {
    Serial.println("Ошибка SPIFFS!");
    blinkLed(5, 200, 200);   // SOS: 5 длинных миганий
    while (1) delay(1000);
  }

  // Удаляем старый файл-чанк
  if (SPIFFS.exists("/chunk.wav")) {
    SPIFFS.remove("/chunk.wav");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Подключение к Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(400);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nОШИБКА: нет Wi-Fi! Перезагрузка...");
    blinkLed(5, 100, 100);
    ESP.restart();
  }
  Serial.println();
  Serial.print("IP адрес: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Свободная память после Wi-Fi: %d байт\n", ESP.getFreeHeap());

  Serial.println("Готов к работе. Кнопка = старт/стоп записи.");
}

// =========================== LOOP ==========================

void loop() {
  if (!isRecording) {
    // Стоп мог быть нажат ПОСЛЕ завершения чанка (во время его отправки):
    // закрываем сессию финальным маркером без данных.
    if (needFinalize) {
      needFinalize = false;
      if (chunkIndex > 0) {
        sendFinalMarkerOnly();
        isRecording = false;
      }
    }
    delay(10);
    return;
  }

  // === Старт нового чанка ===
  if (samplesInChunk == 0) {
    audioFile = SPIFFS.open("/chunk.wav", FILE_WRITE);
    if (!audioFile) {
      Serial.println("Не удалось открыть файл чанка!");
      isRecording = false;
      blinkLed(2, 100, 100);   // 2 мига = ошибка
      return;
    }
    // Резервируем место под заголовок (44 байта)
    uint8_t header[44] = {0};
    audioFile.write(header, 44);
    LED_ON();   // запись идёт
  }

  // === Запись сэмплов с точным таймингом ===
  unsigned long nextSampleUs = micros();
  while (isRecording && samplesInChunk < SAMPLES_PER_CHUNK) {
    // Держим частоту ровно 16 кГц (период 62.5 мкс)
    long waitUs = nextSampleUs - (long)micros();
    if (waitUs > 0) {
      delayMicroseconds(waitUs);
      continue;
    }
    nextSampleUs += 1000000UL / SAMPLE_RATE;

    int raw = analogRead(MIC_PIN);
    int16_t sample = (int16_t)((raw - 2048) * 12);   // центрирование + усиление
    audioFile.write((uint8_t*)&sample, 2);
    samplesInChunk++;
    totalSamples++;

    // Лимит 30 секунд — принудительный финал
    if (totalSamples >= MAX_SAMPLES) {
      maxTimeReached = true;
      isRecording = false;
      break;
    }
  }

  // === Кнопка стоп или лимит времени ===
  bool chunkComplete = (samplesInChunk >= SAMPLES_PER_CHUNK);
  bool stopPressed = needFinalize;

  if (chunkComplete || stopPressed || maxTimeReached) {
    LED_OFF();
    needFinalize = false;

    uint32_t dataSize = samplesInChunk * 2;
    writeWavHeader(audioFile, dataSize);
    audioFile.close();
    Serial.printf("Чанк сохранён: %lu сэмплов (%lu байт)\n",
                  samplesInChunk, dataSize + 44);

    // Финальный чанк: запись остановлена ИЛИ достигнут лимит времени
    bool isFinal = stopPressed || maxTimeReached;

    bool ok = sendChunk(isFinal);
    if (!ok) {
      blinkLed(2, 100, 100);   // 2 мига = ошибка отправки
      Serial.println("Чанк НЕ отправлен. Запись прекращена.");
      isRecording = false;
      return;
    }

    if (isFinal) {
      blinkLed(3, 120, 120);   // 3 мига = успех
      Serial.println("=== Запись завершена, всё отправлено ===");
      isRecording = false;
      return;
    }

    // Переходим к следующему чанку
    chunkIndex++;
    samplesInChunk = 0;

    // Маленькая пауза, чтобы файл освободился
    delay(50);
    if (SPIFFS.exists("/chunk.wav")) {
      SPIFFS.remove("/chunk.wav");
    }
  }
}
