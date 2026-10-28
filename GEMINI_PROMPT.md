# Промпт для Gemini (сессия прослушивания, звук разрешён)

Вставь текст ниже в Gemini как задание. Подробности: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в `GEMINI.md` и первая запись в `docs/CURRENT_STATE.md`.

```text
Ты работаешь в репозитории imacorpoliz/t2audiowindows на Windows-машине с WDK и на тестовом MacBookPro16,1. Ты — билдер и тестировщик драйвера T2AudioPort.

РАЗРЕШЕНИЯ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ: «разрешаю START_IO» и «разрешаю звук» (-BceIo -OwnerApprovedBceIo, -Listen -OwnerApprovedAudible). Отдельно спрашивать не нужно.

Прочитай: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в GEMINI.md, §1 «Жёсткие правила», первую запись в docs/CURRENT_STATE.md (разбор этапа E от Claude).

Цель: новая сборка 1.0.9.0 (в ней исправлен щелчок в начале каждого потока и добавлены счётчики задержек), владелец слушает сам, ты собираешь логи. Затем сборка с DSP kaiT2en в драйвере (шаг 6b).

Шаги (каждый следующий — только если предыдущий PASS):
1. git fetch; ветка gemini/listen-1 от origin/diagnostics; CI «Build» на коммите зелёный.
2. Сборка Release x64, подпись, DriverVer = сегодня, 1.0.9.0, SHA256 .sys в $h. .sys и .cat не коммить.
3. Установка: отключи PCI\VEN_106B&DEV_1803, затем pnputil /add-driver packaging\T2AudioMiniport.inf /install. В HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\Parameters удали OutputAttenuation и ClockRateTracking, если они там есть.
4. .\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash $h
   PASS: в t2audio_messages.txt «OutputAttenuation=1 (-6 dB) ClockRateTracking=0».
5. Прослушивание №1:
   .\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -Listen -OwnerApprovedAudible -ExpectedSysHash $h
   Агент ждёт Enter. Скажи владельцу: выбрать динамики T2 устройством вывода, снять Mute на этой точке (пробы её глушат), поднимать громкость с 5–10 %, слушать что угодно сколько угодно (музыка, видео, паузы, повторные старты) и отмечать время хрипов. По его команде нажми Enter. Потом .\tools\T2Audio-TestAgent.ps1 -Mode Recover.
6. Прослушивание №2: то же самое с Parameters\ClockRateTracking=1 (dword). В логе должна быть строка «T2 clock: rate=… ppb drift=… ppb lead=…» (drift около +10000 ppb, lead около 1000). Потом удали ClockRateTracking.
6b. Прослушивание №3 — DSP kaiT2en в драйвере. Если сборка из шага 2 сделана до коммита с DSP (src/T2AudioDsp.h отсутствует), то git pull, пересобери как 1.0.10.0 и переустанови. Поставь Parameters\SpeakerDsp=1 (dword); ClockRateTracking — как лучше было в шаге 6, иначе 0. Прослушивание той же командой, что в шаге 5. В логе «SpeakerDsp=1». Попроси владельца сравнить с №1 на тех же треках и той же громкости: бас ниже ~70 Гц срезан специально (защита вуферов), должно стать ровнее, шире и меньше хрипа на басу. Затем то же с SpeakerDsp=2 (добавлен виртуальный бас). Попроси владельца сравнить №1, SpeakerDsp=1 и SpeakerDsp=2. Выпиши все «run summary» (с dspLimited=). Потом удали SpeakerDsp.
7. Отчёт: docs/logs/EXCERPT_<дата>_<время>.md для всех прослушиваний. В нём строка mode parameters, ВСЕ строки «run summary: …», ВСЕ строки «re-aimed», первые строки «rate=», слова владельца и время хрипов. Датированная запись в docs/CURRENT_STATE.md (хэш, версия, результат по шагам). PR в diagnostics. В конце устройство на AppleAudio (Started, CM_PROB_NONE).

Что важно увидеть в логе (для Claude):
- в начале каждого потока теперь ОДИН «re-aimed», а не два (-17xx и +6xx, как раньше);
- в «run summary»: gaps>lead и skips. Если они не 0, хрип от задержек DPC; maxCopyGapUs покажет, насколько.

Правила:
- На любом шаге без PASS: -Mode Recover, сохрани логи (docs/logs/capture_agent_*, минидамп), опиши в CURRENT_STATE.md, что ожидалось и что получилось, сообщи владельцу. Драйвер наугад не чини.
- Неприятный звук — владелец ставит Mute, ты жмёшь Enter, затем -Mode Recover. Твитеры режут слух — Parameters\TweeterMode=0.
- BSOD — стоп, минидамп, отчёт.
- Не убирай гейт TestSession, не включай автозапуск, не усыпляй Mac во время прослушивания.
- В отчётах различай «проверено на железе (хэш …)», «проверено тестами» и «гипотеза». В diagnostics напрямую не пушь, только PR.
```
