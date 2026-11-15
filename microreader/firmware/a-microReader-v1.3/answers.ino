// microReader v1.3: Меню 2 (ответы) + Меню 3 (мониторинг).
// Вход из главного меню — удержание ВНИЗ. WiFi используется только в Меню 3,
// включается вручную кнопкой и гасится на любом выходе.
#define ANS_MAX 40

String ansFiles[ANS_MAX];   // имена answer_*.txt без ведущего "/" (старые -> новые)
int ansNums[ANS_MAX];       // их числовые суффиксы
int ansCount = 0;
bool ansExitToMain = false;  // Меню 3 просит Меню 2 сразу выйти в главное

// Живое состояние Меню 3: фиксированные 6 строк, без прокрутки.
int monStaged = 0;              // строка 2: "Кадров: N"
bool monAnswerReady = false;    // строка 4: "Ответ получен"
bool monVoiceBusy = false;      // строка 4: "Голос: думаю..."
int monLessonChunks = 0;        // строка 3: "Пишу: N" / "Записано: N"
String monCamIp = "";           // строка 5: "CAM <ip>"
bool monVoiceRecording = false; // строка 3: идёт запись
String monNotice = "";          // строка 4: ошибка/подсказка вместо статуса
String voiceMode = "qa";     // режим сервера: "qa" (вопрос-ответ) | "lesson" (урок)
bool voiceModeKnown = false;  // true только после подтверждения сервером (GET/POST)

