# workflow-integration - Work Plan

## TL;DR (For humans)

**What you'll get:** Единый репозиторий, в котором связаны три устройства: ESP32-CAM (фоткает задание на уроке), бэкенд-сервер на VPS (отправляет фото в Gemini AI и получает ответ) и microReader — твоя карманная читалка на OLED (забирает ответ и показывает как текстовый файл). На ESP32-CAM доработана кнопка: коротко = фото в серию (до 10), длинно = отправка всех фото на сервер. На microReader добавлен режим «Получить ответ» (удержание DOWN): подключается к Wi-Fi, забирает ответ с сервера, сохраняет как .txt файл. Все пароли/IP/ключи спрятаны в gitignored-файлы.

**Why this approach:** Монорепозиторий — все три компонента используют общий API-контракт, изменения согласованы. Credentials никогда не попадают в git (config.h + .env в .gitignore). microReader использует существующий .txt ридер для показа ответов — не нужно писать новый вьюер.

**What it will NOT do:** Не добавляет HTTPS (устройства на локальной сети, HTTP достаточно). Не меняет существующие функции microReader (игры, калькулятор). Не добавляет новую запись видео/таймлапс на ESP32-CAM.

**Effort:** Medium (3 компонента, 2 прошивки, документация)
**Risk:** Low — бэкенд не меняется, прошивки расширяются без рефакторинга существующей логики
**Decisions to sanity-check:** BURST_MAX_PHOTOS=10 влезает ли в PSRAM; схема long-press кнопки не сломает ли существующий short-press

Your next move: утвердить план, затем запустить `/start-work` для исполнения.

---

> TL;DR (machine): Medium effort, Low risk. 3 components in monorepo. 8 todos. Backend untouched. ESP32-CAM: config extraction + burst 4→10 + long-press button. microReader: new network.ino + portal config + down.hold handler. README.

## Scope
### Must have
1. Реструктурировать репозиторий: `esp32-cam/` и `microreader/` папки
2. ESP32-CAM: вынести credentials в `config.h` (в .gitignore) + создать `config.example.h`
3. ESP32-CAM: BURST_MAX_PHOTOS 4→10
4. ESP32-CAM: кнопка long-press (коротко=фото, длинно=отправка) + LED flash фидбек
5. microReader: `network.ino` — HTTP GET /api/latest + сохранение ответа в .txt
6. microReader: `portal.ino` — блок "Сервер" (backendHost, backendPort, deviceKey)
7. microReader: `a-microReader-v1.2.ino` — down.hold() → enterToGetAnswerMode()
8. .gitignore — exclude config.h, *.bin, __pycache__, *.env
9. README.md — полная документация системы (архитектура, настройка, сценарий, security notes)

### Must NOT have (guardrails, anti-slop, scope boundaries)
- ❌ НЕ менять API-контракт бэкенда (структуру эндпоинтов, формат ответа, заголовки)
- ✅ **Исключение:** в app.py добавить `app.json.ensure_ascii = False` — это не меняет API, только отключает ASCII-экранирование, иначе русский текст в JSON придёт как `\uXXXX` и microReader его не распарсит
- ❌ НЕ писать новый файловый вьюер для microReader
- ❌ НЕ трогать игры/калькулятор microReader
- ❌ НЕ добавлять HTTPS/TLS
- ❌ НЕ добавлять запись видео/таймлапс
- ❌ НЕ менять существующие WiFi/AP режимы microReader
- ❌ Никакие реальные IP/ключи/SSID не должны появиться в коммитах

## Verification strategy
> Zero human intervention - all verification is agent-executed.
- **Test decision:** tests-after — для ESP32-CAM: arduino-cli compile check. Для microReader: arduino-cli compile check. Для бэкенда: curl-тесты эндпоинтов.
- **Evidence:** .omo/evidence/workflow-integration/\<task\>/
- **Physical test:** после компиляции пользователь прошивает платы и проверяет сценарий вручную

