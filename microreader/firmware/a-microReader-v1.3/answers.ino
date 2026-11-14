// microReader v1.3: Меню 2 (ответы) + Меню 3 (мониторинг).
// Вход из главного меню — удержание ВНИЗ. WiFi используется только в Меню 3,
// включается вручную кнопкой и гасится на любом выходе.
#define ANS_MAX 40

String ansFiles[ANS_MAX];   // имена answer_*.txt без ведущего "/" (новые -> старые)
int ansNums[ANS_MAX];       // их числовые суффиксы
int ansCount = 0;
bool ansExitToMain = false;  // Меню 3 просит Меню 2 сразу выйти в главное

String monLines[3];          // живые строки состояния Меню 3
int monLineCount = 0;

// Список /answer_*.txt, сортировка по номеру: НОВЫЕ -> СТАРЫЕ.
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
      int pos = ansCount;
      while (pos > 0 && ansNums[pos - 1] < num) {
        ansNums[pos] = ansNums[pos - 1];
        ansFiles[pos] = ansFiles[pos - 1];
        pos--;
      }
      ansNums[pos] = num;
      ansFiles[pos] = nm.substring(1);  // без "/" для enterToReadTxtFile
      ansCount++;
    }
  }
}

/* ================= Меню 2: ответы (как главное, без WiFi) ================= */
void ansDrawList(int ansCursor) {
  oled.clear();
  oled.home();
  oled.line(0, 10, 127, 10);
  oled.print(F("ОТВЕТЫ:"));
  oled.print(ansCount);

  int totalRows = ansCount + 1;  // строка 0 — "Мониторинг", далее файлы
  int sidx = (ansCursor < 6 ? 0 : ansCursor - 5);
  for (int r = sidx; r < sidx + 6 && r < totalRows; r++) {
    oled.setCursor(6, 2 + (r - sidx));
    if (r == 0) oled.print(F("Мониторинг"));
    else oled.print(ansFiles[r - 1]);
    yield();
  }

  int cp = constrain(ansCursor - sidx, 0, 5) + 2;
  oled.setCursor(0, cp);
  oled.print(">");

  checkBatteryCharge();
  drawBatteryCharge();
  oled.update();
}

/* ============ Меню 3: мониторинг (как главное, всё вручную) ============== */
void ansDrawMonitor(int ansCursor, bool monitoring) {
  oled.clear();
  oled.home();
  oled.line(0, 10, 127, 10);
  oled.print(F("МОНИТОРИНГ"));

  int totalRows = 1 + monLineCount;  // строка 0 — кнопка, далее статус
  int sidx = (ansCursor < 6 ? 0 : ansCursor - 5);
  for (int r = sidx; r < sidx + 6 && r < totalRows; r++) {
    oled.setCursor(6, 2 + (r - sidx));
    if (r == 0) {
      if (monitoring) oled.print(F("Мониторинг: ВКЛ"));
      else oled.print(F("Мониторинг: ВЫКЛ"));
    } else {
      oled.print(monLines[r - 1]);
    }
    yield();
  }

  int cp = constrain(ansCursor - sidx, 0, 5) + 2;
  oled.setCursor(0, cp);
  oled.print(">");

  checkBatteryCharge();
  drawBatteryCharge();
  oled.update();
}

