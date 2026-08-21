# Шпора — монорепозиторий

Три устройства + бэкенд, работающие вместе: **ESP32-CAM** (фото заданий на уроке) → **Backend** (отправка в Gemini AI) → **microReader** (получение ответа), плюс **ESP32-C3 микрофон** (голосовые вопросы).

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐
│  ESP32-CAM  │────▶│   Backend    │────▶│  microReader │
│  (фото)     │     │  (Gemini AI) │     │  (ответ)     │
└─────────────┘     └──────────────┘     └──────────────┘
┌─────────────┐     └───────▲──────┘
│ ESP32-C3    │────▶        │
│ (микрофон)  │     чанки по│ 5 сек, склейка на сервере
└─────────────┘             │
```

## Структура репозитория

```
├── app.py                          # Бэкенд-сервер (Flask)
├── requirements.txt                # Python-зависимости
├── .env.example                    # Шаблон для .env
├── esp32-cam/
│   └── ESP32_CAM_recorder/
│       ├── ESP32_CAM_recorder.ino  # Прошивка ESP32-CAM
│       ├── config.h                # ТВОИ credentials (в .gitignore)
│       └── config.example.h        # Шаблон config.h
├── esp32-c3/
│   └── microphone/
│       ├── microphone.ino          # Прошивка ESP32-C3 микрофона (чанковая запись)
│       ├── config.h                # ТВОИ credentials (в .gitignore)
│       └── config.example.h        # Шаблон config.h
├── microreader/
│   └── firmware/
│       └── a-microReader-v1.2/     # Все файлы прошивки microReader (в одной папке для Arduino IDE)
│           ├── a-microReader-v1.2.ino  # Главный файл прошивки
│           ├── network.ino         # HTTP-опрос бэкенда (ответ по удержанию кнопки ВНИЗ)
│           ├── portal.ino          # Веб-портал конфигурации (вкладка "Сервер")
│           ├── wifi.ino            # WiFi-менеджмент
│           ├── ui.ino              # Интерфейс OLED
│           ├── files.ino           # Файловый менеджер
│           ├── gamemode.ino        # Меню игр
│           ├── dinosaurGame.ino    # Игра "динозавр"
│           ├── tetrisGame.ino      # Игра "тетрис"
│           ├── snakeGame.ino       # Игра "змейка"
│           ├── calcul.ino          # Калькулятор
│           └── servmode.ino        # Сервисный режим
├── .gitignore                      # Игнорируемые файлы
└── README.md                       # Этот файл
```

---

## Компонент 1: ESP32-CAM

### Что делает
- Серийная съёмка до **10 фото** подряд в память платы
- Отправка серии на бэкенд длительным нажатием кнопки
- LED-индикация: 1× миг = фото, 2× = ошибка, 3× = успех

### Настройка
1. Скопируй `esp32-cam/ESP32_CAM_recorder/config.example.h` → `config.h`
2. Заполни реальными данными (Wi-Fi, адрес сервера, ключ)
3. Залей прошивку ESP32_CAM_recorder.ino через Arduino IDE
   - Плата: AI Thinker ESP32-CAM
   - Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
   - PSRAM: Enabled

### Использование
| Действие | Результат |
|----------|-----------|
| **Короткое нажатие** кнопки | Фото добавляется в серию, вспышка мигает 1× |
| **Удержание** кнопки (≥1.5 с) | Отправка всех фото серии на сервер |
| Буфер пуст + удержание | LED мигает 1× (коротко) |
| Ошибка отправки | LED мигает 2× |
| Успешная отправка | LED мигает 3× |

---

## Компонент 2: Бэкенд (Flask)

Посредник между устройствами и Google AI Studio (Gemini API).

### Быстрый старт на VPS

```bash
# 1. Python и venv
sudo apt update && sudo apt install -y python3 python3-venv python3-pip

# 2. Клонируй репозиторий и зайди в папку
cd shpora-backend

# 3. Виртуальное окружение
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# 4. Настрой .env
cp .env.example .env
nano .env   # впиши GOOGLE_API_KEY и придумай DEVICE_KEY, при желании поменяй PORT