## Execution strategy
### Parallel execution waves
- **Wave 1** (все параллельно): Task 1 (restructure), Task 8 (.gitignore)
- **Wave 2** (все параллельно): Task 2 (config.h), Task 3 (burst 10), Task 4+5+6 (microReader: struct + портал + хук)
- **Wave 3**: Task 7 (network.ino — зависит от struct, portal, hook)
- **Wave 4**: Task 9 (README — после всех изменений)

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1. Restructure repo | — | 2,3,4,5,6,7 | 8 |
| 2. ESP32-CAM config.h | 1 | — | 3 |
| 3. ESP32-CAM burst+button | 1 | — | 2 |
| 4. microReader struct ext | 1 | 5,6,7 | 2,3 |
| 5. microReader portal.ino | 1,4 | 7 | 6 |
| 6. microReader main.ino-hook | 1,4 | 7 | 5 |
| 7. microReader network.ino | 1,4,5,6 | — | — |
| 8. .gitignore | — | — | 1 |
| 9. README.md | 1,2,3,4,8 | — | — |

## Todos
> Implementation + Test = ONE todo. Never separate.
<!-- APPEND TASK BATCHES BELOW THIS LINE WITH edit/apply_patch - never rewrite the headers above. -->

- [x] 1. Реструктурировать репозиторий — создать папки esp32-cam/ и microreader/ + скопировать файлы прошивок
  What to do / Must NOT do:
    - Создать `G:\Projects\Endpoint-vps-esp32\esp32-cam\ESP32_CAM_recorder\` и скопировать туда `C:\Users\matve\Documents\Arduino\ESP32_CAM_recorder\ESP32_CAM_recorder.ino`
    - Создать `G:\Projects\Endpoint-vps-esp32\microreader\firmware\`
    - Скопировать все .ino файлы microReader из https://github.com/Nich1con/microReader/tree/main/firmware/a-microReader-v1.2 в локальную папку:
      - a-microReader-v1.2.ino
      - calcul.ino, dinosaurGame.ino, files.ino, gamemode.ino
      - portal.ino, servmode.ino, snakeGame.ino, tetrisGame.ino
      - ui.ino, wifi.ino
    - НЕ менять содержимое файлов на этом этапе — только копирование
    - НЕ удалять исходный файл ESP32-CAM из Arduino-папки
  Parallelization: Wave 1 | Blocked by: — | Blocks: 2,3,4,5,6,7
  References: текущая структура репозитория (корень: app.py, README.md и т.д.), папка ESP32_CAM_recorder в Documents\Arduino, GitHub microReader firmware/
  Acceptance criteria: обе папки созданы, файлы на месте, компиляция ESP32-CAM не сломана (arduino-cli или визуальная проверка)
  QA scenarios:
    - Happy: `dir esp32-cam\ESP32_CAM_recorder\` показывает .ino файл; `dir microreader\firmware\` показывает все .ino файлы
    - Failure: проверить что app.py не перемещён, а скопирован
  Evidence: .omo/evidence/1-restructure/
  Commit: Y | feat(repo): reorganize into monorepo with esp32-cam/ and microreader/

- [x] 2. ESP32-CAM: вынести credentials в config.h + config.example.h
  What to do / Must NOT do:
    - Создать `esp32-cam/ESP32_CAM_recorder/config.h` с реальными значениями пользователя (WIFI_SSID, WIFI_PASSWORD, BACKEND_HOST, BACKEND_PORT, BACKEND_DEVICE_KEY)
    - Создать `esp32-cam/ESP32_CAM_recorder/config.example.h` с ПЛЕЙСХОЛДЕРАМИ (пустые строки или "your_ssid_here")
    - **Почему в той же папке, что и .ino:** Arduino IDE при компиляции копирует скетч в temp-директорию, и `#include "../config.h"` (относительный путь выше) может не скомпилироваться. Файл в той же папке — гарантированно работает.
    - В `ESP32_CAM_recorder.ino`:
      - Закомментировать/удалить строки 61-67 (хардкод credentials)
      - Добавить в начало файла (после #include <time.h>) строку: `#include "config.h"`
      - Убедиться, что все ссылки на BACKEND_HOST, WIFI_SSID и т.д. продолжают работать
    - config.h НЕ должен быть в git — он будет добавлен в .gitignore в задаче 8 (путь: `esp32-cam/ESP32_CAM_recorder/config.h`)
    - config.example.h ДОЛЖЕН быть в git — это шаблон для других пользователей
    - В config.example.h НИКАКИХ реальных данных
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: — | Can parallelize with: 3
  References: ESP32_CAM_recorder.ino строки 61-67, структура после задачи 1
  Acceptance criteria:
    - config.h создан с реальными данными
    - config.example.h создан с плейсхолдерами
    - .ino файл включает config.h и компилируется
    - В .ino больше нет хардкода credentials
  QA scenarios:
    - Happy: `grep "WIFI_SSID" esp32-cam/ESP32_CAM_recorder/ESP32_CAM_recorder.ino` — находит только в #include, не в const char*
    - Happy: проверить что config.example.h не содержит реальных данных
    - Failure: попробовать скомпилировать без config.h — должна быть ошибка компиляции (требует include)
  Evidence: .omo/evidence/2-config-h/
  Commit: Y | feat(esp32-cam): extract credentials to config.h + config.example.h

- [x] 3. ESP32-CAM: BURST_MAX_PHOTOS 4→10 + long-press button + LED flash
  What to do / Must NOT do:
    - В ESP32_CAM_recorder.ino:
      a) Поменять `#define BURST_MAX_PHOTOS 4` на `#define BURST_MAX_PHOTOS 10` (строка 81)
      b) Добавить `#define BUTTON_LONG_PRESS_MS 1500` (после BUTTON_DEBOUNCE_MS)
      c) Добавить переменные: `static bool buttonWasPressed = false; static unsigned long pressTime = 0; static bool longPressHandled = false;` (рядом с lastButtonPressMs)
      d) Добавить `#define FLASH_LED_PIN 4` (если не определён)
      e) ПЕРЕПИСАТЬ обработку кнопки в loop() (строки 684-691) на конечный автомат:

      ```cpp
      bool buttonIsPressed = (digitalRead(SHUTTER_BUTTON_PIN) == LOW);
      unsigned long now = millis();

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
              // Короткое нажатие: фото в серию
              captureIntoBurst();
              // Мигнуть вспышкой 1 раз
              digitalWrite(FLASH_LED_PIN, HIGH); delay(50); digitalWrite(FLASH_LED_PIN, LOW);
          }
      }

      // Длинное удержание — отправка серии
      if (buttonIsPressed && buttonWasPressed && !longPressHandled && (now - pressTime >= BUTTON_LONG_PRESS_MS)) {
          longPressHandled = true;
          if (burstCount == 0) {
              // Буфер пуст: мигнуть вспышкой 1× коротко
              for (int i=0; i<1; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(100); digitalWrite(FLASH_LED_PIN, LOW); delay(100); }
          } else {
              String result = sendBurstToBackend();
              if (result.indexOf("error") >= 0) {
                  // Ошибка: мигнуть вспышкой 2×
                  for (int i=0; i<2; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(200); digitalWrite(FLASH_LED_PIN, LOW); delay(200); }
              } else {
                  // Успех: мигнуть вспышкой 3×
                  for (int i=0; i<3; i++) { digitalWrite(FLASH_LED_PIN, HIGH); delay(100); digitalWrite(FLASH_LED_PIN, LOW); delay(100); }
              }
          }
      }
      ```

      f) В setup() добавить `pinMode(FLASH_LED_PIN, OUTPUT); digitalWrite(FLASH_LED_PIN, LOW);`
    - НЕ менять sendBurstToBackend() — она уже вызывает freeBurstBuffers()
    - НЕ менять логику видео/таймлапса — только фото-серию и кнопку
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: — | Can parallelize with: 2
  References: ESP32_CAM_recorder.ino строки 80-88 (дефайны), 144 (lastButtonPressMs), 678-691 (loop + button handler), 300-377 (burst functions)
  Acceptance criteria:
    - После компиляции: BURST_MAX_PHOTOS = 10
    - Короткое нажатие кнопки (<1.5с): captureIntoBurst() вызывается ровно 1 раз
    - Длинное нажатие (≥1.5с): sendBurstToBackend() вызывается ровно 1 раз
    - После отправки: burstCount = 0
    - Вспышка мигает 1× при пустом буфере, 2× при ошибке, 3× при успехе
  QA scenarios:
    - Static analysis: проверить что state machine не имеет deadlock (каждое нажатие завершается)
    - Happy: все три сценария кнопки отрабатывают
    - Failure: буфер пуст + long-press = мигание 1×, буфер не очищается (и так пуст)
  Evidence: .omo/evidence/3-button-burst/
  Commit: Y | feat(esp32-cam): burst 10 photos, long-press button send, LED flash feedback