void enterMonitorMenu() {
  ansExitToMain = false;
  bool monitoring = false;
  bool monReady = false;
  int staged = 0;
  String photoStatus = "idle";
  int ansCursor = 0;
  unsigned long enterTime = millis();
  unsigned long lastPoll = 0;
  uiTimer = millis();

  monLineCount = 1;
  monLines[0] = "выключено";
  ansDrawMonitor(ansCursor, monitoring);

  while (true) {
    up.tick();
    ok.tick();
    down.tick();

    if (millis() - enterTime > 800 && up.hold()) {
      break;  // UP-hold — назад в Меню 2
    }
    if (millis() - enterTime > 800 && down.hold()) {
      // DOWN-hold — выход в Меню 1
      WiFi.mode(WIFI_OFF);
      uiTimer = millis();
      checkFileSystem();
      drawMainMenu();
      ansExitToMain = true;
      return;
    }

    int totalRows = 1 + monLineCount;
    if (up.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor - 1, 0, totalRows - 1);
      ansDrawMonitor(ansCursor, monitoring);
    } else if (down.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor + 1, 0, totalRows - 1);
      ansDrawMonitor(ansCursor, monitoring);
    }

    if (ok.click()) {
      uiTimer = millis();
      if (monitoring && monReady) {
        // Ответ готов: OK забирает и сохраняет, мониторинг остаётся ВКЛ
        ansRebuildList();
        int oldNums[ANS_MAX];
        int oldCount = ansCount;
        for (int i = 0; i < ansCount; i++) oldNums[i] = ansNums[i];
        bool got = fetchAndSaveAnswer();
        if (got) {
          ansRebuildList();
          String newFn = "";
          for (int i = 0; i < ansCount; i++) {
            bool found = false;
            for (int j = 0; j < oldCount; j++) {
              if (ansNums[i] == oldNums[j]) { found = true; break; }
            }
            if (!found) { newFn = "/" + ansFiles[i]; break; }
          }
          if (newFn == "") newFn = ansCount > 0 ? "/" + ansFiles[0] : "/answer_N.txt";
          monReady = false;
          monLineCount = 0;
          monLines[monLineCount++] = "сохранено:";
          if (monLineCount < 3) monLines[monLineCount++] = newFn;
        } else {
          monReady = false;
          monLineCount = 1;
          monLines[0] = "кадров на сервере: " + String(staged);
        }
        ansCursor = constrain(ansCursor, 0, monLineCount);
        lastPoll = millis();
        ansDrawMonitor(ansCursor, monitoring);
      } else if (ansCursor == 0) {
        // Кнопка: вкл/выкл мониторинга
        if (!monitoring) {
          if (strlen(sets.backendHost) == 0 || sets.backendPort == 0 || strlen(sets.deviceKey) == 0) {
            monLineCount = 1;
            monLines[0] = "настрой сервер";
            ansDrawMonitor(ansCursor, monitoring);
          } else {
            oled.clear(); oled.home();
            oled.println("Мониторинг:");
            oled.println("подключение...");
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
              WiFi.mode(WIFI_OFF);
              monLineCount = 1;
              monLines[0] = "ошибка wifi";
              ansDrawMonitor(ansCursor, monitoring);
            } else {
              monitoring = true;
              bool answerReady = false;
              if (fetchPhotoStatus(staged, photoStatus, answerReady)) {
                monReady = answerReady;
                monLineCount = 0;
                monLines[monLineCount++] = "кадров на сервере: " + String(staged);
                if (monReady && monLineCount < 3) monLines[monLineCount++] = "ответ получен";
              } else {
                monReady = false;
                monLineCount = 1;
                monLines[0] = "ошибка сервера";
              }
              lastPoll = millis();
              ansCursor = constrain(ansCursor, 0, monLineCount);
              ansDrawMonitor(ansCursor, monitoring);
            }
          }
        } else {
          monitoring = false;
          monReady = false;
          WiFi.mode(WIFI_OFF);
          monLineCount = 1;
          monLines[0] = "выключено";
          ansCursor = constrain(ansCursor, 0, monLineCount);
          ansDrawMonitor(ansCursor, monitoring);
        }
      }
      // OK на строках статуса (ответ не готов): ничего не делаем
    }

    // Опрос раз в секунду, только пока ВКЛ
    if (monitoring && millis() - lastPoll >= 1000) {
      lastPoll = millis();
      bool answerReady = false;
      if (fetchPhotoStatus(staged, photoStatus, answerReady)) {
        monReady = answerReady;
        monLineCount = 0;
        monLines[monLineCount++] = "кадров на сервере: " + String(staged);
        if (monReady && monLineCount < 3) monLines[monLineCount++] = "ответ получен";
      } else {
        monReady = false;
        monLineCount = 1;
        monLines[0] = "ошибка сервера";
      }
      ansCursor = constrain(ansCursor, 0, monLineCount);
      ansDrawMonitor(ansCursor, monitoring);
    }

    delay(20);
  }

  // Любой выход: мониторинг ВЫКЛ + WiFi ВЫКЛ
  WiFi.mode(WIFI_OFF);
  uiTimer = millis();
}

void enterAnswersMenu() {
  WiFi.mode(WIFI_OFF);  // в этом меню WiFi не используется
  ansExitToMain = false;
  ansRebuildList();
  int ansCursor = 0;
  unsigned long enterTime = millis();
  uiTimer = millis();

  ansDrawList(ansCursor);

  while (true) {
    up.tick();
    ok.tick();
    down.tick();

    if (millis() - enterTime > 800 && (up.hold() || down.hold())) {
      break;  // UP-hold / DOWN-hold — выход в Меню 1
    }

    int totalRows = ansCount + 1;
    if (up.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor - 1, 0, totalRows - 1);
      ansDrawList(ansCursor);
    } else if (down.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor + 1, 0, totalRows - 1);
      ansDrawList(ansCursor);
    }

    if (ok.click()) {
      uiTimer = millis();
      if (ansCursor == 0) {
        enterMonitorMenu();  // OK на "Мониторинг" — в Меню 3
        if (ansExitToMain) {
          ansExitToMain = false;
          return;  // Меню 3 уже нарисовало главное меню
        }
        ansRebuildList();  // новый файл виден сразу
        ansCursor = constrain(ansCursor, 0, ansCount);
        uiTimer = millis();
        ansDrawList(ansCursor);
      } else {
        int idx = ansCursor - 1;
        if (idx >= 0 && idx < ansCount) {
          selectedFile = ansFiles[idx];
          enterToReadTxtFile();  // штатный ридер
          ansRebuildList();
          ansCursor = constrain(ansCursor, 0, ansCount);
          uiTimer = millis();
          ansDrawList(ansCursor);
        }
      }
    }

    delay(20);
  }

  // Любой выход: WiFi ВЫКЛ + главное меню
  WiFi.mode(WIFI_OFF);
  uiTimer = millis();
  checkFileSystem();
  drawMainMenu();
}
