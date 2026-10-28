# Промпт для Gemini (кто вуфер + сравнение DSP, звук разрешён)

Вставь текст ниже в Gemini как задание. Подробности: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в `GEMINI.md` и запись «analysis of the 18:14 speaker check» в `docs/CURRENT_STATE.md`.

```text
Ты работаешь в репозитории imacorpoliz/t2audiowindows на Windows-машине с WDK и на тестовом MacBookPro16,1. Ты — билдер и тестировщик драйвера T2AudioPort.

РАЗРЕШЕНИЯ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ: «разрешаю START_IO» и «разрешаю звук» (-BceIo -OwnerApprovedBceIo, -Listen/-SpeakerCheck/-AudibleVolume -OwnerApprovedAudible, -RawChannels, OutputAttenuation=0 на время шага 2). Отдельно спрашивать не нужно.

Прочитай блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в GEMINI.md и выполняй его шаги 1–5 по порядку; §1 «Жёсткие правила»; первую запись в docs/CURRENT_STATE.md. Там разбор прошлой проверки: каналы 0/1/3/4 подключены, но какие динамики вуферы — ещё не доказано; сравнение DSP не сделано.

Цели, по важности:
1) Шаг 3 — сравнение на слух SpeakerDsp = 0, 1, 2, 3, 4 в режиме -Listen (одни треки, одна громкость Windows 30–50 %). Владелец говорит про каждый уровень: есть ли бас, яркость, громкость, искажения, какой лучше. Это главное.
2) Шаг 2 — -SpeakerCheck -RawChannels -AudibleVolume 0.25 с OutputAttenuation=0: на свипах владелец говорит, с какой частоты стало слышно (вуфер ~100–150 Гц, твитер — только выше ~1 кГц).
3) Шаг 4 — по возможности ClockRateTracking=1 на 10 минут.

Отчёт: docs/logs/EXCERPT_*.md (speakercheck.txt целиком, оценки по уровням DSP, строки лога mode parameters, все run summary, re-aimed), запись в docs/CURRENT_STATE.md, PR в diagnostics. Выводы о коде не делай: «доказано» пиши только про то, что реально проверено, остальное помечай «гипотеза».

Правила:
- Без PASS — -Mode Recover, логи, запись в CURRENT_STATE.md, сообщить владельцу. Драйвер наугад не чинить.
- Неприятный звук — Mute, Enter, -Mode Recover.
- BSOD — стоп, минидамп, отчёт.
- Не убирать гейт TestSession, не включать автозапуск, не усыплять Mac.
- В конце: AppleAudio (Started, CM_PROB_NONE), значения OutputAttenuation/ClockRateTracking/SpeakerDsp/DspGainDb/TweeterMode в Parameters удалены.
- В diagnostics напрямую не пушить, только PR.
```
