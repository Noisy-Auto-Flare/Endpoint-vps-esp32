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

    // 4. Отключение WiFi
    WiFi.mode(WIFI_OFF);
}