# 5. Тестовый запуск
export $(cat .env | xargs)
python3 app.py
```

### Порты
Порт задаётся в `.env` (переменная `PORT`, по умолчанию 8080). Можно переопределить:
```bash
python3 app.py --port 8090
```

Проверка:
```bash
curl http://ТВОЙ_IP:8080/api/health
# {"status":"ok","model":"gemini-3.1-flash-lite"}
```

### Эндпоинты

| Метод | Путь | Описание |
|-------|------|----------|
| GET | `/api/health` | Проверка сервера |
| POST | `/api/photo` | Принять JPEG-фото, отправить в Gemini, сохранить ответ |
| POST | `/api/photos` | Принять серию фото (JSON с base64), отправить в Gemini |
| POST | `/api/voice` | Принять WAV-аудио, отправить в Gemini |
| POST | `/api/voice-chunk` | Принять WAV-чанк (5 сек) от ESP32-C3; при `X-Final: true` склеить сессию и отправить в Gemini |
| GET | `/api/latest` | Отдать последний готовый ответ (однократно) |

Все эндпоинты (кроме `/api/health`) требуют заголовок `X-Device-Key`.

### Проверка вручную

```bash
# Отправить фото
curl -X POST http://ТВОЙ_IP:8080/api/photo \
  -H "X-Device-Key: твой_device_key" \
  --data-binary @test.jpg

# Отправить аудио
curl -X POST http://ТВОЙ_IP:8080/api/voice \
  -H "X-Device-Key: твой_device_key" \
  --data-binary @test.wav

# Забрать ответ
curl http://ТВОЙ_IP:8080/api/latest \
  -H "X-Device-Key: твой_device_key"
```

### systemd

```ini
[Unit]
Description=Shpora backend
After=network.target

[Service]
Type=simple
User=твой_юзер
WorkingDirectory=/путь/до/shpora-backend
EnvironmentFile=/путь/до/shpora-backend/.env
ExecStart=/путь/до/shpora-backend/venv/bin/python3 app.py
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable shpora-backend
sudo systemctl start shpora-backend
sudo systemctl status shpora-backend
```

---

## Компонент 3: microReader

### Что делает
- Получает ответ от бэкенда удержанием кнопки ВНИЗ
- Сохраняет ответ как `answer_N.txt` в файловой системе
- Открывается штатным ридером .txt файлов

### Прошивка

1. Открой `microreader/firmware/a-microReader-v1.2/a-microReader-v1.2.ino` в Arduino IDE
2. Установи ядро ESP8266 v3.1.2 (или новее)
3. Скопируй библиотеки из оригинального репозитория [Nich1con/microReader](https://github.com/Nich1con/microReader) (`libraries/`) в свою папку libraries с заменой
4. Настройки платы: Generic ESP8266 Module, Flash Size: 4M (FS:2MB OTA:~1019KB)
5. Залей прошивку

> ⚠️ **Важно**: если файловая система показывает ~64 КБ вместо 2 МБ — прошивка была собрана с неверным `Flash Size`. Перепрошей через USB один раз с `Flash Size: 4M (FS:2MB OTA:~1019KB)` и `Erase Flash: All flash`, чтобы пересоздать разметку. Дальше обновляй через OTA.

### Настройка

1. Включи microReader, войди в Wi-Fi меню (удержание средней кнопки)
2. Подключись к точке доступа "Reader AP" (пароль: 00000000)
3. Открой браузер, перейди по IP, который показывает дисплей
4. Во вкладке "Подключение к сети" укажи SSID/пароль своей точки доступа
5. Во вкладке **"Сервер"** укажи:
   - **Хост бэкенда** — IP или домен твоего VPS
   - **Порт** — порт бэкенда (по умолчанию 8080)
   - **Ключ устройства** — тот же DEVICE_KEY, что в .env
6. Нажми "Сохранить"

### Получение ответа

1. Убедись, что microReader подключен к Wi-Fi (настройки STA)
2. В главном меню **удерживай кнопку ВНИЗ** (≥1.5 с)
3. Дисплей покажет процесс:
   - "Подключение к WiFi..."
   - "Запрос к серверу..."
   - "Ответ получен! Сохранено как: /answer_1.txt"
4. Открой файл через штатный ридер .txt

---

## Компонент 4: ESP32-C3 микрофон

### Что делает
- Записывает голос **чанками по 5 секунд** (до 30 секунд суммарно) по нажатию кнопки
- Каждый чанк сразу отправляется на бэкенд (`POST /api/voice-chunk`)
- Сервер накапливает чанки сессии, при финальном чанке склеивает их в один WAV и отправляет в Gemini
- Ответ попадает в `/api/latest` — microReader забирает его как обычно (`answer_N.txt`)

### Прошивка

1. Открой `esp32-c3/microphone/microphone.ino` в Arduino IDE
2. Скопируй `config.example.h` → `config.h` и заполни (Wi-Fi, IP сервера, порт, DEVICE_KEY)
3. Плата: ESP32C3 Dev Module, Flash Size: 4MB, Partition: Default 4MB
4. Залей прошивку

### Подключение (MAX4466 → ESP32-C3 SuperMini)

| MAX4466 | ESP32-C3 |
|---------|----------|
| OUT | GPIO4 |
| VCC | 3V3 |
| GND | GND |

Кнопка — встроенная BOOT (GPIO9). LED-индикация на GPIO8 (встроенный, активный LOW).

### Питание от аккумулятора

```
Li-ion 3.7V → повышающий модуль (boost) → 5V → пин 5V платы
                                       GND → GND платы