// Список /answer_*.txt, сортировка по номеру: СТАРЫЕ -> НОВЫЕ (новейший последний).
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
      if (ansCount >= ANS_MAX) {
        // Массив полон: держим 40 НОВЕЙШИХ — вытесняем самый старый (первый),
        // только если пришедший новее. Иначе файл пропускаем.
        if (num <= ansNums[0]) continue;
        for (int i = 1; i < ansCount; i++) {  // первый (самый старый) выбывает
          ansNums[i - 1] = ansNums[i];
          ansFiles[i - 1] = ansFiles[i];
        }
        ansCount--;
      }
      int pos = ansCount;
      while (pos > 0 && ansNums[pos - 1] > num) {  // по возрастанию: новые в конец
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

/* ============ Меню 3: мониторинг, фиксированные 6 строк ================ */
void ansDrawMonitor(int ansCursor, bool monitoring) {
  oled.clear();
  oled.home();
  oled.line(0, 10, 127, 10);
  oled.print(F("МОНИТОРИНГ"));

  for (int r = 0; r < 6; r++) {  // ровно 6 строк — прокрутка не нужна
    oled.setCursor(6, 2 + r);
    if (r == 0) {
      if (monitoring) oled.print(F("Мониторинг: ВКЛ"));
      else oled.print(F("Мониторинг: ВЫКЛ"));
    } else if (r == 1) {
      if (!voiceModeKnown) oled.print(F("Режим: ...?"));
      else if (voiceMode == "lesson") oled.print(F("Режим: Урок"));
      else oled.print(F("Режим: Вопросы"));
    } else if (r == 2) {
      oled.print(F("Кадров: "));
      oled.print(monStaged);
    } else if (r == 3) {
      bool lesson = (voiceMode == "lesson");
      if (monVoiceRecording) {
        if (lesson) {
          oled.print(F("Пишу: "));
          oled.print(monLessonChunks);
        } else oled.print(F("Идёт запись"));
      } else {
        if (lesson) {
          oled.print(F("Записано: "));
          oled.print(monLessonChunks);
        } else oled.print(F("Запись: нет"));
      }
    } else if (r == 4) {
      if (monNotice.length()) oled.print(monNotice);
      else if (monAnswerReady) oled.print(F("Ответ получен"));
      else if (monVoiceBusy) oled.print(F("Голос: думаю..."));
      else oled.print(F("-"));
    } else {
      if (monCamIp.length()) {
        oled.print(F("CAM "));
        oled.print(monCamIp);
      } else oled.print(F("CAM: -"));
    }
    yield();
  }

  int cp = constrain(ansCursor, 0, 5) + 2;
  oled.setCursor(0, cp);
  oled.print(">");

  checkBatteryCharge();
  drawBatteryCharge();
  oled.update();
}

// Опрос сервера для Меню 3: GET /api/photo-status + GET /api/voice-mode.
// true — статус получен (строки 2..5 свежие), false — ошибка сервера.
bool ansPollMonitor() {
  String photoStatus = "idle";
  if (!fetchPhotoStatus(monStaged, photoStatus, monAnswerReady, monVoiceBusy, monLessonChunks, monCamIp, monVoiceRecording)) {
    monAnswerReady = false;
    monVoiceBusy = false;
    return false;
  }
  // Режим спрашиваем не каждый опрос: пока не подтверждён — сразу, дальше
  // каждый 5-й (~10 с при периоде 2 с), чтобы поймать рестарт сервера.
  static uint8_t modePollTick = 0;
  bool pollMode = !voiceModeKnown;
  if (++modePollTick >= 5) {  // uint8_t не переполнится: сброс на каждом 5-м
    modePollTick = 0;
    pollMode = true;
  }
  if (pollMode) {
    if (fetchVoiceMode(voiceMode)) voiceModeKnown = true;  // при ошибке — прежний режим и прежний флаг
  }
  return true;
}

void enterMonitorMenu() {
  ansExitToMain = false;
  bool monitoring = false;
  int ansCursor = 0;
  unsigned long enterTime = millis();
  unsigned long lastPoll = 0;
  uiTimer = millis();

  voiceModeKnown = false;  // вход в меню: режим подтверждается только свежим опросом
  monStaged = 0;
  monAnswerReady = false;
  monVoiceBusy = false;
  monLessonChunks = 0;
  monCamIp = "";
  monVoiceRecording = false;
  monNotice = "";
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

    if (up.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor - 1, 0, 5);  // курсор только по 6 строкам
      ansDrawMonitor(ansCursor, monitoring);
    } else if (down.click()) {
      uiTimer = millis();
      ansCursor = constrain(ansCursor + 1, 0, 5);
      ansDrawMonitor(ansCursor, monitoring);
    }

    if (ok.click()) {
      uiTimer = millis();
      if (ansCursor == 1) {
        // Строка режима: OK переключает режим сервера (qa <-> lesson)
        if (!monitoring) {
          monNotice = "включи мониторинг";
          ansDrawMonitor(ansCursor, monitoring);
        } else if (!voiceModeKnown) {
          // Режим не подтверждён сервером: переключать нечего, ждём удачный опрос
          monNotice = "нет данных о режиме";
          ansDrawMonitor(ansCursor, monitoring);
        } else {
          String want = (voiceMode == "lesson") ? "qa" : "lesson";
          oled.clear(); oled.home();
          oled.println("Режим:");
          oled.print(want == "lesson" ? "Урок" : "Вопрос-ответ");
          oled.update();
          String confirmed = "";
          if (postVoiceMode(want, confirmed)) {
            voiceMode = confirmed;  // доверяем только ответу сервера
            voiceModeKnown = true;
            monNotice = ansPollMonitor() ? "" : "Ошибка сервера";
          } else {
            monNotice = "ошибка смены режима";
          }
          lastPoll = millis();
          ansDrawMonitor(ansCursor, monitoring);
        }
      } else if (ansCursor == 0) {
        // Строка тумблера: вкл/выкл мониторинга
        if (!monitoring) {
          if (strlen(sets.backendHost) == 0 || sets.backendPort == 0 || strlen(sets.deviceKey) == 0) {
            monNotice = "настрой сервер";
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
              monNotice = "ошибка wifi";
              ansDrawMonitor(ansCursor, monitoring);
            } else {
              monitoring = true;
              monNotice = ansPollMonitor() ? "" : "Ошибка сервера";
              lastPoll = millis();
              ansDrawMonitor(ansCursor, monitoring);
            }
          }
        } else {
          monitoring = false;
          voiceModeKnown = false;  // знание устарело: сервер мог перезапуститься
          WiFi.mode(WIFI_OFF);
          monStaged = 0;
          monAnswerReady = false;
          monVoiceBusy = false;
          monLessonChunks = 0;
          monCamIp = "";
          monVoiceRecording = false;
          monNotice = "";
          ansDrawMonitor(ansCursor, monitoring);
        }
      }
      // OK на строках 2..5: ничего не делаем
    }

    // Опрос раз в 2 с, только пока ВКЛ. Ответ готов — сразу забираем тихо
    // и выходим: файл уже сохранён, Меню 2 покажет его в конце списка.
    if (monitoring && millis() - lastPoll >= 2000) {
      lastPoll = millis();
      if (!ansPollMonitor()) {
        monNotice = "Ошибка сервера";
      } else {
        monNotice = "";
        if (monAnswerReady) {
          String savedName = "";
          if (fetchLatestAnswerQuiet(savedName)) {
            break;  // общий хвост гасит WiFi, Меню 2 перестроит список
          }
          monNotice = "Ошибка сервера";  // не забрали — мониторинг остаётся ВКЛ
        }
      }
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