- [x] 4. microReader: расширить struct settings — добавить поля backendHost, backendPort, deviceKey
  What to do / Must NOT do:
    - В `microreader/firmware/a-microReader-v1.2.ino`:
      - Добавить в struct settings (после snakeBestScore) поля:
        ```cpp
        char backendHost[64] = "";   // хост бэкенд-сервера
        uint16_t backendPort = 0;     // порт бэкенд-сервера
        char deviceKey[64] = "";      // X-Device-Key
        ```
      - Размер data.dat изменится. Чтобы не сбросить существующие настройки (WiFi, яркость, игры) при прошивке — **ДО** `data.read()` в setup() добавить:
        ```cpp
        data.addWithoutWipe(true);   // сохранить старые поля, дописать новые со значениями по умолчанию
        ```
      - После этой initial-миграции `addWithoutWipe(true)` можно убрать (только для первого запуска после прошивки), но для безопасности можно оставить — FileData не меняет поведение, если размер struct не меняется.
      - Добавить комментарий: `// ВНИМАНИЕ: при изменении размера struct FileData перезаписывает data.dat`
    - НЕ добавлять логику работы с этими полями — только объявление
    - НЕ менять существующие поля
  Parallelization: Wave 2 | Blocked by: 1 | Blocks: 5, 6 | Can parallelize with: 2, 3
  References: a-microReader-v1.2.ino строка с struct settings (в районе строк 60-80)
  Acceptance criteria: struct содержит новые поля с дефолтными пустыми значениями
  QA scenarios: скомпилировать проект — без ошибок
  Evidence: .omo/evidence/4-struct-ext/
  Commit: Y | feat(microreader): add backend config fields to settings struct

