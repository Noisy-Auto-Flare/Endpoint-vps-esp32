// Меню ответов v1.3: список локальных answer_*.txt + мониторинг сервера.
// Вход — удержание ВНИЗ из главного меню. Выход — удержание ВВЕРХ/ВНИЗ.
#define ANS_MAX 40

String ansFiles[ANS_MAX];
int ansNums[ANS_MAX];
int ansCount = 0;

// Перестроить список /answer_*.txt, сортировка по номеру (старые -> новые).
void ansRebuildList() {
  ansCount = 0;
  Dir root = LittleFS.openDir("/");
  while (root.next()) {
    yield();
    String nm = root.fileName();
    if (!nm.startsWith("/")) nm = "/" + nm;
    if (nm.startsWith("/answer_") && nm.endsWith(".txt")) {
      int num = nm.substring(8, nm.length() - 4).toInt();
      if (num <= 0) continue;
      if (ansCount >= ANS_MAX) continue;
      // Вставка с сохранением порядка по номеру
      int pos = ansCount;
      while (pos > 0 && ansNums[pos - 1] > num) {
        ansNums[pos] = ansNums[pos - 1];
        ansFiles[pos] = ansFiles[pos - 1];
        pos--;
      }
      ansNums[pos] = num;
      ansFiles[pos] = nm.substring(1);  // без ведущего "/" для enterToReadTxtFile
      ansCount++;
    }
  }
}

// Опрос GET /api/photo-status без собственной отрисовки.
// Возвращает HTTP-код (200 — успех, иначе ошибка). Отрисовку ошибки делает вызывающий.
int ansPollStatus(int &staged, String &photoStatus, bool &answerReady) {
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + String(sets.backendHost) + ":" + String(sets.backendPort) + "/api/photo-status";

  http.begin(client, url);
  http.addHeader("X-Device-Key", sets.deviceKey);
  http.setTimeout(10000);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    int pStaged = response.indexOf("\"staged\":");
    if (pStaged >= 0) {
      staged = response.substring(pStaged + 9).toInt();
    }
    int pStatus = response.indexOf("\"photo_status\":\"");
    if (pStatus >= 0) {
      int vStart = pStatus + 16;  // длина "photo_status":"
      int vEnd = response.indexOf("\"", vStart);
      if (vEnd > vStart) {
        photoStatus = response.substring(vStart, vEnd);
      }
    }
    answerReady = response.indexOf("\"answer_ready\":true") >= 0;
  }

  http.end();
  return httpCode;
}

// Подключение к WiFi с 15-секундным отсчётом (как в v1.2).
bool ansConnectWifi() {
  oled.clear(); oled.home();
  oled.println("Ответы:");
  oled.println("Подключение к WiFi...");
  oled.update();

  WiFi.mode(WIFI_STA);
  WiFi.begin(sets.staSsid, sets.staPass);

  int timeout = 15;  // секунд
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
    return false;
  }
  return true;
}

void ansDrawMenu(int ansCursor, bool monitoring, int staged, const String& photoStatus) {
  oled.clear();
  oled.home();
  oled.line(0, 10, 127, 10);
  oled.print(F("ОТВЕТОВ:"));
  oled.print(ansCount);

  int totalRows = ansCount + 1;  // строка 0 — мониторинг, далее файлы
  int sidx = (ansCursor < 5 ? 0 : ansCursor - 4);  // начальный индекс окна (5 строк: 2..6)
  for (int r = sidx; r < sidx + 5 && r < totalRows; r++) {
    oled.setCursor(6, 2 + (r - sidx));
    if (r == 0) {
      if (monitoring) oled.print(F("Мониторинг: ВКЛ"));
      else oled.print(F("Мониторинг: ВЫКЛ"));
    } else {
      oled.print(ansFiles[r - 1]);
    }
    yield();
  }

  int cp = constrain(ansCursor - sidx, 0, 4) + 2;
  oled.setCursor(0, cp);
  oled.print(">");

  oled.setCursor(0, 7);  // строка счётчика/статуса
  if (monitoring) {
    oled.print(F("К:"));
    oled.print(staged);
    oled.print(F(" "));
    oled.print(photoStatusRu(photoStatus));
  } else {
    oled.print(F("OK-выбор"));
  }

  checkBatteryCharge();
  drawBatteryCharge();
  oled.update();
}

