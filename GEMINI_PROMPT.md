# Промпт для Gemini (доводка звука на 1.0.12.0; звук разрешён)

Вставь текст ниже в Gemini как задание. Подробности: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в `GEMINI.md` и первая запись в `docs/CURRENT_STATE.md`.

```text
Ты работаешь в репозитории imacorpoliz/t2audiowindows на Windows-машине с WDK и на тестовом MacBookPro16,1. Ты — билдер и тестировщик драйвера T2AudioPort.

РАЗРЕШЕНИЯ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ: «разрешаю START_IO» и «разрешаю звук» (-BceIo -OwnerApprovedBceIo, -Listen -OwnerApprovedAudible). Отдельно спрашивать не нужно.

Прочитай: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в GEMINI.md (шаги 1–3 по порядку), §1 «Жёсткие правила», первую запись в docs/CURRENT_STATE.md. SpeakerDsp=3 одобрен владельцем и теперь стоит по умолчанию; в run summary новое поле maxCopyUs.

Цель: сборка 1.0.12.0 и доводка с владельцем по таблице вариантов a–f:
- гул на басовом треке при 100 % громкости (по умолчанию → SpeakerDsp=4 → WooferLimitDb 3/6);
- яркость (TweeterTrimDb 3/6);
- громкость (DspGainDb 9);
- 10 минут лучшего варианта с ClockRateTracking=1.

Отчёт: docs/logs/EXCERPT_*.md (таблица вариантов, слова владельца, все run summary с maxCopyUs, dspLimited, wooferLimited), запись в docs/CURRENT_STATE.md с итоговыми лучшими значениями, PR в diagnostics. «Доказано» — только проверенное, остальное «гипотеза»; оценку стоимости DSP бери из maxCopyUs драйвера, а не из хост-теста.

Правила:
- Без PASS — -Mode Recover, логи, запись в CURRENT_STATE.md, сообщить владельцу. Драйвер наугад не чинить.
- Неприятный звук — Mute, Enter, -Mode Recover.
- BSOD — стоп, минидамп, отчёт.
- Не убирать гейт TestSession, не включать автозапуск, не усыплять Mac с работающим драйвером.
- В конце: AppleAudio (Started, CM_PROB_NONE), все значения в Parameters удалены.
- В diagnostics напрямую не пушить, только PR.
```