- [x] 5. microReader: portal.ino — добавить блок "Сервер" для настройки бэкенда
  What to do / Must NOT do:
    - В `microreader/firmware/portal.ino` в функцию `build()`:
      - Добавить новый M_BLOCK_TAB после блока "Другое":
        ```cpp
        M_BLOCK_TAB(
          "Сервер",
          GP.TEXT("backendHost", "Хост бэкенда", sets.backendHost, "", 63);
          GP.BREAK();
          GP.NUMBER("backendPort", "Порт", (int)sets.backendPort);
          GP.BREAK();
          GP.TEXT("deviceKey", "Ключ устройства", sets.deviceKey, "", 63);
        );
        ```
    - В функцию `action()`:
      - В секции `if (p.form("/cfg"))` добавить:
        ```cpp
        p.copyStr("backendHost", sets.backendHost);
        p.copyInt("backendPort", sets.backendPort);
        p.copyStr("deviceKey", sets.deviceKey);
        ```
    - НЕ менять существующие блоки и их содержимое
    - НЕ удалять существующие поля
  Parallelization: Wave 2 | Blocked by: 1, 4 | Blocks: 7 | Can parallelize with: 6
  References: portal.ino build() и action() — весь файл
  Acceptance criteria:
    - После прошивки и входа в веб-портал появился блок "Сервер"
    - Можно ввести/изменить хост, порт, ключ
    - После сохранения значения сохраняются
    - После перезагрузки платы значения восстанавливаются
  QA scenarios:
    - Happy: открыть портал → изменить → сохранить → перезагрузить → проверить что значения сохранились
    - Failure: пустые поля — сохраняются как пустые (логика проверки будет в network.ino)
  Evidence: .omo/evidence/5-portal/
  Commit: Y | feat(microreader): add backend config block to web portal

