/*
 * ESP32-C3 SuperMini + MAX4466 — голосовой микрофон проекта "Шпора"
 * Версия 2: эталонная архитектура на FreeRTOS с аппаратным DMA-семплингом.
 *
 * ПОЧЕМУ ТАК (по итогам исследования эталонных решений):
 *   1) analogRead() занимает ~80-100 мкс — это МЕДЛЕННЕЕ периода 16 кГц
 *      (62.5 мкс). Программный семплинг физически не успевает -> звук
 *      пишется на заниженной частоте, питч съезжает, Whisper страдает.
 *      Эталон для ESP32-C3 — драйвер adc_continuous: АЦП семплирует сам,
 *      данные уходят в память по DMA без участия CPU.
 *   2) На C3 НЕТ режима I2S-ADC (это только классический ESP32).
 *   3) SPIFFS не потокобезопасен — все операции под мьютексом.
 *
 * ПОТОКИ:
 *   - samplerTask: ждёт кнопку СТАРТ, непрерывно читает кадры DMA,
 *     конвертирует в PCM и пишет в WAV-файлы чанками по 5 сек
 *     (уникальное имя на чанк: /c0.wav ... /c5.wav). Записал чанк ->
 *     в очередь отправки -> СРАЗУ пишет следующий. Речь не прерывается.
 *   - loopTask: берёт готовые файлы из очереди и отправляет на сервер.
 *
 * ПОТОК ДАННЫХ:
 *   MIC -> [DMA] -> кольцевой буфер -> активный WAV в SPIFFS
 *       -> [очередь] -> HTTP POST /api/voice-chunk -> сервер склеивает
 *       -> Groq STT -> Gemini -> ответ через GET /api/latest на microReader.
 *
 * Пины:
 *   GPIO4  — OUT микрофона MAX4466 (ADC1_CH4)
 *   GPIO9  — кнопка (BOOT): старт/стоп записи
 *   GPIO8  — встроенный синий LED: один длинный миг = старт записи
 *            (постоянно НЕ горит: LED_SOLID_WHILE_RECORDING = false);
 *            2 мига = ошибка отправки; 3 мига = сессия успешно завершена;
 *            5 миганий = фатальная ошибка (SPIFFS/Wi-Fi)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_timer.h>
#include <esp_adc/adc_continuous.h>
#include "config.h"

// ============================ ПИНЫ И ПАРАМЕТРЫ ============================

constexpr int      MIC_PIN       = 4;      // ADC1_CH4 на ESP32-C3
constexpr int      BUTTON_PIN    = 9;      // BOOT, INPUT_PULLUP
constexpr int      LED_PIN       = 8;      // SuperMini: светится при LOW

constexpr uint32_t SAMPLE_RATE   = 16000;  // Гц — держится железом (DMA)
constexpr uint32_t CHUNK_SECONDS = 5;      // длительность одного чанка
// Лимит ОДНОЙ записи: урок с запасом. В режиме урока сервер копит одну
// сессию в один WAV, поэтому нажал в начале урока — отпустил в конце.
constexpr uint32_t MAX_SECONDS   = 7200;   // 2 часа
constexpr int16_t  ADC_CENTER    = 2048;   // середина шкалы 12-бит ADC
// Усиление: если Whisper слышит глухо — увеличь (16-24); если хрипы — уменьши.
constexpr int32_t  MIC_GAIN      = 12;

constexpr uint32_t SAMPLES_PER_CHUNK = SAMPLE_RATE * CHUNK_SECONDS;
constexpr uint32_t MAX_TOTAL_SAMPLES = SAMPLE_RATE * MAX_SECONDS;

#ifndef SOC_ADC_DIGI_RESULT_BYTES
#define SOC_ADC_DIGI_RESULT_BYTES 4   // байт на результат преобразования (C3, TYPE2)
#endif
constexpr size_t FRAME_SAMPLES = 256;                                  // сэмплов в кадре DMA
constexpr size_t FRAME_BYTES   = FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES;

constexpr size_t RING_SIZE_BYTES = 16384;              // ≈0.5 с звука запаса

// Имена файлов УНИКАЛЬНЫ для каждого чанка сессии (/c0.wav ... /cN.wav):
// при медленном Wi-Fi загрузка чанка может идти дольше записи следующего,
// и переиспользование имён (ping-pong) приводило к удалению файла,
// открытого загрузчиком -> порча SPIFFS.
constexpr uint32_t MAX_CHUNKS = MAX_SECONDS / CHUNK_SECONDS;   // 2ч / 5с = 1440
// Очередь на отправку ограничена RAM/SPIFFS (8 * 160 КБ ≈ 1.3 МБ), а не
// длиной сессии: файл чанка удаляется сразу после успешной отправки.
constexpr uint32_t UPLOAD_QUEUE_LEN = 8;
// Повторы отправки: один сбой на школьном хотспоте не должен терять чанк
// (дыра в аудио остаётся навсегда). Повторяем только при ошибке.
constexpr int      UPLOAD_RETRIES         = 3;
constexpr uint32_t UPLOAD_RETRY_PAUSE_MS  = 2000;
// Постоянное свечение LED во время записи. По умолчанию ВЫКЛ: урок идёт
// 40-90 минут, горящий светодиод демаскирует устройство перед учителем.
constexpr bool LED_SOLID_WHILE_RECORDING = false;
constexpr const char* LEGACY_CHUNK_FILES[] = { "/chk_a.wav", "/chk_b.wav" };

// ============================ СОСТОЯНИЕ ===================================
// ВАЖНО: типы объявлены ДО любых функций — автопрототипы Arduino IDE
// вставляются перед первой функцией файла и требуют готовых типов.

enum class BtnCmd : uint8_t { NONE = 0, START = 1, STOP = 2 };
volatile BtnCmd isrRequest = BtnCmd::NONE;      // пишет только ISR

struct UploadJob {
  char file[16];          // путь в SPIFFS; "" => пустой финальный маркер
  char sid[24];           // копия session id (защита от гонки сессий)
  int  index;
  bool finalChunk;
};

static QueueHandle_t       cmdQueue;      // loopTask -> samplerTask
static QueueHandle_t       uploadQueue;   // samplerTask -> loopTask
static SemaphoreHandle_t   spiffsMutex;   // SPIFFS не потокобезопасен!

volatile bool recordingActive = false;

WiFiClient httpClient;

// ======================= ПРОТОТИПЫ ФУНКЦИЙ ================================
// Явные прототипы: авто-генерация Arduino IDE ненадёжна
// для функций с кастомными типами в сигнатуре.

void   blinkLed(int times, int onMs, int offMs);
String extractJsonStr(const String& json, const char* key);
void   writeWavHeader(File& f, uint32_t dataSizeBytes);
bool   openChunkFile(File& f, const char* path);
void   finalizeChunkFile(File& f, uint32_t pcmBytes);
void   samplerTask(void* arg);
bool   readResponseAndReport(bool expectReport);
bool   sendJob(const UploadJob& job);
void   IRAM_ATTR buttonISR();

// ============================ LED-ХЕЛПЕРЫ =================================

inline void ledOn()  { digitalWrite(LED_PIN, LOW); }
inline void ledOff() { digitalWrite(LED_PIN, HIGH); }

// ============================ УТИЛИТЫ =====================================

void blinkLed(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    ledOn(); delay(onMs);
    ledOff(); delay(offMs);
  }
}

String extractJsonStr(const String& json, const char* key) {
  const String pat = String("\"") + key + "\":\"";
  int start = json.indexOf(pat);
  if (start < 0) return "";
  start += pat.length();
  String out;
  out.reserve(256);
  for (int i = start; i < (int)json.length(); i++) {
    const char c = json[i];
    if (c == '\\') {
      if (i + 1 < (int)json.length()) {
        const char n = json[++i];
        if (n == 'n') out += '\n';
        else if (n == 't') out += '\t';
        else out += n;
      }
    } else if (c == '"') break;
    else out += c;
  }
  return out;
}

void writeWavHeader(File& f, uint32_t dataSizeBytes) {
  constexpr uint16_t CHANNELS = 1, BITS = 16;
  const uint32_t byteRate   = SAMPLE_RATE * CHANNELS * BITS / 8;
  const uint16_t blockAlign = CHANNELS * BITS / 8;

  f.seek(0);
  f.write((const uint8_t*)"RIFF", 4);
  uint32_t v = dataSizeBytes + 36;  f.write((uint8_t*)&v, 4);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  v = 16;                           f.write((uint8_t*)&v, 4);
  uint16_t u = 1;                   f.write((uint8_t*)&u, 2);   // PCM
  u = CHANNELS;                     f.write((uint8_t*)&u, 2);
  v = SAMPLE_RATE;                  f.write((uint8_t*)&v, 4);
  v = byteRate;                     f.write((uint8_t*)&v, 4);
  u = blockAlign;                   f.write((uint8_t*)&u, 2);
  u = BITS;                         f.write((uint8_t*)&u, 2);
  f.write((const uint8_t*)"data", 4);
  v = dataSizeBytes;                f.write((uint8_t*)&v, 4);
}

// ============================ ADC (DMA) ===================================

static adc_continuous_handle_t adcHandle = nullptr;

// Сырое 12-бит значение -> PCM16: центрирование, усиление, клиппинг
static inline int16_t adcToPcm(uint16_t raw) {
  int32_t s = ((int32_t)raw - ADC_CENTER) * (int32_t)MIC_GAIN;
  if (s > 32767)  s = 32767;
  if (s < -32768) s = -32768;
  return (int16_t)s;
}

static void adcInit() {
  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = FRAME_BYTES * 4;
  hcfg.conv_frame_size    = FRAME_BYTES;
  ESP_ERROR_CHECK(adc_continuous_new_handle(&hcfg, &adcHandle));

  // Официальный маппинг пин -> (unit, channel); GPIO4 -> ADC1_CH4 на C3
  adc_unit_t unit = ADC_UNIT_1;
  adc_channel_t ch = ADC_CHANNEL_4;
  ESP_ERROR_CHECK(adc_continuous_io_to_channel(MIC_PIN, &unit, &ch));

  adc_digi_pattern_config_t pattern = {};
  pattern.atten     = ADC_ATTEN_DB_11;    // полный диапазон до ~3.1 В
  pattern.bit_width = ADC_BITWIDTH_12;
  pattern.unit      = unit;
  pattern.channel   = ch;

  adc_continuous_config_t cfg = {};
  cfg.sample_freq_hz = SAMPLE_RATE;
  cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
  cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;   // ТОЛЬКО так на C3
  cfg.pattern_num    = 1;
  cfg.adc_pattern    = &pattern;
  ESP_ERROR_CHECK(adc_continuous_config(adcHandle, &cfg));
}

static inline void adcStartCapture() {
  ESP_ERROR_CHECK(adc_continuous_start(adcHandle));
}

static inline void adcStopCapture() {
  adc_continuous_stop(adcHandle);
}

// ====================== КОЛЬЦЕВОЙ БУФЕР PCM ===============================

// Один производитель и один потребитель — оба в samplerTask, без блокировок.
static uint8_t ring[RING_SIZE_BYTES];
static size_t  ringHead = 0, ringTail = 0, ringUsed = 0;
static uint32_t ringOverflows = 0;   // байты, потерянные при переполнении кольца

// Возвращает сколько байт реально принято (переполнение — дропаем НОВОЕ,
// а не затираем уже записанное: иначе тихо портится весь чанк).
inline size_t ringPush(const uint8_t* data, size_t len) {
  const size_t freeBytes = (ringUsed >= RING_SIZE_BYTES) ? 0 : (RING_SIZE_BYTES - ringUsed);
  const size_t take = (len <= freeBytes) ? len : freeBytes;
  for (size_t i = 0; i < take; i++) {
    ring[ringHead] = data[i];
    ringHead = (ringHead + 1) % RING_SIZE_BYTES;
  }
  ringUsed += take;
  if (take < len) ringOverflows += (len - take);
  return take;
}

inline size_t ringPop(uint8_t* dst, size_t maxLen) {
  const size_t n = min(maxLen, ringUsed);
  for (size_t i = 0; i < n; i++) {
    dst[i] = ring[ringTail];
    ringTail = (ringTail + 1) % RING_SIZE_BYTES;
  }
  ringUsed -= n;
  return n;
}

// ============================ ЗАДАЧА ЗАПИСИ ===============================

bool openChunkFile(File& f, const char* path) {
  xSemaphoreTake(spiffsMutex, portMAX_DELAY);
  if (SPIFFS.exists(path)) SPIFFS.remove(path);
  bool ok = (bool)(f = SPIFFS.open(path, FILE_WRITE));
  if (ok) {
    static const uint8_t zeros[44] = {0};
    ok = f.write(zeros, 44) == 44;
  }
  xSemaphoreGive(spiffsMutex);
  if (!ok && f) f.close();
  return ok;
}

void finalizeChunkFile(File& f, uint32_t pcmBytes) {
  xSemaphoreTake(spiffsMutex, portMAX_DELAY);
  writeWavHeader(f, pcmBytes);
  f.close();
  xSemaphoreGive(spiffsMutex);
}

void samplerTask(void*) {
  // Кадр DMA выровнен по границе 4 байт (требование DMA-драйвера)
  static uint8_t frame[FRAME_BYTES] __attribute__((aligned(4)));

  for (;;) {  // жизнь задачи: спим до START, потом пишем до STOP
    BtnCmd cmd;
    xQueueReceive(cmdQueue, &cmd, portMAX_DELAY);
    if (cmd != BtnCmd::START) continue;

    // Страховка от гонки имён: пока чанки прошлой записи не ушли — новую не начинаем.
    if (uxQueueMessagesWaiting(uploadQueue) > 0) {
      Serial.println("Подожди: догружаются чанки прошлой записи, старт отклонён");
      blinkLed(2, 100, 100);
      continue;
    }

    // --- Старт сессии (все переменные локальны — гонок с новой сессией нет) ---
    String sessionId = String((unsigned long)(esp_timer_get_time() / 1000000ULL)) +
                       String(random(1000, 9999));
    int chunkIndex = 0;
    ringHead = ringTail = ringUsed = 0;

    adcStartCapture();

    // Прогрев конвейера DMA: выбрасываем первый кадр (может содержать мусор)
    uint32_t got = 0;
    adc_continuous_read(adcHandle, frame, sizeof(frame), &got, pdMS_TO_TICKS(500));

    recordingActive = true;
    if (LED_SOLID_WHILE_RECORDING) {
      ledOn();
    } else {
      blinkLed(1, 300, 0);   // один длинный миг = запись началась (LED не горит постоянно)
    }
    Serial.println(">>> Запись НАЧАЛАСЬ, сессия: " + sessionId);

    bool stopping = false;
    bool sessionHasAudio = false;   // ушёл ли хоть один чанк с данными

    while (!stopping) {
      char path[12];
      snprintf(path, sizeof(path), "/c%d.wav", chunkIndex);

      File f;
      if (!openChunkFile(f, path)) {
        Serial.println("ФАТАЛЬНО: не удалось открыть файл чанка");
        adcStopCapture();
        recordingActive = false;
        ledOff();
        blinkLed(5, 200, 200);
        goto sessionEnd;
      }

      // --- Семплирование одного чанка ---
      uint32_t chunkSamples = 0;
      while (chunkSamples < SAMPLES_PER_CHUNK) {
        // Реакция на стоп — между кадрами DMA (латентность ≤ 16 мс)
        if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE &&
            cmd == BtnCmd::STOP) {
          stopping = true;
          break;
        }

        const esp_err_t err = adc_continuous_read(
            adcHandle, frame, sizeof(frame), &got, pdMS_TO_TICKS(200));
        if (err != ESP_OK || got == 0) continue;   // таймаут — просто дальше

        // Кадр TYPE2 -> PCM16 -> кольцевой буфер
        for (size_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got;
             i += SOC_ADC_DIGI_RESULT_BYTES) {
          const adc_digi_output_data_t* p =
              (const adc_digi_output_data_t*)&frame[i];
          const int16_t pcm = adcToPcm(p->type2.data);
          ringPush((const uint8_t*)&pcm, sizeof(pcm));
        }
        chunkSamples += got / SOC_ADC_DIGI_RESULT_BYTES;

        // Сливаем буфер в файл (все операции SPIFFS — под мьютексом)
        uint8_t io[512];
        xSemaphoreTake(spiffsMutex, portMAX_DELAY);
        size_t n;
        while ((n = ringPop(io, sizeof(io))) > 0) f.write(io, n);
        xSemaphoreGive(spiffsMutex);

        // Жёсткий лимит длины всей записи
        if ((uint32_t)chunkIndex * SAMPLES_PER_CHUNK + chunkSamples >=
            MAX_TOTAL_SAMPLES) {
          stopping = true;
          break;
        }
      }

      // --- Финализируем чанк и ставим в очередь отправки ---
      finalizeChunkFile(f, chunkSamples * 2);

      if (ringOverflows > 0) {   // не молчим о потерянных байтах аудио
        Serial.printf("Внимание: кольцо переполнено, потеряно %lu байт в чанке %d\n",
                      (unsigned long)ringOverflows, chunkIndex);
        ringOverflows = 0;
      }

      const bool nothingRecorded = (chunkSamples == 0);

      // Случай "стоп в момент ровно между чанками, данных нет вообще":
      // пустую сессию не отправляем вовсе.
      if (!(nothingRecorded && !sessionHasAudio)) {
        UploadJob job{};
        if (!nothingRecorded) {
          strncpy(job.file, path, sizeof(job.file) - 1);
          sessionHasAudio = true;
        }
        strncpy(job.sid, sessionId.c_str(), sizeof(job.sid) - 1);
        job.index = chunkIndex;
        job.finalChunk = stopping;
        xQueueSend(uploadQueue, &job, portMAX_DELAY);

        Serial.printf("Чанк %d готов (%lu байт)%s\n", job.index,
                      (unsigned long)(nothingRecorded ? 44 : chunkSamples * 2 + 44),
                      job.finalChunk ? " [FINAL]" : "");
      } else {
        Serial.println("Нечего отправлять: запись почти мгновенно остановлена.");
      }

      chunkIndex++;
    }

    adcStopCapture();
    recordingActive = false;
    if (LED_SOLID_WHILE_RECORDING) ledOff();
    Serial.println(">>> Запись завершена");

sessionEnd:;
  }
}

// ========================= ОТПРАВКА НА СЕРВЕР =============================

bool readResponseAndReport(bool expectReport) {
  // Худший случай сервера: Groq 60с + Gemini 60с последовательно (~120с), запас до 150с для финала
  const unsigned long firstByteMs = expectReport ? 150000 : 15000;
  const unsigned long idleMs = expectReport ? 10000 : 2000;
  unsigned long startMs = millis();
  while (!httpClient.available() && httpClient.connected() &&
         millis() - startMs < firstByteMs) {
    delay(10);
  }

  String body;
  unsigned long lastData = millis();
  uint8_t rbuf[512];
  while (httpClient.connected() || httpClient.available()) {
    if (httpClient.available()) {
      int n = httpClient.read(rbuf, sizeof(rbuf));
      if (n > 0) {
        body.concat((const char*)rbuf, n);
        lastData = millis();
      } else {
        delay(5);
      }
      continue;
    }
    if (millis() - lastData > idleMs) break;
    delay(10);
  }
  httpClient.stop();

  const bool ok = body.indexOf(" 200 ") >= 0;
  Serial.printf("Ответ за %lu мс, тело %u байт -> %s\n",
                millis() - startMs, body.length(), ok ? "OK" : "ОШИБКА");
  if (!ok) {
    Serial.print("[debug] первые байты: ");
    Serial.println(body.length() ? body.substring(0, 80) : String("(пусто)"));
    return false;
  }

  if (expectReport) {
    const String sttPath    = extractJsonStr(body, "stt");
    const String transcript = extractJsonStr(body, "transcript");
    const String answerText = extractJsonStr(body, "text");

    Serial.println("========================================");
    Serial.println("Путь распознавания: " +
                   (sttPath.length() ? sttPath : String("(нет данных)")));
    Serial.println("--- ЧТО РАСПОЗНАНО ИЗ ЗАПИСИ ---");
    Serial.println(transcript.length() ? transcript : "(пусто)");
    if (answerText.length()) {
      Serial.println("--- ОТВЕТ НЕЙРОНКИ (первые 400 симв.) ---");
      Serial.println(answerText.length() > 400
                         ? answerText.substring(0, 400) + "..."
                         : answerText);
    }
    Serial.println("========================================");
  }
  return true;
}

bool sendJob(const UploadJob& job) {
  File f;
  uint32_t fileSize = 0;
  const bool hasFile = job.file[0] != '\0';

  if (hasFile) {
    xSemaphoreTake(spiffsMutex, portMAX_DELAY);
    f = SPIFFS.open(job.file, FILE_READ);
    xSemaphoreGive(spiffsMutex);
    if (!f) {
      Serial.println("ОШИБКА: не удалось открыть файл для отправки");
      return false;
    }
    fileSize = f.size();
  }

  Serial.printf("Отправка чанка %d (%s), %u байт...\n",
                job.index, job.finalChunk ? "FINAL" : "chunk", fileSize);

  if (!httpClient.connect(BACKEND_HOST, BACKEND_PORT, 5000)) {
    Serial.println("ОШИБКА: нет соединения с сервером");
    if (f) f.close();
    return false;
  }
  httpClient.setTimeout(30000);   // слабый Wi-Fi + большой чанк

  httpClient.printf("POST /api/voice-chunk HTTP/1.1\r\n");
  httpClient.printf("Host: %s:%d\r\n", BACKEND_HOST, BACKEND_PORT);
  httpClient.printf("X-Device-Key: %s\r\n", BACKEND_DEVICE_KEY);
  httpClient.printf("X-Session-Id: %s\r\n", job.sid);
  httpClient.printf("X-Chunk-Index: %d\r\n", job.index);
  httpClient.printf("X-Final: %s\r\n", job.finalChunk ? "true" : "false");
  httpClient.printf("Content-Type: audio/wav\r\n");
  httpClient.printf("Content-Length: %u\r\n", fileSize);
  httpClient.printf("Connection: close\r\n\r\n");

  if (hasFile) {
    unsigned long upStart = millis();
    size_t totalSent = 0;
    bool fail = false;
    uint8_t buf[4096];
    while (f.available()) {
      int n = f.read(buf, sizeof(buf));
      if (n <= 0) break;
      const size_t written = httpClient.write(buf, n);
      totalSent += written;
      delay(1);   // слот WiFi-стеку: длинная блокировка роняет lwIP
      if (written != (size_t)n) {
        Serial.printf("ОШИБКА отправки: записано %u из %d (итого %u/%u)\n",
                      (unsigned)written, n,
                      (unsigned)totalSent, (unsigned)fileSize);
        fail = true;
        break;
      }
    }
    httpClient.flush();
    f.close();
    if (fail || totalSent != fileSize) {
      Serial.printf("Загрузка оборвалась: %u/%u байт за %lu мс\n",
                    (unsigned)totalSent, (unsigned)fileSize,
                    millis() - upStart);
      httpClient.stop();
      return false;
    }
  }

  return readResponseAndReport(job.finalChunk);
}

// ======================== LOOP: ОТПРАВЩИК =================================

void loop() {
  // Перенаправляем запрос кнопки задаче записи
  if (isrRequest != BtnCmd::NONE) {
    const BtnCmd req = isrRequest;
    isrRequest = BtnCmd::NONE;
    if (req == BtnCmd::START ||
        (req == BtnCmd::STOP && recordingActive)) {
      xQueueSend(cmdQueue, &req, 0);
    }
  }

  UploadJob job;
  if (xQueueReceive(uploadQueue, &job, pdMS_TO_TICKS(100)) == pdTRUE) {
    bool ok = false;
    for (int attempt = 1; attempt <= UPLOAD_RETRIES; attempt++) {
      ok = sendJob(job);
      if (ok) break;
      if (attempt < UPLOAD_RETRIES) {
        Serial.printf("Повтор отправки чанка %d: попытка %d из %d",
                      job.index, attempt + 1, UPLOAD_RETRIES);
        Serial.printf(" через %lu мс\n", (unsigned long)UPLOAD_RETRY_PAUSE_MS);
        delay(UPLOAD_RETRY_PAUSE_MS);
      }
    }

    if (ok && job.file[0] != '\0') {
      xSemaphoreTake(spiffsMutex, portMAX_DELAY);
      SPIFFS.remove(job.file);
      xSemaphoreGive(spiffsMutex);
    }

    if (!ok) {
      if (job.file[0] != '\0') {
        Serial.printf("Чанк %d НЕ отправлен после %d попыток — файл %s",
                      job.index, UPLOAD_RETRIES, job.file);
        Serial.println(" оставлен в SPIFFS");
      } else {
        Serial.printf("Финальный маркер не отправлен после %d попыток\n",
                      UPLOAD_RETRIES);
      }
    }

    if (job.finalChunk) {
      if (ok) {
        blinkLed(3, 120, 120);
        Serial.println("=== Сессия полностью отправлена ===");
      } else {
        blinkLed(2, 100, 100);
        Serial.println("=== Сессия завершена С ОШИБКОЙ отправки ===");
      }
    }
  }
}

// ================================ SETUP ===================================

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== ESP32-C3 Voice Chunks v2 (DMA) ===");
  Serial.printf("Свободная память: %d байт\n", ESP.getFreeHeap());

  pinMode(LED_PIN, OUTPUT);
  ledOff();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  if (!SPIFFS.begin(true)) {
    Serial.println("Ошибка SPIFFS!");
    blinkLed(5, 200, 200);
    while (true) delay(1000);
  }
  for (const char* p : LEGACY_CHUNK_FILES) {
    if (SPIFFS.exists(p)) SPIFFS.remove(p);   // мусор старых версий
  }
  // Недоотправленные чанки прошлой сессии: обходим каталог один раз,
  // а не перебираем 1440 имён (иначе заметная задержка на старте).
  File root = SPIFFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String n = f.name();
      if (!n.startsWith("/")) n = "/" + n;
      if (n.startsWith("/c") && n.endsWith(".wav")) SPIFFS.remove(n);
      f = root.openNextFile();
    }
    root.close();
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
    Serial.println("\nНет Wi-Fi! Перезагрузка...");
    blinkLed(5, 100, 100);
    ESP.restart();
  }
  Serial.printf("\nIP: %s, куча после Wi-Fi: %d байт\n",
                WiFi.localIP().toString().c_str(), ESP.getFreeHeap());

  adcInit();   // драйвер создаётся один раз; старт/стоп захвата — по сессии

  cmdQueue    = xQueueCreate(4, sizeof(BtnCmd));
  uploadQueue = xQueueCreate(UPLOAD_QUEUE_LEN, sizeof(UploadJob));   // ограниченная очередь
  spiffsMutex = xSemaphoreCreateMutex();

  // Приоритет выше loopTask; WiFi/lwIP всё ещё выше внутри IDF.
  // Чтение DMA-кадра блокирует задачу на семафоре -> CPU отдаётся сети.
  xTaskCreate(samplerTask, "sampler", 8192, nullptr, 3, nullptr);

  Serial.println("Готов к работе. Кнопка = старт/стоп записи.");
}

// ============================== ISR =======================================

void IRAM_ATTR buttonISR() {
  static unsigned long lastPressMs = 0;
  const unsigned long now = (unsigned long)(esp_timer_get_time() / 1000ULL);
  if (now - lastPressMs < 400) return;   // антидребезг
  lastPressMs = now;

  // Только флаг: никаких String/Serial/heap в ISR!
  isrRequest = recordingActive ? BtnCmd::STOP : BtnCmd::START;
}
