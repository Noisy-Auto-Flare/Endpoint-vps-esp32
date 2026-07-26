# Шпора — монорепозиторий

Три компонента, работающие вместе: **ESP32-CAM** (фото заданий на уроке) → **Backend** (отправка в Gemini AI) → **microReader** (получение ответа).

```
┌─────────────┐     ┌──────────────┐     ┌──────────────┐
│  ESP32-CAM  │────▶│   Backend    │────▶│  microReader │
│  (фото)     │     │  (Gemini AI) │     │  (ответ)     │
└─────────────┘     └──────────────┘     └──────────────┘
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
├── microreader/
│   └── firmware/
│       ├── a-microReader-v1.2.ino  # Главный файл прошивки microReader
│       ├── network.ino             # HTTP-опрос бэкенда
│       ├── portal.ino              # Веб-портал конфигурации
│       ├── wifi.ino                # WiFi-менеджмент
│       ├── ui.ino                  # Интерфейс OLED
│       ├── files.ino               # Файловый менеджер
│       ├── gamemode.ino            # Меню игр
│       ├── dinosaurGame.ino        # Игра "динозавр"
│       ├── tetrisGame.ino          # Игра "тетрис"
│       ├── snakeGame.ino           # Игра "змейка"
│       ├── calcul.ino              # Калькулятор
│       ├── servmode.ino            # Сервисный режим
│       └── ...
├── microReader-repo/               # Оригинальный репозиторий microReader
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

1. Открой `microreader/firmware/a-microReader-v1.2.ino` в Arduino IDE
2. Установи ядро ESP8266 v3.1.2 (или новее)
3. Скопируй библиотеки из `microReader-repo/libraries/` в свою папку libraries с заменой
4. Настройки платы: Generic ESP8266 Module, Flash Size: 4M (FS:2MB OTA:~1019KB)
5. Залей прошивку

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
- **microReader libraries**: скопировать из `microReader-repo/libraries/` с заменой

## Про смену AI-провайдера

Сейчас используется Google AI Studio (Gemini). Если понадобится более точная транскрипция или другая модель — замени одну функцию `call_gemini()` в `app.py`, структура кода это позволяет без переделки API-контракта.