- [x] 6. microReader: main.ino — добавить down.hold() → enterToGetAnswerMode()
  What to do / Must NOT do:
    - В `microreader/firmware/a-microReader-v1.2.ino`:
      a) После `#include <GyverTimer.h>` добавить:
         `#include <ESP8266HTTPClient.h>`
         `#include <WiFiClient.h>`
      b) Добавить прототип функции перед setup():
         `void enterToGetAnswerMode();`
      c) В setup() добавить `down.setHoldTimeout(1500);` — **ПОСЛЕ** блока `if (sets.leftmode)` (после строки `down = Button(UP_BTN_PIN);`), иначе в режиме левши timeout сбросится при ре-assign кнопок.
      d) В loop() после секции `up.hold()` добавить:
         ```cpp
         if (down.hold()) {
             uiTimer = millis();
             enterToGetAnswerMode();
         }
         ```
    - НЕ писать тело enterToGetAnswerMode() здесь — оно будет в network.ino
    - НЕ менять существующие хендлеры кнопок
  Parallelization: Wave 2 | Blocked by: 1, 4 | Blocks: 7 | Can parallelize with: 5
  References: a-microReader-v1.2.ino setup(), loop()
  Acceptance criteria: компиляция проходит, удержание DOWN не конфликтует с UP/OK
  QA scenarios: проверка что все hold-хендлеры имеют разные кнопки (UP=игры, OK=WiFi, DOWN=ответ)
  Evidence: .omo/evidence/6-main-hook/
  Commit: Y | feat(microreader): add down.hold() handler for answer polling

