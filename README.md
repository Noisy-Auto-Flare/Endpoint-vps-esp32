# Шпора-backend

Посредник между ESP32-CAM / microReader и Google AI Studio (Gemini API).

## Получить API-ключ

https://aistudio.google.com/apikey — создаётся бесплатно, привязка карты не нужна для базового лимита.

## Быстрый старт на VPS

```bash
# 1. Python и venv (если ещё не стоят)
sudo apt update && sudo apt install -y python3 python3-venv python3-pip

# 2. Клонируй/скопируй эту папку на сервер, зайди в неё
cd shpora-backend

# 3. Виртуальное окружение
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# 4. Настрой .env
cp .env.example .env
nano .env   # впиши GOOGLE_API_KEY и придумай DEVICE_KEY, при желании поменяй PORT

# 5. Тестовый запуск (для проверки)
export $(cat .env | xargs)
python3 app.py
```

### Порт

Порт задаётся в `.env` (переменная `PORT`, по умолчанию 8080). Если он занят —
поменяй значение в `.env`, либо запусти с явным флагом (перекрывает .env):

```bash
python3 app.py --port 8090
```

Проверка с локальной машины:
```bash
curl http://ТВОЙ_IP:8080/api/health
# должно вернуть {"status":"ok","model":"gemini-3.1-flash-lite"}
```

## Постоянная работа через systemd

Создай файл `/etc/systemd/system/shpora-backend.service`:

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

(Порт здесь подхватится из `EnvironmentFile` автоматически.)

Дальше:
```bash
sudo systemctl daemon-reload
sudo systemctl enable shpora-backend
sudo systemctl start shpora-backend
sudo systemctl status shpora-backend   # проверить, что запустился
```

## Открыть порт в файрволе (если используешь ufw)

```bash
sudo ufw allow 8080/tcp   # или свой порт, если менял
```

Для продакшена лучше поставить nginx как reverse proxy с HTTPS (Let's Encrypt) перед сервером, но для старта и тестов можно обращаться напрямую по IP:порту.

## Проверка вручную (эмулируем устройства)

Отправить тестовое фото:
```bash
curl -X POST http://ТВОЙ_IP:8080/api/photo \
  -H "X-Device-Key: твой_device_key" \
  --data-binary @test.jpg
```

Отправить тестовое аудио (WAV):
```bash
curl -X POST http://ТВОЙ_IP:8080/api/voice \
  -H "X-Device-Key: твой_device_key" \
  --data-binary @test.wav
```

Забрать последний готовый ответ (то, что будет делать шпора):
```bash
curl http://ТВОЙ_IP:8080/api/latest \
  -H "X-Device-Key: твой_device_key"
```

## Что дальше

- Код ESP32-CAM: добавить отправку кадра на `/api/photo`
- Код microReader: добавить опрос `/api/latest` + (по желанию) запись голоса и отправку на `/api/voice`

## Про смену провайдера в будущем

Сейчас используется только Google AI Studio (одна модель на всё — и фото, и голос,
она умеет понимать аудио напрямую, без отдельной транскрипции). Если позже
понадобится более точная транскрипция (например, для сложных терминов) или
захочется сравнивать ответы нескольких моделей — можно вернуться к схеме через
OpenRouter или добавить отдельный шаг Whisper, структура кода это позволяет
без сильной переделки (замена одной функции `call_gemini`).
