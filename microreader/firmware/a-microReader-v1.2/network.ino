// fetchAndSaveAnswer: одиночный опрос GET /api/latest.
// Требует уже подключённого WiFi. Возвращает true, если ответ
// получен и сохранён как /answer_N.txt. Прогресс — на OLED.
bool fetchAndSaveAnswer() {
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
    return success;
}

void enterToGetAnswerMode() {
    // 1. Валидация конфига
    if (strlen(sets.backendHost) == 0 || sets.backendPort == 0 || strlen(sets.deviceKey) == 0) {
        oled.clear(); oled.home();
        oled.println("Ошибка:");
        oled.println("Настрой сервер");
        oled.println("в веб-портале");
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

    // 3. HTTP GET (одиночный опрос, общий helper с меню статуса)
    fetchAndSaveAnswer();

    // 4. Отключение WiFi
    WiFi.mode(WIFI_OFF);
}

// Русское имя статуса фото для OLED.
String photoStatusRu(const String& s) {
    if (s == "idle") return "Жду";
    if (s == "staged") return "Кадры ждут";
    if (s == "sent_to_llm") return "Думает...";
    if (s == "txt_ready") return "Готов!";
    if (s == "consumed") return "Забран";
    if (s == "error") return "Ошибка";
    return s;
}

// Опрос GET /api/photo-status. Возвращает true при HTTP 200.
// На выходе: staged, photoStatus, answerReady.
bool fetchPhotoStatus(int &staged, String &photoStatus, bool &answerReady) {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + String(sets.backendHost) + ":" + String(sets.backendPort) + "/api/photo-status";

    http.begin(client, url);
    http.addHeader("X-Device-Key", sets.deviceKey);
    http.setTimeout(10000);

    int httpCode = http.GET();
    bool ok = false;

    if (httpCode == 200) {
        String response = http.getString();
        int pStaged = response.indexOf("\"staged\":");
        if (pStaged >= 0) {
            staged = response.substring(pStaged + 9).toInt();
        }
        int pStatus = response.indexOf("\"photo_status\":\"");
        if (pStatus >= 0) {
            int vStart = pStatus + 16; // длина "photo_status":"
            int vEnd = response.indexOf("\"", vStart);
            if (vEnd > vStart) {
                photoStatus = response.substring(vStart, vEnd);
            }
        }
        answerReady = response.indexOf("\"answer_ready\":true") >= 0;
        ok = true;
    } else {
        oled.clear(); oled.home();
        oled.println("Ошибка сервера:");
        oled.printf("HTTP %d", httpCode);
        oled.update(); delay(2000);
    }

    http.end();
    return ok;
}

// Окно статуса фото: 4 строки.
void drawPhotoStatus(int staged, const String& ruStatus) {
    oled.clear(); oled.home();
    oled.print("Кадров: ");
    oled.println(staged);
    oled.print("Статус: ");
    oled.println(ruStatus);
    oled.println("OK=обновить");
    oled.println("UP=забрать");
    oled.update();
}

// Меню статуса фото: staged + photo_status с GET /api/photo-status.
// OK — обновить, UP — забрать ответ (если answer_ready),
// удержание ВНИЗ или 30 с бездействия — выход.
void enterPhotoStatusMenu() {
    // 1. Валидация конфига
    if (strlen(sets.backendHost) == 0 || sets.backendPort == 0 || strlen(sets.deviceKey) == 0) {
        oled.clear(); oled.home();
        oled.println("Ошибка:");
        oled.println("Настрой сервер");
        oled.println("в веб-портале");
        oled.update(); delay(2000);
        return;
    }

    // 2. Подключение к WiFi
    oled.clear(); oled.home();
    oled.println("Статус фото:");
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

    // 3. Цикл меню
    int staged = 0;
    String photoStatus = "idle";
    bool answerReady = false;

    if (fetchPhotoStatus(staged, photoStatus, answerReady)) {
        drawPhotoStatus(staged, photoStatusRu(photoStatus));
    }

    unsigned long enterTime = millis();
    unsigned long lastActivity = millis();

    while (true) {
        up.tick();
        ok.tick();
        down.tick();

        if (millis() - enterTime > 800 && down.hold()) {
            break; // удержание ВНИЗ — выход
        }
        if (millis() - lastActivity > 30000UL) {
            break; // 30 с бездействия — выход
        }

        if (ok.click()) {
            // OK — обновить статус
            if (fetchPhotoStatus(staged, photoStatus, answerReady)) {
                drawPhotoStatus(staged, photoStatusRu(photoStatus));
            }
            lastActivity = millis();
        }

        if (up.click()) {
            // UP — забрать ответ, если готов
            if (answerReady) {
                fetchAndSaveAnswer();
                if (fetchPhotoStatus(staged, photoStatus, answerReady)) {
                    drawPhotoStatus(staged, photoStatusRu(photoStatus));
                }
            } else {
                oled.clear(); oled.home();
                oled.println("Ещё не готово:");
                oled.println("ответа пока");
                oled.println("нет на сервере");
                oled.update(); delay(1500);
                drawPhotoStatus(staged, photoStatusRu(photoStatus));
            }
            lastActivity = millis();
        }

        delay(20);
    }

    // 4. Отключение WiFi
    WiFi.mode(WIFI_OFF);
}