- [x] 7. microReader: network.ino — HTTP GET /api/latest + сохранение ответа
  What to do / Must NOT do:
    - Создать `microreader/firmware/network.ino` с функцией:
      ```cpp
      void enterToGetAnswerMode() {
          // 1. Валидация конфига
          if (strlen(sets.backendHost) == 0 || sets.backendPort == 0 || strlen(sets.deviceKey) == 0) {
              oled.clear(); oled.home();
              oled.println("Ошибка:"); oled.println("Настрой сервер"); oled.println("в веб-портале");
              oled.update(); delay(2000);
              return;
          }

          // 2. Подключение к WiFi
          oled.clear(); oled.home();
          oled.println("Получение ответа:");
          oled.println("Подключение к WiFi...");
          oled.update();

          WiFi.mode(WIFI_STA);
          WiFi.begin(sets.staSsid, sets.staPass);

          int timeout = 15; // секунд
          while (WiFi.status() != WL_CONNECTED && timeout > 0) {
              delay(1000);
              timeout--;
              oled.setCursor(0, 4);
              oled.printf("Осталось: %d сек", timeout);
              oled.update();
          }

          if (WiFi.status() != WL_CONNECTED) {
              oled.clear(); oled.home();
              oled.println("Ошибка WiFi:");
              oled.println("Не удалось");
              oled.println("подключиться");
              oled.update(); delay(2000);
              WiFi.mode(WIFI_OFF);
              return;
          }

      // ⚠️ ВАЖНО: перед использованием этого режима убедись, что в app.py
      // добавлено app.json.ensure_ascii = False (см. примечание в Must NOT have).
      // Иначе русский текст придёт как \uXXXX — OLED покажет кракозябры.

      // 3. HTTP GET
      oled.clear(); oled.home();
      oled.println("Запрос к серверу...");
      oled.update();

      WiFiClient client;
      HTTPClient http;
      String url = "http://" + String(sets.backendHost) + ":" + String(sets.backendPort) + "/api/latest";

      http.begin(client, url);
      http.addHeader("X-Device-Key", sets.deviceKey);
      http.setTimeout(10000);

      int httpCode = http.GET();
      bool success = false;

      if (httpCode == 200) {
          String response = http.getString();
          // Парсинг JSON вручную (лёгкий ответ {"ready":true,"text":"...","source":"...","created_at":...})
          bool ready = response.indexOf("\"ready\":true") >= 0;

          if (ready) {
              // Извлекаем text между "text":" и ","source"
              // Учитываем экранированные кавычки внутри текста (\" != closing ")
              int textStart = response.indexOf("\"text\":\"");
              if (textStart >= 0) {
                  textStart += 8; // длина "text":" 
                  // Ищем закрывающую кавычку, пропуская экранированные \" 
                  int textEnd = -1;
                  for (int i = textStart; i < (int)response.length() - 1; i++) {
                      if (response[i] == '\\' && response[i+1] == '"') {
                          i++; // пропускаем экранированную кавычку
                      } else if (response[i] == '"') {
                          textEnd = i;
                          break;
                      }
                  }
                  if (textEnd > textStart) {
                      String answerText = response.substring(textStart, textEnd);
                      // Экранирование: заменяем \\n на \n, \\\" на \" и т.д.
                      answerText.replace("\\n", "\n");
                      answerText.replace("\\\"", "\"");
                      answerText.replace("\\\\", "\\");

                      // Сохраняем в файл
                          // Найти следующий номер ответа
                          int answerNum = 1;
                          while (LittleFS.exists("/answer_" + String(answerNum) + ".txt")) {
                              answerNum++;
                          }

                          String fn = "/answer_" + String(answerNum) + ".txt";
                          File f = LittleFS.open(fn, "w");
                          if (f) {
                              f.print(answerText);
                              f.close();
                              oled.clear(); oled.home();
                              oled.println("Ответ получен!");
                              oled.println("");
                              oled.print("Сохранено как:");
                              oled.setCursor(0, 4);
                              oled.print(fn);
                              oled.update();
                              success = true;
                              delay(3000);
                          }
                      }
                  }
              } else {
                  oled.clear(); oled.home();
                  oled.println("Нет новых");
                  oled.println("ответов на");
                  oled.println("сервере");
                  oled.update(); delay(2000);
              }
          } else {
              oled.clear(); oled.home();
              oled.println("Ошибка сервера:");
              oled.printf("HTTP %d", httpCode);
              oled.update(); delay(2000);
          }

          http.end();

          // 4. Отключение WiFi
          WiFi.mode(WIFI_OFF);
      }
      ```
    - НЕ добавлять зависимостей от библиотек (ESP8266HTTPClient и WiFiClient уже включены в main.ino)
    - НЕ дублировать код подключения к WiFi — он уже есть в wifi.ino, но здесь он самодостаточен
    - Экранирование JSON: минимальное — замена \\n, \\", \\\\
    - При ошибках не чистить файлы пользователя
  **Перед реализацией:** добавить в `app.py` (после `app = Flask(__name__)`) одну строку:
  ```python
  app.json.ensure_ascii = False   # отключает экранирование Unicode — иначе русский текст придёт как \uXXXX
  ```
  Это не меняет API, не ломает другие эндпоинты. Без этого microReader не сможет прочитать ответ на русском.
  Parallelization: Wave 3 | Blocked by: 4, 5, 6 | Blocks: — | Can parallelize with: —
  References: сетевые функции wifi.ino, формат ответа /api/latest (app.py:205-217)
  Acceptance criteria:
    - Компиляция проходит (проверить arduino-cli или визуально)
    - Удержание DOWN → визуальный фидбек на OLED → подключение к WiFi → HTTP GET → сохранение .txt → отключение WiFi
    - Если ответа нет — "Нет новых ответов"
    - Если WiFi нет — "Ошибка WiFi"
    - Сохранённый файл можно открыть штатным ридером microReader
  QA scenarios:
    - Happy: ответ есть → файл создаётся
    - Happy: ответа нет → "Нет новых ответов"
    - Failure: WiFi не подключился → "Ошибка WiFi"
    - Failure: HTTP ошибка → "Ошибка сервера: HTTP NNN"
    - Failure: хост/порт/ключ не настроены → "Настрой сервер в веб-портале"
  Evidence: .omo/evidence/7-network/
  Commit: Y | feat(microreader): add HTTP polling for backend answers + save as .txt

- [x] 8. Создать .gitignore (файла пока нет, нужно создать с нуля)
  What to do / Must NOT do:
    - Создать корневой `.gitignore` со следующим содержимым:
      ```gitignore
      # ESP32-CAM config (содержит SSID/пароль/IP/ключ)
      esp32-cam/ESP32_CAM_recorder/config.h

      # Environment
      .env

      # Python
      __pycache__/
      *.pyc
      venv/

      # Arduino compiled binaries
      *.bin
      *.elf
      *.map

      # IDE
      .vscode/
      .idea/
      ```
    - НЕ добавлять в .gitignore файлы, которые должны быть в git (config.example.h, .env.example)
    - НЕ добавлять всю папку esp32-cam/ в gitignore — только конкретный config.h
  Parallelization: Wave 1 | Blocked by: — | Blocks: — | Can parallelize with: 1
  References: .gitignore не существует — создать с нуля
  Acceptance criteria:
    - config.h находится в gitignore
    - config.example.h НЕ в gitignore
    - .env.example НЕ в gitignore
    - `git add --dry-run esp32-cam/config.h` показывает что файл проигнорирован
  QA scenarios:
    - Happy: `git check-ignore esp32-cam/config.h` возвращает путь (файл игнорируется)
    - Failure: `git check-ignore esp32-cam/config.example.h` не должен ничего вернуть
  Evidence: .omo/evidence/8-gitignore/
  Commit: Y | chore: add gitignore rules for credentials and build artifacts

