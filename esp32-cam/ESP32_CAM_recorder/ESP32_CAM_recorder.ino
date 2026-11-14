/*
 * ESP32-CAM (AI Thinker, сенсор OV3660) — запись на SD-карту + отправка в ИИ
 * --------------------------------------------------------------
 * Что делает:
 *  1) Подключается к ДОМАШНЕМУ Wi-Fi (режим STA, с паролем твоей сети)
 *  2) Живое превью на странице (/stream) — быстрый MJPEG-поток на низком
 *     разрешении (QVGA). ВАЖНО: превью больше НЕ открывается автоматически
 *     при загрузке страницы — только по кнопке "Показать превью". Причина:
 *     сервер платы однопоточный, и пока /stream открыт (а браузер держит
 *     его открытым бесконечно), ВСЕ остальные запросы (отправка в ИИ,
 *     серия, счётчики кадров) зависают и не доходят до платы. Раньше
 *     <img src='/stream'> стоял в странице напрямую — это и было причиной
 *     "ничего не отправляется, счётчик не растёт": страница открыта в
 *     браузере = сервер платы намертво занят стримом.
 *  3) "Видео" (старт/стоп) -> папка /recordings/rec_N, частые кадры
 *     в ПОЛНОМ качестве (SVGA), ~5 кадров/сек
 *  4) "Таймлапс" (старт/стоп) -> папка /timelapse/tl_N, редкие кадры
 *     в полном качестве, при старте включается ночной режим
 *  5) "Отправить кадр в ИИ" — один кадр в лучшем качестве уходит на
 *     бэкенд-сервер (/api/photo), ответ показывается на странице
 *  6) Серийная съёмка: до BURST_MAX_PHOTOS (10) кадров в лучшем качестве
 *     накапливаются в памяти (кнопкой на странице ИЛИ физической кнопкой
 *     на плате), затем отправляются все разом на /api/photos
 *  7) Зеркалирование: независимые переключатели "по горизонтали" и
 *     "по вертикали" (кнопки на странице), применяются сразу и сохраняются
 *     до перезагрузки платы (при перезапуске сбрасываются в "выкл")
 *
 * ВАЖНО про /stream: пока страница с превью открыта, весь остальной сервер
 * (включая фоновую запись видео/таймлапса) блокируется — WebServer в этом
 * проекте однопоточный. Закрой вкладку с превью, если должна идти фоновая
 * запись/таймлапс без задержек.
 *
 * ФИЗИЧЕСКАЯ КНОПКА: подключается между GPIO13 и GND (см. SHUTTER_BUTTON_PIN).
 * GPIO13 выбран как свободный пин на этой плате — не занят камерой, SD-картой
 * (в 1-битном режиме) и вспышкой, в отличие от GPIO0/2/4/12.
 *
 * ПЕРЕД ЗАЛИВКОЙ ОБЯЗАТЕЛЬНО ЗАПОЛНИ:
 *   WIFI_SSID / WIFI_PASSWORD  — твоя домашняя сеть
 *   BACKEND_HOST / BACKEND_PORT / BACKEND_DEVICE_KEY — данные твоего бэкенда
 *
 * Плата в Arduino IDE: "AI Thinker ESP32-CAM"
 * Partition Scheme: "Huge APP (3MB No OTA/1MB SPIFFS)"
 * PSRAM: Enabled (обязательно! иначе серийная съёмка в память может не влезть)
 *
 * Библиотеки нужны только штатные, которые идут вместе с пакетом esp32:
 *   esp_camera.h, WiFi.h, WebServer.h, HTTPClient.h, base64.h, SD_MMC.h, FS.h
 * Ничего дополнительно через Library Manager ставить не нужно.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include "base64.h"
#include "FS.h"
#include "SD_MMC.h"
#include "time.h"
#include "config.h"

// ============================ CONFIG ================================

// Credentials are now in config.h — see config.example.h for template

// Разрешение/качество для КАЧЕСТВЕННЫХ снимков (запись, таймлапс, отправка в ИИ)
// FRAMESIZE_ варианты: FRAMESIZE_QVGA(320x240), FRAMESIZE_VGA(640x480),
// FRAMESIZE_SVGA(800x600), FRAMESIZE_XGA, FRAMESIZE_UXGA(1600x1200) и т.д.
#define CAPTURE_FRAMESIZE   FRAMESIZE_SVGA
#define CAPTURE_JPEG_QUALITY 10   // 0(лучшее)..63(худшее), 10-12 — хороший баланс

// Разрешение/качество ТОЛЬКО для быстрого живого превью в браузере (/stream)
// Специально маленькое и похуже качеством — чтобы обновлялось быстро
#define PREVIEW_FRAMESIZE    FRAMESIZE_QVGA
#define PREVIEW_JPEG_QUALITY 25

// Сколько кадров максимум можно накопить в "серию" перед отправкой всех разом в ИИ
#define BURST_MAX_PHOTOS 10

// Физическая кнопка спуска затвора.
// GPIO13 выбран как безопасный: не занят камерой, SD-картой (в 1-битном режиме)
// или вспышкой (в отличие от GPIO0/2/4/12 на этой плате).
// Подключение: между GPIO13 и GND, используется внутренняя подтяжка (INPUT_PULLUP).
#define SHUTTER_BUTTON_PIN 13
#define BUTTON_DEBOUNCE_MS 300
#define BUTTON_LONG_PRESS_MS 1500
#define FLASH_LED_PIN 4
// v2: тихий режим — вспышка на захвате только если true
#define FLASH_ON_CAPTURE false

// Целевой интервал между кадрами при записи "видео" (кадр-последовательности)
// SD-карта на 1-битном режиме ESP32-CAM не очень быстрая, поэтому не гонимся
// за высоким fps. 200мс = примерно 5 кадров/сек.
#define RECORD_FRAME_INTERVAL_MS 200

// Интервал между кадрами таймлапса. 60000 = раз в 60 секунд.
// За ночь (8 часов) это даст ~480 кадров — удобно и для карты, и для сборки видео.
#define TIMELAPSE_INTERVAL_MS 60000

// Настройки сенсора, которые автоматически применяются при СТАРТЕ таймлапса
// (актуально для съёмки в темноте/через окно ночью)
#define TIMELAPSE_NIGHT_MODE_ENABLED true
// Примечание: усиление (Gainceiling) при старте таймлапса выставляется в
// максимум (GAINCEILING_128X) прямо в функции applyTimelapseNightSettings() ниже

// =====================================================================

// ---- Пины камеры для AI Thinker ESP32-CAM ----
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WebServer server(80);

bool recording = false;
unsigned long lastRecordFrameMs = 0;
int recordSessionNumber = 0;
String recordDir = "";
unsigned long recordFrameCount = 0;

bool timelapseActive = false;
unsigned long lastTimelapseMs = 0;
int timelapseSessionNumber = 0;
String timelapseDir = "";
unsigned long timelapseFrameCount = 0;

// Буфер серийной съёмки (до BURST_MAX_PHOTOS кадров в PSRAM/куче)
uint8_t* burstBuf[BURST_MAX_PHOTOS];
size_t   burstLen[BURST_MAX_PHOTOS];
int burstCount = 0;

unsigned long lastButtonPressMs = 0;
static bool buttonWasPressed = false;
static unsigned long pressTime = 0;
static bool longPressHandled = false;

// Зеркалирование изображения — независимые переключатели.
// hmirror = отражение по горизонтали (влево-вправо), vflip = по вертикали (вверх-вниз).
bool mirrorH = false;
bool mirrorV = true;   // камера висит вверх ногами -> вертикальное отражение включено с завода

// ---------------------------------------------------------------------
// Инициализация камеры
// ---------------------------------------------------------------------
bool initCamera() {
  camera_config_t config = {};  // обнуляем структуру целиком - без этого
                                 // fb_location/grab_mode могут содержать
                                 // "мусор" и приводить к malloc failed
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = CAPTURE_FRAMESIZE;
    config.jpeg_quality = CAPTURE_JPEG_QUALITY;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    Serial.println("PSRAM не найдена! Работаем на пониженных настройках "
                    "(VGA, 1 буфер, память в DRAM). Проверь Tools -> PSRAM: Enabled в Arduino IDE.");
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Ошибка инициализации камеры: 0x%x\n", err);
    return false;
  }
  applyMirror();
  return true;
}

// ---------------------------------------------------------------------
// Применить текущие настройки зеркалирования (mirrorH/mirrorV) к сенсору.
// Вызывается при старте, при каждом переключении, и на всякий случай
// внутри ensureCaptureQuality()/setPreviewQuality(), т.к. некоторые
// прошивки сенсора могут сбрасывать эти флаги при смене framesize.
// ---------------------------------------------------------------------
void applyMirror() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_hmirror(s, mirrorH ? 1 : 0);
  s->set_vflip(s, mirrorV ? 1 : 0);
}

// ---------------------------------------------------------------------
// Применить/снять настройки сенсора для ночной съёмки
// ---------------------------------------------------------------------
void applyTimelapseNightSettings(bool enable) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  if (enable) {
    // Примечание: enum gainceiling_t принимает не произвольное число, а ступени
    // (2X,4X,8X,16X,32X,64X,128X). Берём максимум для ночи.
    s->set_gainceiling(s, GAINCEILING_128X);
    s->set_special_effect(s, 0);
    if (TIMELAPSE_NIGHT_MODE_ENABLED) {
      s->set_ae_level(s, 2); // сдвиг автоэкспозиции в сторону "светлее"
    }
    Serial.println("Ночные настройки для таймлапса применены (Gainceiling max)");
  } else {
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_ae_level(s, 0);
    Serial.println("Ночные настройки для таймлапса сняты");
  }
}

// ---------------------------------------------------------------------
// Гарантировать, что сенсор в режиме ВЫСОКОГО качества (для записи/таймлапса/
// отправки в ИИ). Вызывается перед каждым "серьёзным" кадром на случай,
// если до этого кто-то смотрел /stream и качество было понижено.
// ---------------------------------------------------------------------
void ensureCaptureQuality() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_framesize(s, CAPTURE_FRAMESIZE);
  s->set_quality(s, CAPTURE_JPEG_QUALITY);
  applyMirror();
}

// Понизить качество для быстрого живого превью
void setPreviewQuality() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_framesize(s, PREVIEW_FRAMESIZE);
  s->set_quality(s, PREVIEW_JPEG_QUALITY);
  applyMirror();
}

// ---------------------------------------------------------------------
// Снять кадр и отправить его на бэкенд-сервер (/api/photo)
// Возвращает текстовый ответ нейросети (или сообщение об ошибке)
// ---------------------------------------------------------------------
String sendPhotoToBackend() {
  ensureCaptureQuality();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    return "Ошибка: не удалось получить кадр с камеры";
  }

  HTTPClient http;
  String url = "http://" + String(BACKEND_HOST) + ":" + String(BACKEND_PORT) + "/api/photo";

  http.begin(url);
  http.addHeader("X-Device-Key", BACKEND_DEVICE_KEY);
  http.addHeader("Content-Type", "application/octet-stream");
  http.setTimeout(30000); // Gemini может думать несколько секунд, даём запас

  int httpCode = http.POST(fb->buf, fb->len);
  String result;

  if (httpCode == 200) {
    result = http.getString();
    Serial.println("Ответ бэкенда: " + result);
  } else {
    result = "Ошибка HTTP: " + String(httpCode) + " " + http.getString();
    Serial.println(result);
  }

  http.end();
  esp_camera_fb_return(fb);
  return result;
}

// ---------------------------------------------------------------------
// v2: одиночный кадр сразу в staging — POST /api/photo-frame (raw JPEG).
// Сервер только складывает в staging, в нейронку НЕ отправляет.
// Возвращает HTTP status code; тело ответа кладётся в out-param body.
// 200 = кадр принят, 413 = staging full / bytes full / frame too large
// (сервер отклонил — нужен коммит), прочее = ошибка отправки.
// ---------------------------------------------------------------------
int sendFrameToBackend(String &body) {
  ensureCaptureQuality();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("sendFrame: не удалось получить кадр с камеры");
    body = "";
    return -1;
  }

  HTTPClient http;
  String url = "http://" + String(BACKEND_HOST) + ":" + String(BACKEND_PORT) + "/api/photo-frame";

  http.begin(url);
  http.addHeader("X-Device-Key", BACKEND_DEVICE_KEY);
  http.addHeader("Content-Type", "application/octet-stream");
  http.setTimeout(30000);

  int httpCode = http.POST(fb->buf, fb->len);
  String payload = http.getString();
  Serial.println("sendFrame HTTP " + String(httpCode) + " " + payload);

  http.end();
  esp_camera_fb_return(fb);
  body = payload;
  return httpCode;
}

// ---------------------------------------------------------------------
// v2: коммит staging — POST /api/photos/commit с пустым телом.
// Сервер забирает все staged-кадры и отправляет в Gemini одним запросом.
// Возвращает HTTP status code; тело ответа кладётся в out-param body.
// 200 = успех (тело может содержать слово "error" в прозе ответа —
// это НЕ признак неудачи, решаем только по коду), 400 = пустой staging,
// прочие (502/транспортные <0) = ошибка сервера/сети.
// ---------------------------------------------------------------------
int sendCommitToBackend(String &body) {
  HTTPClient http;
  String url = "http://" + String(BACKEND_HOST) + ":" + String(BACKEND_PORT) + "/api/photos/commit";

  http.begin(url);
  http.addHeader("X-Device-Key", BACKEND_DEVICE_KEY);
  // Худший случай сервера: Groq 60с + Gemini 60с последовательно (~120с), запас до 150с
  http.setTimeout(150000);

  int httpCode = http.POST(String(""));
  String result;

  if (httpCode == 200) {
    result = http.getString();
    Serial.println("Ответ бэкенда (commit): " + result);
  } else {
    result = "{\"error\":\"HTTP " + String(httpCode) + " " + http.getString() + "\"}";
    Serial.println(result);
  }

  http.end();
  body = result;
  return httpCode;
}

// ---------------------------------------------------------------------
// Серийная съёмка: захват кадра в память (не на SD), очистка серии,
// отправка всей накопленной серии на бэкенд одним запросом
// v2: этот burst-флоу оставлен только для веб-страницы (/burst_*);
// физическая кнопка теперь использует sendFrameToBackend/sendCommitToBackend.
// ---------------------------------------------------------------------
void freeBurstBuffers() {
  for (int i = 0; i < burstCount; i++) {
    if (burstBuf[i]) {
      free(burstBuf[i]);
      burstBuf[i] = nullptr;
    }
  }
  burstCount = 0;
}

// Возвращает true, если кадр успешно добавлен в серию
bool captureIntoBurst() {
  if (burstCount >= BURST_MAX_PHOTOS) {
    Serial.println("Серия уже заполнена, сначала отправь или очисти");
    return false;
  }

  ensureCaptureQuality();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Не удалось получить кадр для серии");
    return false;
  }

  // Копируем данные кадра в отдельный буфер, т.к. esp_camera_fb_return()
  // освобождает/переиспользует внутренний буфер камеры.
  uint8_t* copy = (uint8_t*) (psramFound() ? ps_malloc(fb->len) : malloc(fb->len));
  if (!copy) {
    Serial.println("Не хватило памяти для кадра серии");
    esp_camera_fb_return(fb);
    return false;
  }
  memcpy(copy, fb->buf, fb->len);
  burstBuf[burstCount] = copy;
  burstLen[burstCount] = fb->len;
  burstCount++;
  esp_camera_fb_return(fb);

  Serial.println("Кадр " + String(burstCount) + "/" + String(BURST_MAX_PHOTOS) + " добавлен в серию");
  return true;
}

// Отправляет накопленную серию на /api/photos (JSON с массивом base64-картинок)
String sendBurstToBackend() {
  if (burstCount == 0) {
    return "{\"error\":\"Серия пуста\"}";
  }

  String body = "{\"images\":[";
  for (int i = 0; i < burstCount; i++) {
    if (i > 0) body += ",";
    body += "\"" + base64::encode(burstBuf[i], burstLen[i]) + "\"";
  }
  body += "]}";

  HTTPClient http;
  String url = "http://" + String(BACKEND_HOST) + ":" + String(BACKEND_PORT) + "/api/photos";

  http.begin(url);
  http.addHeader("X-Device-Key", BACKEND_DEVICE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(45000); // несколько картинок сразу - Gemini думает дольше

  int httpCode = http.POST(body);
  String result;

  if (httpCode == 200) {
    result = http.getString();
    Serial.println("Ответ бэкенда (серия): " + result);
  } else {
    result = "{\"error\":\"HTTP " + String(httpCode) + "\"}";
    Serial.println(result);
  }

  http.end();
  freeBurstBuffers();
  return result;
}

// ---------------------------------------------------------------------
// Сохранить один кадр в указанный файл на SD
// ---------------------------------------------------------------------
bool saveFrameToFile(const String &path) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Не удалось получить кадр с камеры");
    return false;
  }

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("Не удалось открыть файл для записи: %s\n", path.c_str());
    esp_camera_fb_return(fb);
    return false;
  }
  file.write(fb->buf, fb->len);
  file.close();
  esp_camera_fb_return(fb);
  return true;
}

// ---------------------------------------------------------------------
// Веб-страница
// ---------------------------------------------------------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ESP32-CAM</title>";
  html += "<style>body{font-family:sans-serif;background:#1a1a1a;color:#eee;text-align:center;padding:20px;}";
  html += "button{font-size:18px;padding:12px 20px;margin:8px;border-radius:8px;border:none;}";
  html += ".on{background:#c0392b;color:white;} .off{background:#444;color:white;}";
  html += "img{max-width:95%;border-radius:8px;margin-top:15px;}</style></head><body>";
  html += "<h2>ESP32-CAM</h2>";

  html += "<p>Видео: <b>" + String(recording ? "ИДЁТ (" + recordDir + ", " + String(recordFrameCount) + " кадров)" : "остановлено") + "</b></p>";
  if (recording) {
    html += "<button class='on' onclick=\"fetch('/stop_record').then(()=>location.reload())\">Остановить видео</button>";
  } else {
    html += "<button class='off' onclick=\"fetch('/start_record').then(()=>location.reload())\">Начать видео</button>";
  }
  html += "<br>";

  html += "<p>Таймлапс: <b>" + String(timelapseActive ? "ИДЁТ (" + timelapseDir + ", " + String(timelapseFrameCount) + " кадров)" : "остановлен") + "</b></p>";
  if (timelapseActive) {
    html += "<button class='on' onclick=\"fetch('/stop_timelapse').then(()=>location.reload())\">Остановить таймлапс</button>";
  } else {
    html += "<button class='off' onclick=\"fetch('/start_timelapse').then(()=>location.reload())\">Начать таймлапс (ночной режим)</button>";
  }

  html += "<hr style='margin:20px 0;border-color:#444'>";
  html += "<p>Зеркало по горизонтали: <b>" + String(mirrorH ? "ВКЛ" : "выкл") + "</b> &nbsp; ";
  html += "Зеркало по вертикали: <b>" + String(mirrorV ? "ВКЛ" : "выкл") + "</b></p>";
  html += "<button class='" + String(mirrorH ? "on" : "off") + "' onclick=\"fetch('/toggle_mirror_h').then(()=>location.reload())\">Зеркало по горизонтали</button>";
  html += "<button class='" + String(mirrorV ? "on" : "off") + "' onclick=\"fetch('/toggle_mirror_v').then(()=>location.reload())\">Зеркало по вертикали</button>";

  html += "<hr style='margin:20px 0;border-color:#444'>";
  html += "<p style='font-size:14px;color:#aaa'>Превью открывается только по кнопке — пока оно открыто, "
          "остальные действия на плате (отправка в ИИ, серия, запись) не работают, "
          "т.к. сервер платы однопоточный. Закрой превью перед тем, как жать другие кнопки.</p>";
  html += "<button class='off' id='previewBtn' onclick='togglePreview()'>Показать превью</button>";
  html += "<div id='previewWrap'></div>";

  html += "<hr style='margin:20px 0;border-color:#444'>";
  html += "<button class='off' id='aiBtn' onclick='sendToAI()'>Отправить кадр в ИИ (лучшее качество)</button>";
  html += "<p id='aiAnswer' style='text-align:left;white-space:pre-wrap;background:#222;padding:10px;border-radius:8px;min-height:20px;'></p>";

  html += "<hr style='margin:20px 0;border-color:#444'>";
  html += "<p>Серия: <b>" + String(burstCount) + "/" + String(BURST_MAX_PHOTOS) + " кадров</b> (можно снимать физической кнопкой на плате)</p>";
  html += "<button class='off' onclick=\"fetch('/burst_capture').then(()=>location.reload())\">Снять кадр в серию</button>";
  html += "<button class='off' onclick=\"fetch('/burst_clear').then(()=>location.reload())\">Очистить серию</button><br>";
  html += "<button class='off' id='burstBtn' onclick='sendBurst()'>Отправить серию в ИИ</button>";
  html += "<p id='burstAnswer' style='text-align:left;white-space:pre-wrap;background:#222;padding:10px;border-radius:8px;min-height:20px;'></p>";

  html += "<script>";
  html += "let previewOn = false;";
  html += "function togglePreview(){";
  html += "  const wrap = document.getElementById('previewWrap');";
  html += "  const btn = document.getElementById('previewBtn');";
  html += "  if (!previewOn) {";
  html += "    wrap.innerHTML = \"<img src='/stream' id='preview'>\";";
  html += "    btn.innerText = 'Скрыть превью (чтобы разблокировать остальные кнопки)';";
  html += "    previewOn = true;";
  html += "  } else {";
  html += "    wrap.innerHTML = '';"; // убираем img -> браузер закрывает соединение -> сервер платы разблокируется
  html += "    btn.innerText = 'Показать превью';";
  html += "    previewOn = false;";
  html += "  }";
  html += "}";
  html += "function sendToAI(){";
  html += "  document.getElementById('aiBtn').disabled = true;";
  html += "  document.getElementById('aiAnswer').innerText = 'Отправляю и жду ответ...';";
  html += "  fetch('/send_to_ai').then(r=>r.json()).then(data=>{";
  html += "    document.getElementById('aiAnswer').innerText = data.text || data.error || 'Пустой ответ';";
  html += "    document.getElementById('aiBtn').disabled = false;";
  html += "  }).catch(e=>{";
  html += "    document.getElementById('aiAnswer').innerText = 'Ошибка запроса: ' + e;";
  html += "    document.getElementById('aiBtn').disabled = false;";
  html += "  });";
  html += "}";
  html += "function sendBurst(){";
  html += "  document.getElementById('burstBtn').disabled = true;";
  html += "  document.getElementById('burstAnswer').innerText = 'Отправляю серию и жду ответ...';";
  html += "  fetch('/burst_send').then(r=>r.json()).then(data=>{";
  html += "    document.getElementById('burstAnswer').innerText = data.text || data.error || 'Пустой ответ';";
  html += "    document.getElementById('burstBtn').disabled = false;";
  html += "    setTimeout(()=>location.reload(), 800);";
  html += "  }).catch(e=>{";
  html += "    document.getElementById('burstAnswer').innerText = 'Ошибка запроса: ' + e;";
  html += "    document.getElementById('burstBtn').disabled = false;";
  html += "  });";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleCapture() {
  ensureCaptureQuality();
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }
  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// Живой MJPEG-поток для быстрого превью на странице (низкое качество/разрешение).
// ВАЖНО: пока эта функция выполняется (клиент держит соединение открытым),
// весь остальной HTTP-сервер и фоновая логика (запись видео/таймлапса) не
// продвигаются — WebServer в этом проекте однопоточный. Это нормально для
// просмотра "туда-сюда", но не открывай /stream надолго, если параллельно
// должна идти запись/таймлапс.
void handleStream() {
  WiFiClient client = server.client();

  String header = "HTTP/1.1 200 OK\r\n";
  header += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  client.print(header);

  setPreviewQuality();

  while (client.connected()) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      break;
    }
    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: " + String(fb->len) + "\r\n\r\n");
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    delay(20); // небольшая пауза, чтобы не перегружать Wi-Fi
  }

  ensureCaptureQuality();
  Serial.println("Клиент отключился от /stream, качество восстановлено");
}

void handleSendToAI() {
  String answer = sendPhotoToBackend();
  server.send(200, "application/json; charset=utf-8", answer);
}

void handleBurstCapture() {
  bool ok = captureIntoBurst();
  if (ok) {
    server.send(200, "text/plain; charset=utf-8", "Кадр " + String(burstCount) + "/" + String(BURST_MAX_PHOTOS) + " сохранён в серию");
  } else {
    server.send(400, "text/plain; charset=utf-8", "Не удалось добавить кадр (серия заполнена или ошибка камеры/памяти)");
  }
}

void handleBurstClear() {
  freeBurstBuffers();
  server.send(200, "text/plain; charset=utf-8", "Серия очищена");
}

void handleBurstSend() {
  String result = sendBurstToBackend();
  server.send(200, "application/json; charset=utf-8", result);
}

void handleToggleMirrorH() {
  mirrorH = !mirrorH;
  applyMirror();
  Serial.println("Зеркало по горизонтали: " + String(mirrorH ? "включено" : "выключено"));
  server.send(200, "text/plain", mirrorH ? "on" : "off");
}

void handleToggleMirrorV() {
  mirrorV = !mirrorV;
  applyMirror();
  Serial.println("Зеркало по вертикали: " + String(mirrorV ? "включено" : "выключено"));
  server.send(200, "text/plain", mirrorV ? "on" : "off");
}

void handleStartTimelapse() {
  if (!timelapseActive) {
    timelapseSessionNumber++;
    timelapseDir = "/timelapse/tl_" + String(timelapseSessionNumber);
    SD_MMC.mkdir("/timelapse");
    SD_MMC.mkdir(timelapseDir);
    timelapseFrameCount = 0;
    lastTimelapseMs = 0;
    timelapseActive = true;
    applyTimelapseNightSettings(true);
    Serial.println("Таймлапс начат: " + timelapseDir);
  }
  server.send(200, "text/plain", "started");
}

void handleStopTimelapse() {
  timelapseActive = false;
  applyTimelapseNightSettings(false);
  Serial.println("Таймлапс остановлен: " + timelapseDir + " (" + String(timelapseFrameCount) + " кадров)");
  server.send(200, "text/plain", "stopped");
}

void handleStartRecord() {
  if (!recording) {
    recordSessionNumber++;
    recordDir = "/recordings/rec_" + String(recordSessionNumber);
    SD_MMC.mkdir("/recordings");
    SD_MMC.mkdir(recordDir);
    recordFrameCount = 0;
    lastRecordFrameMs = 0;
    recording = true;
    Serial.println("Запись начата: " + recordDir);
  }
  server.send(200, "text/plain", "started");
}

void handleStopRecord() {
  recording = false;
  Serial.println("Запись остановлена: " + recordDir + " (" + String(recordFrameCount) + " кадров)");
  server.send(200, "text/plain", "stopped");
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  pinMode(SHUTTER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!initCamera()) {
    Serial.println("Камера не инициализирована, остановка");
    return;
  }

  // SD-карта в ESP32-CAM работает в 1-битном режиме (второй параметр true)
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Не удалось смонтировать SD-карту! Проверь, что она вставлена и отформатирована в FAT32.");
  } else {
    Serial.println("SD-карта смонтирована успешно");
    SD_MMC.mkdir("/recordings");
    SD_MMC.mkdir("/timelapse");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Подключение к Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Подключено! IP адрес камеры: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.println("Не удалось подключиться к Wi-Fi. Проверь SSID/пароль.");
  }

  server.on("/", handleRoot);
  server.on("/capture", handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/start_record", handleStartRecord);
  server.on("/stop_record", handleStopRecord);
  server.on("/start_timelapse", handleStartTimelapse);
  server.on("/stop_timelapse", handleStopTimelapse);
  server.on("/send_to_ai", handleSendToAI);
  server.on("/burst_capture", handleBurstCapture);
  server.on("/burst_clear", handleBurstClear);
  server.on("/burst_send", handleBurstSend);
  server.on("/toggle_mirror_h", handleToggleMirrorH);
  server.on("/toggle_mirror_v", handleToggleMirrorV);
  server.begin();
  Serial.println("Веб-сервер запущен");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // v2: короткое нажатие = один кадр сразу в staging, длинное удержание = коммит.
  // (Старый burst-флоу через captureIntoBurst/sendBurstToBackend оставлен только
  // для веб-страницы; физическая кнопка его больше не использует.)
  bool buttonIsPressed = (digitalRead(SHUTTER_BUTTON_PIN) == LOW);

  // Falling edge — кнопка нажата
  if (buttonIsPressed && !buttonWasPressed) {
    buttonWasPressed = true;
    pressTime = now;
    longPressHandled = false;
  }

  // Rising edge — кнопка отпущена
  if (!buttonIsPressed && buttonWasPressed) {
    buttonWasPressed = false;
    unsigned long pressDuration = now - pressTime;
    if (pressDuration >= BUTTON_DEBOUNCE_MS && pressDuration < BUTTON_LONG_PRESS_MS) {
      // v2: короткое нажатие — снять ОДИН кадр и сразу POST /api/photo-frame
      String frameBody;
      int frameCode = sendFrameToBackend(frameBody);
      if (frameCode == 200) {
        Serial.println("Кнопка: кадр отправлен в staging");
        // 1x = кадр принят: яркая вспышка только если разрешена, иначе тихий блип
        if (FLASH_ON_CAPTURE) {
          digitalWrite(FLASH_LED_PIN, HIGH); delay(100); digitalWrite(FLASH_LED_PIN, LOW);
        } else {
          digitalWrite(FLASH_LED_PIN, HIGH); delay(30); digitalWrite(FLASH_LED_PIN, LOW);
        }
      } else if (frameCode == 413) {
        Serial.println("staging full — сделай коммит");
        // 5x = staging full / bytes full / frame too large — нужен коммит
        for (int i=0; i<5; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(200); digitalWrite(FLASH_LED_PIN, LOW); delay(200); }
      } else {
        Serial.println("Кнопка: ошибка отправки кадра");
        // 2x = ошибка отправки
        for (int i=0; i<2; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(200); digitalWrite(FLASH_LED_PIN, LOW); delay(200); }
      }
    }
  }

  // Длинное удержание — v2: ВСЕГДА коммит (решение о пустом staging принимает сервер).
  // Успех решаем СТРОГО по HTTP-коду (ответ 200 может содержать слово "error"
  // в прозе Gemini — это не неудача): 200 = 3x блика, 400 = пустой staging =
  // 1x короткий блик, 429 = сервер занят (конкурентный коммит) = 4x быстрых,
  // прочее = 2x блика (ошибка сервера/сети).
  if (buttonIsPressed && buttonWasPressed && !longPressHandled && (now - pressTime >= BUTTON_LONG_PRESS_MS)) {
    longPressHandled = true;
    String commitBody;
    int commitCode = sendCommitToBackend(commitBody);
    if (commitCode == 200) {
      // Успешный коммит: мигнуть 3×
      for (int i=0; i<3; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(100); digitalWrite(FLASH_LED_PIN, LOW); delay(100); }
    } else if (commitCode == 400) {
      // Пустой staging (сервер: no staged frames): один короткий блик
      Serial.println("Кнопка: коммит отклонён — staging пуст (HTTP 400)");
      digitalWrite(FLASH_LED_PIN, HIGH); delay(30); digitalWrite(FLASH_LED_PIN, LOW);
    } else if (commitCode == 429) {
      // Сервер занят (конкурентный коммит): 4x быстрых — подожди и повтори позже
      Serial.println("Кнопка: сервер занят (HTTP 429) — подожди и повтори позже");
      for (int i=0; i<4; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(80); digitalWrite(FLASH_LED_PIN, LOW); delay(80); }
    } else {
      // Ошибка Gemini/сети: мигнуть 2×
      Serial.println("Кнопка: ошибка коммита, HTTP " + String(commitCode));
      for (int i=0; i<2; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(200); digitalWrite(FLASH_LED_PIN, LOW); delay(200); }
    }
  }

  // Запись видео (частая последовательность кадров)
  if (recording && (now - lastRecordFrameMs >= RECORD_FRAME_INTERVAL_MS)) {
    lastRecordFrameMs = now;
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/frame_%06lu.jpg", recordDir.c_str(), recordFrameCount);
    if (saveFrameToFile(String(fname))) {
      recordFrameCount++;
    }
  }

  // Таймлапс (редкие кадры, независимо от записи видео)
  if (timelapseActive && (now - lastTimelapseMs >= TIMELAPSE_INTERVAL_MS)) {
    lastTimelapseMs = now;
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/frame_%06lu.jpg", timelapseDir.c_str(), timelapseFrameCount);
    if (saveFrameToFile(String(fname))) {
      timelapseFrameCount++;
    }
  }
}

