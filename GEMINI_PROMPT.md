# Промпт для Gemini (раскладка каналов и сравнение DSP, звук разрешён)

Вставь текст ниже в Gemini как задание. Подробности: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в `GEMINI.md` и запись «analysis of Listening Session #1» в `docs/CURRENT_STATE.md`.

```text
Ты работаешь в репозитории imacorpoliz/t2audiowindows на Windows-машине с WDK и на тестовом MacBookPro16,1. Ты — билдер и тестировщик драйвера T2AudioPort.

РАЗРЕШЕНИЯ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ: «разрешаю START_IO» и «разрешаю звук» (-BceIo -OwnerApprovedBceIo, -AudibleVolume/-Listen/-SpeakerCheck -OwnerApprovedAudible, -RawChannels для проверки динамиков). Отдельно спрашивать не нужно.

Прочитай: блок «▶ СЛЕДУЮЩИЙ ЗАПУСК» в GEMINI.md (выполняй его шаги 1–6 строго по порядку), §1 «Жёсткие правила», первую запись в docs/CURRENT_STATE.md (разбор прослушивания №1 от Claude: «баса нет» скорее из-за баланса простого пути, а не мёртвых вуферов; гипотезы A/B/C разобраны).

Цель сессии:
1) сборка 1.0.10.0 с актуальной diagnostics (в ней DSP kaiT2en: Parameters\SpeakerDsp = 1, 2, 3);
2) проверка динамиков по шагам (шаг 3, агент с -SpeakerCheck): 3a — каждый из 6 динамиков отдельно (-RawChannels): вуферы 100 Гц / 1 кГц / свип 50–500 Гц, твитеры 2 кГц / 8 кГц / свип 1–16 кГц; 3b — стерео-точка (левая и правая сторона: 100 Гц, 1 кГц, 8 кГц, свип) с SpeakerDsp=0 и SpeakerDsp=3. Владелец сам слушает и отвечает, агент пишет ответы в speakercheck.txt. Главный вопрос: работают ли НЧ-динамики (вуферы);
3) владелец сравнивает на слух SpeakerDsp = 0, 1, 2, 3 в режиме -Listen на одних и тех же треках и одной громкости и говорит, какой лучше;
4) по возможности 10 минут лучшего уровня с ClockRateTracking=1.

В отчёт (docs/logs/EXCERPT_*.md + запись в docs/CURRENT_STATE.md + PR в diagnostics):
- speakercheck.txt из 3a и 3b целиком и таблица: канал, частота, что слышно и откуда;
- для каждого SpeakerDsp: оценка владельца (бас, яркость, громкость, хрип) и строки лога «mode parameters», все «run summary» (с dspLimited=), «re-aimed»;
- хэш, версия, PASS/FAIL по шагам.

Правила:
- На любом шаге без PASS: -Mode Recover, сохрани логи (docs/logs/capture_agent_*, минидамп), опиши в CURRENT_STATE.md, что ожидалось и что получилось, сообщи владельцу. Код драйвера наугад не чини: выводы и правки делает Claude.
- Неприятный звук — Mute, Enter (в -Listen), -Mode Recover. Твитеры режут слух — Parameters\TweeterMode=0 (только для SpeakerDsp=0).
- BSOD — стоп, минидамп, отчёт.
- Не убирай гейт TestSession, не включай автозапуск, не усыпляй Mac во время прогона.
- В конце: устройство на AppleAudio (Started, CM_PROB_NONE), значения OutputAttenuation/ClockRateTracking/SpeakerDsp в Parameters удалены.
- В отчётах различай «проверено на железе (хэш …)», «проверено тестами» и «гипотеза». В diagnostics напрямую не пушь, только PR.
```
