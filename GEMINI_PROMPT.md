# Промпт для Gemini (сессия на железе, звук разрешён)

Вставь текст ниже в Gemini как задание. Подробности: `GEMINI.md` (блок «▶ СЛЕДУЮЩИЙ ЗАПУСК», §6, §7) и `docs/REVIEW_20261028.md`.

```text
Ты работаешь в репозитории imacorpoliz/t2audiowindows на Windows-машине с WDK и на тестовом MacBookPro16,1. Ты — билдер и тестировщик драйвера T2AudioPort.

РАЗРЕШЕНИЯ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ (это и есть разрешение из GEMINI.md §1, отдельно спрашивать не нужно):
- «Разрешаю START_IO»: -BceIo -OwnerApprovedBceIo для шагов ниже.
- «Разрешаю звук»: -AudibleVolume -OwnerApprovedAudible, тон, раскладка каналов (-RawChannels только с тоном >= 1 кГц на одном канале), 10-минутные прогоны часов.
Лимиты остаются: громкость точки T2 не выше 0.05, амплитуда тона не выше 0.1, Parameters\OutputAttenuation не ниже 3 (−18 дБ). Обычная музыка — только после PASS тона, на громкости Windows 1–5 %.

Перед началом прочитай: GEMINI.md (блок «▶ СЛЕДУЮЩИЙ ЗАПУСК», §1, §6, §7), docs/REVIEW_20261028.md, последние записи docs/CURRENT_STATE.md.

Шаги (каждый следующий — только если предыдущий PASS):
1. git fetch; ветка gemini/stage-e от origin/diagnostics. Проверь, что CI «Build» на этом коммите зелёный.
2. Сборка Release x64, подпись тестовым сертификатом, DriverVer = сегодня, 1.0.8.0. Запиши SHA256 .sys в $h. Файлы .sys и .cat не коммить.
3. Установка: отключи устройство PCI\VEN_106B&DEV_1803, затем pnputil /add-driver packaging\T2AudioMiniport.inf /install.
4. Stage 1:
   .\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash $h
   PASS: в wasapi.txt «endpoint mute=True volume=0», в логе «OutputAttenuation=3 (-18 dB) ClockRateTracking=0».
5. Регрессия этапа D (беззвучно):
   .\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -ExpectedSysHash $h
   .\tools\T2Audio-TestAgent.ps1 -Mode Recover
   PASS: как в docs/logs/EXCERPT_20261028_043442.md — «START_IO reply … msg=1 status=0x00000000», «SetState RUN StartIo status=0x00000000», шаг sample 16640, re-aimed 1–2 раза в начале, «STOP_IO reply … msg=3», после стопа меток нет.
6. Этап E — первый звук:
   .\tools\T2Audio-TestAgent.ps1 -Mode Test -Mmio -BceIo -OwnerApprovedBceIo -AudibleVolume 0.03 -OwnerApprovedAudible -ExpectedSysHash $h
   .\tools\T2Audio-TestAgent.ps1 -Mode Recover
   В tone.txt должно быть «endpoint mute=False volume=0.03», «Start hr=0x00000000», «after run: endpoint mute=True volume=0».
   Спроси владельца, что он услышал: есть ли тон, с обеих ли сторон, из вуферов и/или твитеров, есть ли щелчки примерно раз в 0,35 с. Запиши ответ.
   Тона нет, а лог как на шаге 5? Повтори с -ToneFrequency 1000 и затем с Parameters\OutputAttenuation=1. Громкость точки не повышай. Если и так тихо — стоп, отчёт: T2 не читает кольцо.
7. Раскладка (GEMINI.md §6.2): цикл по каналам 0..5 с -RawChannels -ToneChannel $c -ToneFrequency 1000 -AudibleVolume 0.03 -OwnerApprovedAudible, после каждого -Mode Recover. Спрашивай владельца, какой динамик звучал. Ожидаемо: 0/1 — левые вуферы, 2 — левый твитер, 3/4 — правые вуферы, 5 — правый твитер. Если не совпало, не правь код, запиши в отчёт.
8. Часы (GEMINI.md §7 E2/E3): два прогона по 10 минут тихой музыки или тона (громкость Windows 1–5 %):
   a) Parameters\ClockRateTracking=0 — посчитай «re-aimed», отметь щелчки на слух, вычисли дрейф T2 по меткам (шаг sample 16640 против интервала в мс);
   b) Parameters\ClockRateTracking=1 (dword, перезапуск устройства) — в логе «T2 clock: rate=… ppb drift=… ppb lead=…»: drift стабилен, lead 1000 ± 100, re-aimed не больше 1–2 раз в начале. Если rate упирается в ±1000000 или lead уходит — верни 0 и приложи лог.
   Для длинного прогона увеличь -CaptureSeconds (например 660) и проиграй звук своим тоном или плеером.
9. Отчёт: docs/logs/EXCERPT_<дата>_<время>.md по каждому шагу; датированная запись в docs/CURRENT_STATE.md (хэш, версия, PASS/FAIL каждого шага, что услышал владелец, раскладка, дрейф в ppm, результат ClockRateTracking); отметки в GEMINI.md §7; PR из своей ветки в diagnostics. В конце устройство снова на AppleAudio (Started, CM_PROB_NONE), звуковая схема Windows возвращена, Parameters\ClockRateTracking и OutputAttenuation возвращены к значениям по умолчанию (удалить значения).

Правила:
- На любом шаге без PASS: стоп, .\tools\T2Audio-TestAgent.ps1 -Mode Recover, сохрани логи (docs/logs/capture_agent_*, минидамп), опиши в CURRENT_STATE.md, что ожидалось и что получилось, сообщи владельцу. Код драйвера не чини наугад.
- Неприятный звук, треск, перегруз — сразу -Mode Recover и Mute. Твитеры режут слух — Parameters\TweeterMode=0.
- BSOD — стоп, минидамп, отчёт, без разбора не повторять.
- Не убирай гейт TestSession, не включай автозапуск, не усыпляй Mac во время прогона.
- В отчётах различай «проверено на железе (хэш …)», «проверено тестами» и «гипотеза».
- В diagnostics напрямую не пушь, только PR.
```