- [ ] 9. README.md — полная документация системы
  What to do / Must NOT do:
    - Переписать корневой README.md, включив:
      - **Архитектура**: схема взаимодействия ESP32-CAM → Backend (Gemini) → microReader
      - **Структура репозитория**: описание каждой папки
      - **Компонент 1: ESP32-CAM**:
        - Что делает (серийная съёмка до 10 фото, отправка длинным нажатием)
        - Настройка: скопировать config.example.h → config.h, заполнить данные
        - Использование: коротко=фото, длинно=отправка, LED фидбек
      - **Компонент 2: Бэкенд** (перенести существующую инструкцию из старого README)
        - Быстрый старт на VPS
        - systemd service
        - Эндпоинты
      - **Компонент 3: microReader**:
        - Загрузить прошивку через Arduino IDE
        - Настроить STA (Wi-Fi) через веб-портал
        - Настроить сервер (хост/порт/ключ) в блоке "Сервер"
        - Удержание DOWN = получить ответ
        - Ответ читается как .txt файл
      - **Сценарий использования** (на уроке):
        1. Включить ESP32-CAM и microReader
        2. Убедиться, что оба подключены к точке доступа телефона
        3. Кнопка на ESP32-CAM: коротко × N = фото в серию
        4. Кнопка удержание = отправить на сервер
        5. Ждать (Gemini обрабатывает)
        6. На microReader удержать DOWN = получить ответ
        7. Открыть answer_N.txt в ридере
      - **Security notes**:
        - Все credentials в .gitignore (config.h, .env)
        - Никогда не коммитить config.h
        - microReader хранит креды в LittleFS — не светить в веб-портале на публике
      - **Требования к ПО**:
        - Arduino IDE с esp32 и esp8266 пакетами
        - Python 3 + requirements.txt
    - НЕ указывать реальные IP/ключи/пароли — только плейсхолдеры
    - НЕ писать слишком много —结构化 и по делу
  Parallelization: Wave 4 | Blocked by: 2, 3, 7, 8 | Blocks: — | Can parallelize with: —
  References: старый README.md, файлы прошивок после всех изменений
  Acceptance criteria: README.md содержит все 4 раздела, нет реальных credentials, инструкции рабочие
  QA scenarios: прочитать README от начала до конца — нет упоминаний реальных IP/ключей
  Evidence: .omo/evidence/9-readme/
  Commit: Y | docs: comprehensive README with architecture, setup, scenario, security notes

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.
- [ ] F1. Plan compliance audit — проверить что выполнены все 9 todos
- [ ] F2. Security audit — grep всех файлов на наличие реальных IP/ключей/паролей
- [ ] F3. Compilation check — arduino-cli compile для ESP32-CAM и microReader
- [ ] F4. Review README — полнота и отсутствие секретов

## Commit strategy
- Каждый todo → отдельный коммит с типом feat/chore/docs
- Последовательность: 1 → 8 → (2,3,4 в любом порядке) → (5,6,7 в любом порядке) → 9
- Изменение app.py (ensure_ascii) — часть Task 7, commit вместе с network.ino:
  `feat(microreader): add HTTP polling for backend answers + enable UTF-8 in Flask`
- Формат: `тип(компонент): краткое описание`
- Финальная проверка: `git log --oneline` — чистая история

## Success criteria
1. ✅ ESP32-CAM: 10 фото в серию, long-press отправляет, LED фидбек
2. ✅ microReader: удержание DOWN → получение ответа → сохранение как .txt
3. ✅ Никакие credentials не попали в git (grep проверка)
4. ✅ README описывает всю систему от начала до конца
5. ✅ После `bun run build` (или аналога) — без ошибок компиляции