void enterAnswersMenu() {
  ansRebuildList();
  int ansCursor = 0;
  bool monitoring = false;
  int staged = 0;
  String photoStatus = "idle";
  bool answerReady = false;
  unsigned long enterTime = millis();
  unsigned long lastPoll = 0;
  uiTimer = millis();

  ansDrawMenu(ansCursor, monitoring, staged, photoStatus);

  while (true) {
    up.tick();
    ok.tick();
    down.tick();

    if (millis() - enterTime > 800 && (up.hold() || down.hold())) {
      break;  // удержание ВВЕРХ/ВНИЗ — выход
    }

    int totalRows = ansCount + 1;
    if (up.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor - 1, 0, totalRows - 1);
      ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
    } else if (down.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor + 1, 0, totalRows - 1);
      ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
    }

    if (ok.click()) {
      uiTimer = millis();
      if (ansCursor == 0) {
        // Переключение мониторинга
        if (!monitoring) {
          if (strlen(sets.backendHost) == 0 || sets.backendPort == 0 || strlen(sets.deviceKey) == 0) {
            oled.clear(); oled.home();
            oled.println("Ошибка:");
            oled.println("Настрой сервер");
            oled.println("в веб-портале");
            oled.update(); delay(2000);
            ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
          } else if (ansConnectWifi()) {
            monitoring = true;
            int code = ansPollStatus(staged, photoStatus, answerReady);
            lastPoll = millis();
            if (code != 200) {
              oled.clear(); oled.home();
              oled.println("Ошибка сервера:");
              oled.printf("HTTP %d", code);
              oled.update(); delay(1500);
            }
            ansRebuildList();
            ansCursor = constrain(ansCursor, 0, ansCount);
            ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
          } else {
            ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
          }
        } else {
          monitoring = false;
          WiFi.mode(WIFI_OFF);
          ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
        }
      } else {
        // Открыть файл штатным ридером
        int idx = ansCursor - 1;
        if (idx >= 0 && idx < ansCount) {
          selectedFile = ansFiles[idx];
          enterToReadTxtFile();
          ansRebuildList();
          ansCursor = constrain(ansCursor, 0, ansCount);
          uiTimer = millis();
          ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
          lastPoll = millis();  // не опрашивать сразу после возврата из ридера
        }
      }
    }

    // Тик мониторинга: опрос раз в секунду
    if (monitoring && millis() - lastPoll >= 1000) {
      lastPoll = millis();
      int code = ansPollStatus(staged, photoStatus, answerReady);
      if (code == 200) {
        if (answerReady) {
          // Снимок списка, чтобы найти новый файл после сохранения
          String ansBefore[ANS_MAX];
          int ansBeforeCount = ansCount;
          for (int i = 0; i < ansCount; i++) ansBefore[i] = ansFiles[i];
          bool got = fetchAndSaveAnswer();
          if (got) {
            WiFi.mode(WIFI_OFF);
            monitoring = false;
            ansRebuildList();
            String newFn = "";
            for (int i = 0; i < ansCount; i++) {
              bool found = false;
              for (int j = 0; j < ansBeforeCount; j++) {
                if (ansFiles[i] == ansBefore[j]) { found = true; break; }
              }
              if (!found) { newFn = "/" + ansFiles[i]; break; }
            }
            if (newFn == "") {
              newFn = ansCount > 0 ? "/" + ansFiles[ansCount - 1] : "/answer_N.txt";
            }
            oled.clear(); oled.home();
            oled.println("Получено новое:");
            oled.println(newFn);
            oled.update(); delay(2000);
            ansCursor = constrain(ansCursor, 0, ansCount);
            ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
          } else {
            ansRebuildList();
            ansCursor = constrain(ansCursor, 0, ansCount);
            ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
          }
        } else {
          ansRebuildList();
          ansCursor = constrain(ansCursor, 0, ansCount);
          ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
        }
      } else {
        oled.clear(); oled.home();
        oled.println("Ошибка сервера:");
        oled.printf("HTTP %d", code);
        oled.update(); delay(1500);
        ansDrawMenu(ansCursor, monitoring, staged, photoStatus);
      }
    }

    delay(20);
  }

  // Любой выход: мониторинг ВЫКЛ + WiFi выкл + главное меню
  WiFi.mode(WIFI_OFF);
  uiTimer = millis();
  checkFileSystem();
  drawMainMenu();
}
