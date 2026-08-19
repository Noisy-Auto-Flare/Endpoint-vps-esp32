# Полный гайд настройки проекта «Endpoint / Шпора» с нуля

> **Дата**: 2026-07-26
> **Версия прошивок**: ESP32-CAM Recorder v1.0 / microReader v1.2

---

## TL;DR (Для человека)

Проект состоит из трёх частей:

1. **VPS-сервер (бэкенд)** — Python-сервер (Flask), который принимает фото от ESP32-CAM и аудио от microReader, отправляет их в Google Gemini AI и отдаёт готовый ответ обратно.
2. **ESP32-CAM** — плата с камерой, делает фото заданий и отправляет на сервер.
3. **microReader** — устройство на ESP8266 с экраном, получает ответ от сервера и показывает его.

**Схема работы**: ESP32-CAM (фото) → VPS (Gemini AI) → microReader (ответ)

---

## Оглавление

- [Часть 0: Подготовка — что нужно иметь](#part-0)
- [Часть 1: ESP32-CAM — подключение кнопки](#part-1-cam-wiring)
- [Часть 2: VPS — настройка бэкенда](#part-2-vps)
- [Часть 3: ESP32-CAM — прошивка](#part-3-cam-firmware)
- [Часть 4: microReader — прошивка](#part-4-reader-firmware)
- [Часть 5: microReader — настройка через веб-портал](#part-5-reader-config)
- [Часть 6: Сценарий работы](#part-6-usage)
- [Часть 7: Решение проблем](#part-7-troubleshooting)

---

## <a id="part-0"></a>Часть 0: Подготовка — что нужно иметь

### Железо

| Компонент | Описание |
|-----------|----------|
| **ESP32-CAM** (AI Thinker) | Плата с камерой OV2640/OV3660 |
| **microReader** | Собранное устройство на ESP8266 + OLED SSD1306 + кнопки |
| **USB-UART адаптер** | Для прошивки ESP32-CAM (FTDI232, CH340G, CP2102) |
| **USB-кабель microUSB** | Для прошивки microReader |
| **Кнопка (тактовая)** | Для подключения к GPIO13 ESP32-CAM (любая тактовая кнопка) |
| **Провода/перемычки** | Для подключения кнопки |
| **SD-карта** (microSD) | Для ESP32-CAM, отформатирована в FAT32 |
| **Аккумулятор/питание** | Для microReader (Li-Po 502025) |

### ПО на компьютере (Windows 11)

| Программа | Где взять |
|-----------|-----------|
| **Arduino IDE** | Уже установлена |
| **Ядро ESP32** | В Arduino IDE: Меню `Файл → Настройки → Дополнительные ссылки`: `https://espressif.github.io/arduino-esp32/package_esp32_index.json` |
| **Ядро ESP8266** | В Arduino IDE: `https://arduino.esp8266.com/stable/package_esp8266com_index.json` |
| **Git** (опционально) | `winget install Git.Git` |
| **SSH-клиент** | Встроенный в Windows 11 (терминал / PowerShell: `ssh`) |
| **Текстовый редактор** | Notepad++, VS Code и т.п. |

### Учётные записи

- ✅ **Google AI Studio API ключ** — есть (брать: https://aistudio.google.com/apikey)
- **VPS / облачный сервер** — Ubuntu/Debian ✅

### Данные, которые понадобятся

- `GOOGLE_API_KEY` — твой ключ Gemini API
- `DEVICE_KEY` — придумать случайную строку (пароль для устройств)
- `WIFI_SSID` + `WIFI_PASSWORD` — Wi-Fi сеть, к которой подключатся устройства
- IP-адрес или домен VPS
- Порт для бэкенда (по умолчанию 8080, можно любой свободный)

---

## <a id="part-1-cam-wiring"></a>Часть 1: ESP32-CAM — подключение кнопки

### Схема подключения

Физическая кнопка на ESP32-CAM подключается элементарно:

```
GPIO13 ──────┬────── Кнопка (тактовая)
             │
             │    ┌───┐
             └────┤   │
                  │   │
GND  ────────────┴───┘
```

**Как подключить:**

1. Возьми тактовую кнопку (4-пиновую, но используются только 2 контакта — любые два с одной стороны).
2. Один контакт кнопки соедини с **GPIO13** на ESP32-CAM.
3. Второй контакт кнопки соедини с **GND** (любой земляной пин на ESP32-CAM).
4. **Никаких внешних резисторов не нужно** — в прошивке включена внутренняя подтяжка `INPUT_PULLUP`.

### Расположение GPIO13 на AI Thinker ESP32-CAM

На плате AI Thinker ESP32-CAM пины выведены по краям. GPIO13 находится на нижней колодке (со стороны камеры), вторая ножка слева. Если смотреть на плату камерой вверх:

```
  ┌──────────────────────────────────────┐
  │    ╔═══╗                             │
  │    ║   ║  Камера (объектив сверху)   │
  │    ╚═══╝                             │
  │                                      │
  │  GPIO13                              │
──┤ ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ●● │
  │                                      │
──┤ ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ● ●● │
  │  GND ●                               │
  └──────────────────────────────────────┘
```

Точнее: возьми мультиметр в режиме прозвонки — найди пин, подписанный `GPIO13` (или `13`), и пин `GND`.

### Подключение USB-UART для прошивки

Для прошивки ESP32-CAM нужен USB-UART адаптер. Подключение:

| USB-UART адаптер | ESP32-CAM |
|-------------------|-----------|
| TXD | U0R (GPIO3) |
| RXD | U0T (GPIO1) |
| VCC (3.3V!) | 3.3V |
| GND | GND |
| GND | IO0 (через кнопку/перемычку на GND — для режима прошивки) |

**ВАЖНО**: ESP32-CAM работает от **5V** на пине 5V (если есть), но UART адаптер должен быть на **3.3V** логике. Лучше всего:
- Питать плату от внешнего 5V (или от USB-UART с 5V на пин 5V)
- UART сигналы (TXD/RXD) — 3.3V

**Режим прошивки**: Зажми кнопку на GPIO0 (или замкни GPIO0 на GND), подключи питание, отпусти через секунду. После этого можно заливать прошивку.

---

## <a id="part-2-vps"></a>Часть 2: VPS — настройка бэкенда

### 2.1. Подключение к VPS через SSH

```bash
ssh root@ТВОЙ_IP_VPS
# или
ssh твой_юзер@ТВОЙ_IP_VPS
```

### 2.2. Базовая настройка сервера

```bash
# Обновление пакетов
sudo apt update && sudo apt upgrade -y

# Установка Python и нужных пакетов
sudo apt install -y python3 python3-venv python3-pip git curl
```

### 2.3. Клонирование репозитория

```bash
cd /opt
sudo git clone https://github.com/ТВОЙ_ЛОГИН/Endpoint-vps-esp32.git shpora-backend
# ИЛИ если репозиторий приватный или ты заливаешь вручную — создай папку и скопируй файлы:
# sudo mkdir -p /opt/shpora-backend
# а затем залей туда app.py, requirements.txt, .env

sudo chown -R $USER:$USER /opt/shpora-backend
cd /opt/shpora-backend
```

### 2.4. Виртуальное окружение и зависимости

```bash
python3 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
```

**Файл `requirements.txt`** должен содержать:

```
flask==3.0.3
requests==2.32.3
gunicorn==22.0.0
python-dotenv==1.0.1
```

### 2.5. Настройка .env

```bash
cp .env.example .env
nano .env
```

Заполни:

```ini
# Ключ Google AI Studio: https://aistudio.google.com/apikey
GOOGLE_API_KEY=AIza...твой_ключ

# Придуманный тобой секретный ключ
DEVICE_KEY=придумай-случайную-строку-123

# Модель Gemini
GEMINI_MODEL=gemini-3.1-flash-lite

# Порт сервера
PORT=8080
```

**Рекомендации по `DEVICE_KEY`:**
- Минимум 16 символов
- Буквы (A-Z, a-z) + цифры + спецсимволы
- Пример: `K7x9mP2qR5vW8nJ1`
- **Запомни его** — он понадобится для ESP32-CAM и microReader

### 2.6. Тестовый запуск

```bash
source venv/bin/activate
python3 app.py
```

Должен увидеть:
```
 * Running on all addresses (0.0.0.0)
 * Running on http://127.0.0.1:8080
 * Running on http://ТВОЙ_IP_VPS:8080
```

Проверь в другом терминале (или через curl с твоего ПК):

```bash
curl http://ТВОЙ_IP_VPS:8080/api/health
# Ответ: {"status":"ok","model":"gemini-3.1-flash-lite"}
```

Останови тестовый запуск (Ctrl+C).

### 2.7. Настройка systemd (автозапуск сервера)

Создай файл сервиса:

```bash
sudo nano /etc/systemd/system/shpora-backend.service
```

Содержимое:

```ini
[Unit]
Description=Shpora backend
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/shpora-backend
EnvironmentFile=/opt/shpora-backend/.env
ExecStart=/opt/shpora-backend/venv/bin/gunicorn -w 1 -b 0.0.0.0:${PORT:-8080} app:app
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

**Примечание**: Вместо `app.run()` мы используем **gunicorn** — production WSGI-сервер. Он тоже установлен через `requirements.txt`.

Активируй и запусти:

```bash
sudo systemctl daemon-reload
sudo systemctl enable shpora-backend
sudo systemctl start shpora-backend
sudo systemctl status shpora-backend
```

Проверь:

```bash
curl http://localhost:8080/api/health
```

**Полезные команды:**
```bash
sudo systemctl status shpora-backend    # статус
sudo systemctl restart shpora-backend   # перезапуск
sudo journalctl -u shpora-backend -f    # логи в реальном времени
```

### 2.8. Настройка файрвола

```bash
sudo ufw allow 8080/tcp
sudo ufw allow ssh
sudo ufw enable    # если ещё не включён
sudo ufw status
```

**Если используешь другой порт** — замени 8080 на свой.

### 2.9. Проверка полного цикла (с ПК)

```bash
# Здоровье
curl http://ТВОЙ_IP_VPS:8080/api/health

# Отправка тестового фото (если есть test.jpg)
curl -X POST http://ТВОЙ_IP_VPS:8080/api/photo \
  -H "X-Device-Key: твой_device_key" \
  --data-binary @test.jpg

# Получение ответа
curl http://ТВОЙ_IP_VPS:8080/api/latest \
  -H "X-Device-Key: твой_device_key"
```

---

## <a id="part-3-cam-firmware"></a>Часть 3: ESP32-CAM — прошивка

### 3.1. Настройка Arduino IDE для ESP32

1. Открой **Arduino IDE**.
2. Перейди: `Файл → Настройки`.
3. В поле **"Дополнительные ссылки для менеджеров плат"** добавь:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
   (если уже есть другие ссылки — добавь через запятую)
4. Нажми **OK**.
5. Перейди: `Инструменты → Плата → Менеджер плат`.
6. Найди **ESP32** и установи (последнюю версию).
7. Подключи ESP32-CAM к компьютеру через USB-UART (см. Часть 1).

### 3.2. Настройка config.h

Найди файл `esp32-cam/ESP32_CAM_recorder/config.example.h` в проекте.

**Скопируй** его как `config.h` (если ещё нет):

```bash
# У себя на ПК, в папке проекта:
copy esp32-cam\ESP32_CAM_recorder\config.example.h esp32-cam\ESP32_CAM_recorder\config.h
```

Открой `config.h` и заполни:

```cpp
// ESP32-CAM credentials
#ifndef CONFIG_H
#define CONFIG_H

const char* WIFI_SSID        = "твой_wi-fi_ssid";
const char* WIFI_PASSWORD    = "твой_wi-fi_пароль";
const char* BACKEND_HOST     = "IP_ТВОЕГО_VPS";   // или домен
const int   BACKEND_PORT     = 8080;               // порт бэкенда
const char* BACKEND_DEVICE_KEY = "твой_device_key"; // тот же, что в .env

#endif
```

**Где взять данные:**
- `WIFI_SSID` / `WIFI_PASSWORD` — твоя домашняя Wi-Fi сеть (или точка доступа телефона, если在校)
- `BACKEND_HOST` — IP-адрес VPS (можно узнать командой `curl ifconfig.me` на VPS)
- `BACKEND_PORT` — порт, который указан в .env (по умолчанию 8080)
- `BACKEND_DEVICE_KEY` — тот же ключ, что в .env на VPS

### 3.3. Открытие и настройка скетча

1. В Arduino IDE: `Файл → Открыть` → выбери `esp32-cam\ESP32_CAM_recorder\ESP32_CAM_recorder.ino`.
2. Настройки платы (`Инструменты`):
   - **Плата**: `AI Thinker ESP32-CAM`
   - **Partition Scheme**: `Huge APP (3MB No OTA/1MB SPIFFS)`
   - **PSRAM**: `Enabled`
   - **Flash Frequency**: `80MHz`
   - **Upload Speed**: `921600`
   - **Port**: выбери COM-порт твоего USB-UART адаптера
   - Остальное — по умолчанию

### 3.4. Загрузка прошивки

1. Переведи ESP32-CAM в **режим прошивки**:
   - Зажми кнопку на GPIO0 (или замкни GPIO0 на GND)
   - Нажми кнопку RESET на ESP32-CAM (или переподключи питание)
   - Отпусти GPIO0 через 1 секунду
2. В Arduino IDE нажми кнопку **Загрузить** (стрелка вправо).
3. После `Connecting...` плата должна прошиться.
4. После загрузки нажми **RESET** на плате.

### 3.5. Проверка

1. Открой **Serial Monitor** (Инструменты → Монитор порта, скорость 115200).
2. После перезагрузки ESP32-CAM должен показать:
   ```
   Камера инициализирована
   SD-карта смонтирована успешно
   Подключение к Wi-Fi...
   Подключено! IP адрес камеры: 192.168.1.xxx
   Веб-сервер запущен
   ```
3. Открой браузер на любом устройстве в той же Wi-Fi сети, перейди по адресу: `http://192.168.1.xxx`
4. Должна открыться веб-страница ESP32-CAM с кнопками управления.

> **Примечание**: Если IP не отображается в Serial Monitor — посмотри его в роутере (DHCP-клиенты) или используй сетевой сканер.

---

## <a id="part-4-reader-firmware"></a>Часть 4: microReader — прошивка

### 4.1. Настройка Arduino IDE для ESP8266

1. Arduino IDE: `Файл → Настройки`.
2. В поле **"Дополнительные ссылки для менеджеров плат"** добавь (если ещё нет):
   ```
   https://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
3. `Инструменты → Плата → Менеджер плат` → найди **ESP8266**.
4. Установи версию **3.1.2** (или новее).

### 4.2. Установка библиотек

Библиотеки для microReader **НЕ ставятся через менеджер библиотек** — их нужно скопировать **с заменой** из папки проекта.

**Порядок действий:**

1. Узнай, где Arduino IDE хранит библиотеки:
   - Обычно: `C:\Users\ИМЯ_ПОЛЬЗОВАТЕЛЯ\Documents\Arduino\libraries\`

2. Скопируй **все папки** из `microReader-repo\libraries\` в папку `Arduino\libraries\` **с заменой**:

   | Папка в проекте | Скопировать в |
   |----------------|---------------|
   | `microReader-repo/libraries/EncButton-main/` | `Arduino/libraries/EncButton-main/` |
   | `microReader-repo/libraries/FileData/` | `Arduino/libraries/FileData/` |
   | `microReader-repo/libraries/GyverIO-main/` | `Arduino/libraries/GyverIO-main/` |
   | `microReader-repo/libraries/GyverOLED_fix/` | `Arduino/libraries/GyverOLED_fix/` |
   | `microReader-repo/libraries/GyverPortal/` | `Arduino/libraries/GyverPortal/` |
   | `microReader-repo/libraries/GyverTimer/` | `Arduino/libraries/GyverTimer/` |
   | `microReader-repo/libraries/StringUtils/` | `Arduino/libraries/StringUtils/` |
   | `microReader-repo/libraries/TJpg_Decoder/` | `Arduino/libraries/TJpg_Decoder/` |

   **ВАЖНО**: Копировать именно **с заменой** — если библиотеки уже есть, замени их версиями из проекта, иначе прошивка может не скомпилироваться.

   **Самый простой способ**: удалить папку `libraries` в Arduino, затем скопировать туда все из `microReader-repo/libraries/`.

### 4.3. Открытие и настройка скетча

1. Arduino IDE: `Файл → Открыть` → выбери `microreader/firmware/a-microReader-v1.2.ino`.
2. Настройки платы (`Инструменты`):
   - **Плата**: `Generic ESP8266 Module`
   - **Flash Size**: `4M (FS:2MB OTA:~1019KB)`
   - **CPU Frequency**: `80MHz`
   - **Upload Speed**: `921600`
   - **Port**: выбери COM-порт microReader (обычно один)
   - **Erase Flash**: `All flash` (при первой прошивке)
   - Остальное — по умолчанию

3. Подключи microReader к компьютеру через microUSB.

### 4.4. Загрузка прошивки

1. Нажми **Загрузить** (стрелка вправо) в Arduino IDE.
2. Дождись окончания компиляции и загрузки.
3. После прошивки отключи и снова подключи microReader к питанию.

### 4.5. Проверка после прошивки

При включении:
- Встроенный светодиод на ESP8266 быстро мигнёт 6 раз (индикатор успешного запуска).
- На экране OLED появится главное меню: `ФАЙЛОВ: 0` (файлов пока нет).
- Если экран не загорелся — проверь пайку дисплея и контакты I2C (SDA на GPIO4, SCL на GPIO5).

---

## <a id="part-5-reader-config"></a>Часть 5: microReader — настройка через веб-портал

### 5.1. Вход в Wi-Fi меню

1. Включи microReader.
2. На главном экране **удерживай среднюю кнопку (OK)** ~1.5 секунды.
3. Устройство перейдёт в Wi-Fi меню.

### 5.2. Подключение к точке доступа microReader

По умолчанию microReader создаёт свою Wi-Fi сеть (режим AP):

1. На экране появится:
   ```
   РЕЖИМ AP
   Сеть: Reader AP
   Ключ: 00000000
   Локал.IP: 192.168.4.1
   ```
2. На телефоне/ноутбуке:
   - Подключись к Wi-Fi сети **"Reader AP"**
   - Пароль: **00000000**
3. Открой браузер, перейди по адресу: **http://192.168.4.1**

### 5.3. Настройка в веб-портале

Откроется веб-страница с несколькими вкладками:

#### Вкладка "Подключение к сети"

Заполни:

| Поле | Значение |
|------|----------|
| **Имя сети** | SSID твоей Wi-Fi сети |
| **Пароль** | Пароль твоей Wi-Fi сети |
| ✅ **Автоподключение** | Включи галочку |

#### Вкладка "Сервер"

Заполни:

| Поле | Что ввести |
|------|------------|
| **Хост бэкенда** | IP-адрес твоего VPS (тот же, что в config.h ESP32-CAM) |
| **Порт** | Порт бэкенда (8080 или какой указал в .env) |
| **Ключ устройства** | Тот же `DEVICE_KEY`, что в .env на VPS |

#### Остальные вкладки (опционально)

| Вкладка | Настройка |
|---------|-----------|
| **Другое** | Режим левши (если нужно), яркость дисплея |
| **Файловая система** | Загрузка/удаление .txt файлов (шпор) |
| **Обновление прошивки** | OTA-обновление (по воздуху) |

#### Сохранение

Нажми кнопку **"Сохранить"** внизу страницы. Настройки запишутся в файловую систему microReader.

### 5.4. Выход из Wi-Fi меню

- Нажми **среднюю кнопку (OK)** — устройство вернётся в главное меню.
- Или подожди 5 минут — тайм-аут автоматически выйдет.

### 5.5. Проверка получения ответа

1. Убедись, что microReader подключён к Wi-Fi (в настройках включено автоподключение).
2. На главном экране **удерживай кнопку ВНИЗ (DOWN)** ~1.5 секунды.
3. Экран покажет:
   ```
   Получение ответа:
   Подключение к WiFi...
   ```
4. Затем:
   ```
   Запрос к серверу...
   ```
5. Если на сервере есть ответ — сохранится файл `answer_1.txt`.
6. Если ответа нет — `Нет новых ответов на сервере`.

---

## <a id="part-6-usage"></a>Часть 6: Сценарий работы (на уроке)

### Полный цикл

```
1. ESP32-CAM: коротко нажимать кнопку — каждое нажатие = одно фото в серию
   (LED мигает 1 раз — подтверждение)
   Максимум: 10 фото в одной серии

2. ESP32-CAM: удерживать кнопку ≥1.5 с — отправка серии на сервер
   LED: 3× мига = успех, 2× мига = ошибка, 1× (если пусто) = нет фото

3. Подождать 3-15 секунд (Gemini обрабатывает)

4. microReader: удерживать кнопку ВНИЗ ≥1.5 с — получение ответа
   На экране: "Ответ получен! Сохранено как: /answer_1.txt"

5. microReader: открыть answer_1.txt через штатный ридер
   (нажать ОК на файле в главном меню)
```

### Управление ESP32-CAM через веб-страницу

Открой в браузере `http://IP_КАМЕРЫ` (тот, что вывелся в Serial Monitor).

Кнопки на странице:

| Кнопка | Что делает |
|--------|------------|
| **Показать превью** | Живое видео (MJPEG поток) |
| **Отправить кадр в ИИ** | Одно фото сразу в Gemini |
| **Снять кадр в серию** | Добавить фото в серию |
| **Отправить серию в ИИ** | Отправить все накопленные фото |
| **Очистить серию** | Сбросить буфер серии |
| **Зеркало** | Отразить изображение (по горизонтали/вертикали) |
| **Начать видео / Стоп** | Запись последовательности кадров на SD |
| **Начать таймлапс / Стоп** | Периодическая съёмка на SD |

### Управление microReader

| Действие | Результат |
|----------|-----------|
| **ВВЕРХ / ВНИЗ** (коротко) | Навигация по файлам |
| **ВВЕРХ** (удержание) | Игровое меню (динозавр, тетрис, змейка) |
| **OK** (коротко) | Открыть выбранный файл (.txt / .itxt / .jpg) |
| **OK** (удержание) | Wi-Fi меню (настройка) |
| **ВНИЗ** (удержание) | Получить ответ от сервера |
| Запуск с зажатым **ВВЕРХ** | Сервисный режим (сброс/формат) |

---

## <a id="part-7-troubleshooting"></a>Часть 7: Решение проблем

### VPS / Бэкенд

| Проблема | Решение |
|----------|---------|
| `curl: Connection refused` | Проверь, запущен ли сервер: `sudo systemctl status shpora-backend`. Проверь порт в файрволе: `sudo ufw status`. |
| `curl: Connection timed out` | VPS не отвечает — проверь IP, файрвол хостера, не блокирует ли провайдер порт. |
| `{"error":"unauthorized"}` | Неверный `X-Device-Key`. Проверь .env и ключ в запросе. |
| `{"error":"gemini request failed:..."}` | Проблема с Gemini API — проверь `GOOGLE_API_KEY`, квоты, баланс. |
| В логах `ModuleNotFoundError` | Забыл активировать venv или установить requirements: `source venv/bin/activate && pip install -r requirements.txt` |

### ESP32-CAM

| Проблема | Решение |
|----------|---------|
| Не прошивается (ошибка соединения) | Проверь подключение GPIO0 к GND при включении. Попробуй другую скорость (115200). Проверь провода. |
| "Camera init failed" | Проверь PSRAM: Enabled в настройках платы. Проверь, что камера подключена правильно. |
| "SD card mount failed" | Вставь SD-карту. Отформатируй в FAT32. Проверь контакты. |
| "WiFi connection failed" | Проверь SSID/пароль в config.h. Убедись, что Wi-Fi сеть доступна (2.4 GHz!). |
| Не отправляется серия | Закрой вкладку с превью (стрим блокирует сервер). |

### microReader

| Проблема | Решение |
|----------|---------|
| Не включается, LED мигает 2 раза/сек | Ошибка инициализации OLED — проверь пайку SDA/SCL, удали остатки флюса. |
| Не входит в Wi-Fi меню | Проверь пайку кнопки OK и её контактов. |
| "Настрой сервер в веб-портале" | Зайди в веб-портал, заполни Host/Port/Device Key, сохрани. |
| "Ошибка WiFi" при получении ответа | Проверь SSID/пароль в настройках. Убедись, что Wi-Fi в зоне доступа. |
| "Ошибка сервера: HTTP -1" | microReader не может соединиться с VPS — проверь IP/порт бэкенда, файрвол. |
| "Нет новых ответов на сервере" | Сначала отправь фото с ESP32-CAM, потом получай ответ. |
| Файлы не отображаются (например `10[14]`) | Число в скобках — битые/неподдерживаемые файлы. Имена должны быть до 12 символов. |

---

## Приложение A: Полезные ссылки

| Ресурс | Ссылка |
|--------|--------|
| Google AI Studio (API ключ) | https://aistudio.google.com/apikey |
| Ядро ESP32 для Arduino | https://espressif.github.io/arduino-esp32/package_esp32_index.json |
| Ядро ESP8266 для Arduino | https://arduino.esp8266.com/stable/package_esp8266com_index.json |
| Схемы и PCB microReader | В папке `microReader-repo/PCB/` |
| Image Processor (для .itxt) | В папке `microReader-repo/image-processor/` |
| Оригинальный репозиторий microReader | https://github.com/Nich1con/microReader |

## Приложение B: GPIO пины

### ESP32-CAM (AI Thinker)

| Пин | Назначение |
|-----|-----------|
| GPIO13 | Кнопка затвора (вход, INPUT_PULLUP) |
| GPIO4 | Вспышка (LED) |
| GPIO0 | Boot mode (GND = режим прошивки) |
| GPIO1 (U0T) | UART TX |
| GPIO3 (U0R) | UART RX |

### microReader (ESP8266)

| Пин | Назначение |
|-----|-----------|
| GPIO14 | Кнопка ВВЕРХ |
| GPIO12 | Кнопка OK (средняя) |
| GPIO13 | Кнопка ВНИЗ |
| GPIO4 | OLED SDA (I2C) |
| GPIO5 | OLED SCL (I2C) |

---

*Гайд составлен на основе кода проекта `Endpoint-vps-esp32` (бэкенд app.py, прошивка ESP32-CAM Recorder, microReader v1.2).*
