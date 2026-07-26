---
slug: workflow-integration
status: review-passed
intent: clear
review_required: true
pending-action: execute .omo/plans/workflow-integration.md
approach: monorepo — все 3 компонента в одном репозитории, разделены папками
---

# Draft: workflow-integration

## Components (topology ledger)
| id | outcome | status | evidence path |
|----|---------|--------|---------------|
| backend | Flask-сервер (app.py) — посредник ESP32-CAM ↔ Gemini. Уже работает. | active | app.py:1-239 |
| esp32-cam | ESP32-CAM прошивка — серийная съёмка + отправка на бэкенд. Уже работает, требует доработки кнопки и BURST_MAX_PHOTOS. | active | ESP32_CAM_recorder.ino:1-713 |
| microreader | ESP8266 читалка — OLED + кнопки. Нужно: HTTP-клиент + polling /api/latest + конфиг бэкенда. | active | microReader firmware: main.ino, portal.ino, wifi.ino, files.ino |

## Open assumptions (announced defaults)
| assumption | adopted default | rationale | reversible? |
|-----------|----------------|-----------|-------------|
| BURST_MAX_PHOTOS | 10 кадров максимум | PSRAM позволяет (4MB, каждый ~150KB → 1.5MB). Можно отправлять и меньше — сколько накопил. | Да |
| Button scheme ESP32-CAM | 1 кнопка: коротко(<1.5с)=фото, длинно(≥1.5с)=отправка | Не требует доп. пайки, логично | Да |
| microReader hold DOWN | "Получить ответ" | Кнопка DOWN свободна (только click), не конфликтует с играми (UP) и WiFi-меню (OK) | Да |
| microReader ответ | Сохраняется как .txt файл на LittleFS | Читается штатным ридером. Можно перечитать позже. Не теряется при выкл. | Да |
| Вынос credentials | отдельный config.h в той же папке, что .ino (в .gitignore) | Arduino IDE не всегда резолвит `../` в include paths | Нет (однократное решение) |

## Findings (cited - path:lines)
1. **ESP32-CAM** уже умеет: серийную съёмку в память (captureIntoBurst, стр. 311-340), отправку серии (sendBurstToBackend, стр. 343-377), очистку буфера после отправки (freeBurstBuffers, стр. 300-308 — вызывается внутри sendBurstToBackend)
2. **ESP32-CAM** credentials жёстко зашиты в коде: WIFI_SSID/PASSWORD (стр. 61-62), BACKEND_HOST/PORT/DEVICE_KEY (стр. 65-67) — УЯЗВИМОСТЬ
3. **microReader** не имеет HTTP-клиента. WiFi используется только для веб-портала (GyverPortal). Outbound HTTP не реализован.
4. **microReader** settings struct хранится в LittleFS через FileData. Легко расширяется новыми полями.
5. **microReader** portal.ino использует GyverPortal build()/action() — стандартный паттерн для добавления полей.
6. **microReader** кнопки: UP=GPIO14, OK=GPIO12, DOWN=GPIO13. В loop() обрабатываются: up.click, down.click, ok.click, ok.hold, up.hold. down.hold() свободен.
7. **Бэкенд** уже имеет GET /api/latest с consumed-на-чтение семантикой (app.py:205-217) — идеально для microReader polling.
8. **Oracle review** выявил critical-проблему: Flask по умолчанию кодирует Unicode как \uXXXX (break кириллицы на OLED). Добавлено исключение в guardrail — `app.json.ensure_ascii = False`.

## Decisions (with rationale)
- **Монорепозиторий** (всё в Endpoint-vps-esp32): одна точка сборки, общий контракт API, согласованность версий прошивок и бэкенда.
- **10 фото максимум** вместо 4: PSRAM позволяет, пользователь не всегда отправляет все 10.
- **config.h в той же папке, что .ino**: Arduino IDE копирует скетч в temp при компиляции, `../config.h` может не сработать.
- **microReader: настройки бэкенда через веб-портал**: не требуется перепрошивка для смены IP/ключа. Хранятся в LittleFS.
- **Ответ сохраняется как .txt**: microReader уже умеет читать .txt с прокруткой. Не нужно писать новый вьюер.
- **Удержание DOWN**: не конфликтует с существующими функциями (UP=игры, OK=WiFi). DOWN пока только click (навигация в файлах).
- **app.json.ensure_ascii = False**: не меняет API, но критически важно для чтения русского текста на microReader.

## Scope IN
1. Реструктуризация репозитория
2. ESP32-CAM: config.h + config.example.h (в папке скетча)
3. ESP32-CAM: BURST_MAX_PHOTOS 10
4. ESP32-CAM: long-press кнопка (коротко=фото, длинно=отправка) + LED flash фидбек
5. microReader: new network.ino (HTTP-клиент)
6. microReader: portal.ino (блок "Сервер")
7. microReader: main.ino (down.hold handler)
8. .gitignore (создать с нуля)
9. README.md с полной документацией
10. app.py: одна строка `app.json.ensure_ascii = False`

## Scope OUT (Must NOT have)
- ❌ НЕ менять API-контракт бэкенда (эндпоинты, формат ответа, заголовки)
- ❌ НЕ писать новый вьюер для microReader (используем существующий .txt reader)
- ❌ НЕ добавлять HTTPS/TLS
- ❌ НЕ добавлять запись видео/таймлапс — только фото-серия
- ❌ НЕ трогать игры/калькулятор microReader

## Open questions
Нет — все решены.

## Review findings (High-Accuracy Review)
**Momus:** OKAY — план в целом корректен. Dependency matrix имеет путаницу меток (строки 4 и 7), что исправлено.
**Oracle:** PASS CONDITIONAL — найдены и исправлены:
- 🔴 CRITICAL: JSON parser не декодирует \uXXXX → добавлен ensure_ascii=False
- 🔴 CRITICAL: .gitignore не существует → создание, не обновление
- 🟠 HIGH: down.setHoldTimeout теряется в leftmode → перенесён после блока leftmode
- 🟡 MEDIUM: GP.NUMBER со String вместо int → исправлен
- 🟡 MEDIUM: data.addWithoutWipe(true) для сохранения настроек → добавлено
- 🟡 MEDIUM: JSON parser сломается на кавычках → исправлен на state-machine поиск
- 🟢 LOW: dependency matrix → исправлена

## Approval gate
status: review-passed
План прошёл high-accuracy review. Все найденные проблемы исправлены.
Для запуска выполнения: `/start-work` с выбором плана workflow-integration.
