# Промпт для Gemini (подбор звука: компрессор, громкость, яркость; звук разрешён)

Вставь текст ниже в Gemini как задание. Подробности: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в `GEMINI.md` и первая запись в `docs/CURRENT_STATE.md`.

```text
Ты работаешь в репозитории imacorpoliz/t2audiowindows на Windows-машине с WDK и на тестовом MacBookPro16,1. Ты — билдер и тестировщик драйвера T2AudioPort.

РАЗРЕШЕНИЯ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ: «разрешаю START_IO» и «разрешаю звук» (-BceIo -OwnerApprovedBceIo, -Listen -OwnerApprovedAudible). Отдельно спрашивать не нужно.

Прочитай: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в GEMINI.md (выполняй шаги 1–3 по порядку), §1 «Жёсткие правила», первую запись в docs/CURRENT_STATE.md (разбор прослушивания №2: бас появился; «тускло/низкая частота дискретизации» — скорее баланс твитеров, добавлен Parameters\TweeterTrimDb).

Цель: собрать 1.0.11.0 и вместе с владельцем подобрать лучшие значения SpeakerDsp (3 или 4), DspGainDb (6/9/12) и TweeterTrimDb (0/3/6) по таблице вариантов a–g; лучший вариант прослушать 10 минут с ClockRateTracking=1.

Отчёт: docs/logs/EXCERPT_*.md (таблица вариантов: слова владельца, mode parameters, все run summary с dspLimited=), запись в docs/CURRENT_STATE.md с лучшими значениями, PR в diagnostics. Выводы о коде не делай; «доказано» — только проверенное, остальное «гипотеза».

Правила:
- Без PASS — -Mode Recover, логи, запись в CURRENT_STATE.md, сообщить владельцу. Драйвер наугад не чинить.
- Неприятный звук — Mute, Enter, -Mode Recover.
- BSOD — стоп, минидамп, отчёт.
- Не убирать гейт TestSession, не включать автозапуск, не усыплять Mac.
- В конце: AppleAudio (Started, CM_PROB_NONE), все значения в Parameters (OutputAttenuation, ClockRateTracking, SpeakerDsp, DspGainDb, TweeterTrimDb, TweeterMode) удалены.
- В diagnostics напрямую не пушить, только PR.
```