```
Поставь конденсатор 100–470 мкФ на выходе повышайки, чтобы убрать пульсации (микрофон сидит на ADC).

### Использование

| Действие | Результат |
|----------|-----------|
| Нажатие кнопки (старт) | Запись началась (LED горит), чанки по 5 сек уходят на сервер |
| Нажатие кнопки (стоп) | Последний чанк уходит с `X-Final: true`, сервер склеивает и шлёт в Gemini |
| LED 3 мигания | Успех: всё отправлено |
| LED 2 мигания | Ошибка отправки / записи |
| LED 5 миганий | Критическая ошибка (SPIFFS / нет Wi-Fi) |

### Протокол чанков

```
POST /api/voice-chunk
X-Device-Key: <ключ>
X-Session-Id: <уникальный ID записи>
X-Chunk-Index: <0, 1, 2, ...>
X-Final: true|false        # true только на последнем чанке
Content-Type: audio/wav    # тело — сырые байты WAV (16 кГц, 16 бит, моно)
```

---

## Сценарий использования (на уроке)

1. Включи ESP32-CAM и microReader
2. Убедись, что оба в одной сети Wi-Fi (например, точка доступа телефона)
3. **ESP32-CAM**: нажимай кнопку коротко — каждое нажатие добавляет фото в серию (до 10)
4. **ESP32-CAM**: удерживай кнопку (≥1.5 с) — серия отправляется на сервер
5. Подожди (Gemini обрабатывает фото, обычно 3–10 секунд)
6. **microReader**: удерживай ВНИЗ (≥1.5 с) — устройство получит ответ
7. Открой `answer_N.txt` в ридере и читай

---

## Security notes

- **Все credentials в `.gitignore`**: `config.h` и `.env` не попадут в git
- `config.example.h` — единственный файл с credentials, который должен быть в git (с плейсхолдерами)
- Никогда не коммить `config.h` с реальными данными
- microReader хранит настройки (включая ключ устройства) в LittleFS — не открывай веб-портал на публике
- Бэкенд использует `X-Device-Key` для аутентификации — не используй простые ключи

## Требования к ПО

- **Arduino IDE** с пакетами:
  - ESP32 (для ESP32-CAM): https://espressif.github.io/arduino-esp32/package_esp32_index.json
  - ESP8266 (для microReader): https://arduino.esp8266.com/stable/package_esp8266com_index.json
- **Python 3.8+** + `requirements.txt` для бэкенда
- **microReader libraries**: скопировать из [Nich1con/microReader](https://github.com/Nich1con/microReader) (`libraries/`) с заменой

## Про смену AI-провайдера

Сейчас используется Google AI Studio (Gemini). Если понадобится более точная транскрипция или другая модель — замени одну функцию `call_gemini()` в `app.py`, структура кода это позволяет без переделки API-контракта.
