# План реверс-инжиниринга (для Gemini)

> Задача — закрыть оставшиеся вопросы о том, как `AppleAudio.sys` и `AppleUSBVHCI.sys` работают с T2. От ответов зависят звук через `START_IO`, аппаратные часы, jack и сон.
> Сначала прочитай [`analysis/START_IO_BSOD_ROOT_CAUSE.md`](analysis/START_IO_BSOD_ROOT_CAUSE.md): там найденная причина BSOD и адреса, от которых нужно отталкиваться.

## 0. Исходные данные

| Что | Где |
|---|---|
| `AppleUSBVHCI.sys` 6.1.7500.5 | приватный репозиторий `imacorpoliz/mbp`: `AppleUSBVHCI-extracted/` (SHA256 `0438292090fc…55c1`) |
| `AppleAudio.sys` + `appleaudio.inf` | `imacorpoliz/mbp`: `MBP-Audio-driver/` (SHA256 `cd8b81fabc9b…a150`) |
| Прежние дизассемблерные выборки | `imacorpoliz/mbp`: `evidence/traces/` (36 файлов) |
| Прежние отчёты | этот репозиторий: `docs/analysis/*.md` |
| Минидампы BSOD | на тестовой машине: `C:\Windows\Minidump\100826-15859-01.dmp`, `100826-23546-01.dmp` |
| Linux-эталон | [kaiT2en/KaiT2en-Fedora](https://github.com/kaiT2en/KaiT2en-Fedora) `modules/t2bce_audio` (`protocol_bce.c`, `audio.c`, `pcm.c`, `timing.h`) |

**Основной инструмент — REA (Reverse Engineer Anything)**, тот же фреймворк, которым сделаны прежние отчёты (`analysis/AppleAudio_Reverse_Engineering_Report.md`). В связке с ним — Ghidra 12.x. Каждую задачу R1–R9 веди через REA: загрузи нужный бинарник, перейди к указанным адресам, получи декомпиляцию, граф вызовов и перекрёстные ссылки. Проекты и выгрузки REA/Ghidra сохраняй только в приватный `imacorpoliz/mbp` (например, `rea/<задача>/`). Вспомогательные инструменты: WinDbg с символами Microsoft (`.symfix; .reload`) — только для минидампов (R1); `objdump -d -M intel` — для быстрой сверки адресов. Адреса ниже — VA при ImageBase `0x140000000` (RVA = VA − `0x140000000`).

## 1. Правила

0. **Никакого воспроизведения звука** (дома спят). Реверс — офлайн-работа с бинарниками и дампами, ей звук не нужен. Не запускай тоны, плееры, `tools/Test-WasapiTone.ps1` и проверки динамиков. Если для задачи нужен прогон на железе — только беззвучный (см. `GEMINI.md`, правило 0).

1. **Бинарники Apple, декомпилированные листинги целиком и проекты Ghidra — только в приватный `imacorpoliz/mbp`.** Публичный `t2audiowindows` получает только отчёты: адреса, короткие (≤ 15 строк) выдержки инструкций, выводы.
2. Каждый вывод помечай: **доказано** (инструкции/дамп), **вероятно** (косвенно) или **гипотеза**. Указывай адреса.
3. Отчёт по каждой задаче — `docs/analysis/RE_<тема>.md`. Сводку изменений внеси в `analysis/START_IO_BSOD_ROOT_CAUSE.md` §5 (открытые вопросы) и в `CURRENT_STATE.md`.
4. Код драйвера по итогам реверса меняй отдельным PR: чистый разборщик в `src/BceProtocolLogic.h` + тест, затем код ядра. Ничего не включай на железе без этапов из `GEMINI.md`.

---

## 2. Задачи (по приоритету)

> **Статус (2026-10-28):** R1–R3 — PR imacorpoliz/t2audiowindows#1 (Gemini, с замечаниями ревью). R4, R5, R9 и часть R7 (формат сэмплов) — [`analysis/RE_appleaudio_protocol.md`](analysis/RE_appleaudio_protocol.md). Осталось: R6 (часы), R7 (раскладка стерео → 6 каналов, коэффициенты фильтров), R8 (питание в AppleAudio), msg 28 и свойства `'pfta'`/`'ring'`/`'sact'`/`'term'`/`'lcdr'`/`'lcdv'`.

### R1. Подтвердить причину BSOD по минидампам — **первым делом**

Гипотеза: AppleUSBVHCI доставляет сообщение bridgeOS через `IOCTL 0x222020` в наш стек, а KS разыменовывает `NULL` FileObject.

```
windbg -z C:\Windows\Minidump\100826-23546-01.dmp
.symfix; .reload
!analyze -v
kv                                  ; найти кадр AppleUSBVHCI+0x259a
.frame /r <номер кадра ks!DispatchDeviceIoControl>
; IRP — второй аргумент (rdx) у ks!DispatchDeviceIoControl
!irp <адрес IRP>                    ; ожидается: IRP_MJ_DEVICE_CONTROL, IoControlCode 0x222020, FileObject 0
!devstack <DeviceObject из IRP>     ; ожидается: ksthunk над T2AudioMiniport над PCI
db <SystemBuffer> L40               ; ожидается: msg (смещение 0x0D) == 4 (UPDATE_TIMESTAMP)
```

В минидампе могут отсутствовать страницы с IRP или буфером. Запиши, что удалось увидеть, а что нет.

**Готово, когда:** в `RE_minidump.md` указаны IoControlCode, `FileObject`, стек устройств и (если доступен) `msg` из буфера, либо явно отмечено, что в дампе этого нет.

### R2. Семантика поля `tag` (первые 4 байта сообщения)

Мы отправляем `"Audt"`, AppleAudio — счётчик 1..127 (`FUN_140004a4c`: `*tag = *tag % 0x7F + 1`), kaiT2en — `'S'+3 цифры`.

1. В AppleUSBVHCI найди обработчик `IOCTL 0x222018`: поиск по `cmp …, 0x222018` / `0x222018` и таблице диспетчеризации из `AppleUSBVHCI_IOCTL_COMPLETE_ANALYSIS.md`.
2. Проследи, как используются первые 4 байта входного буфера: индекс в таблицу ожидающих запросов? ключ сопоставления ответа? Есть ли ограничение 1..127 (≤ `0x7F`)?
3. Найди **всех вызывающих** функцию доставки `0x1400023F0` (`IoGetDeviceInterfaces` → `0x222020`). При каких условиях сообщение уходит в неё: только уведомления/команды от bridgeOS (type 1/3) или ещё и ответы, не сопоставленные с ожидающим запросом?
4. Что AppleUSBVHCI возвращает в выходной буфер `0x222018` и каким `Information`?

**Результат:** надо ли нам слать 1..127 вместо `"Audt"` и чем грозит текущее значение. Это определяет изменение `T2AudioSendBceMessage` и сборку заголовка.

### R3. Нужен ли ответ на `UPDATE_TIMESTAMP`

kaiT2en отвечает `UPDATE_TIMESTAMP_RESPONSE` (msg 34) с тем же тегом. AppleAudio (`0x14000435C`) только сохраняет timestamp и ничего не отправляет.

1. В AppleUSBVHCI рядом с вызовом `0x1400023F0` и после него: формирует ли он сам ответ (константа `0x22` = 34 в поле msg по смещению 13)?
2. В AppleAudio: есть ли где-то запись msg = 34 / вызов `FUN_140004a4c` с ним?
3. Если ответ не отправляет никто — запиши это. Тогда на железе проверь, продолжают ли приходить отметки (счётчик `VhciTimestampCount` в логе).

### R4. Формат `PROPERTY_CHANGED` (msg 13) и jack

AppleAudio `0x1400012CC`–`0x140001332`: сравнение `*(u32*)(buf+0x21)` с `'outp'`/`'inpt'`, флаги `[g+0x38]`/`[g+0x39]`, вызов `[0x14000E250]` (определи импорт — вероятно `KeSetEvent`).

1. Сопоставь смещения с `t2audio_msg_read_property_changed` в kaiT2en (devid, obj, scope/selector/element).
2. Что сигналится и кто ждёт событие (наушники → переключение endpoint)?

**Результат:** раскладка сообщения для `src/BceProtocolLogic.h` и описание логики jack.

### R5. Последовательность инициализации AppleAudio до первого `START_IO`

1. Перечисли **все** вызовы `FUN_140004a4c` (отправка `0x222018`) в порядке выполнения от `DriverEntry`/StartDevice/создания потока: код msg, device_id, payload.
2. Особо: есть ли `NOTIFICATION_ALIVE` (100), `SET_REMOTE_ACCESS` (32, с каким mode), `GET_INPUT/OUTPUT_STREAM_LIST`, `SET_PROPERTY` (9), `PROPERTY_LISTENER` (11), `SET_INPUT_STREAM_ADDRESS_RANGES` (18)?
3. Сравни с нашей последовательностью (`DRIVER_GUIDE.md` §5.3) и kaiT2en (`t2audio_init_cmd`, `t2audio_init_dev`).

**Результат:** таблица «AppleAudio / kaiT2en / мы» и список того, чего у нас не хватает.

### R6. Часы и позиция в AppleAudio

AppleAudio сохраняет timestamp в контекст устройства (`+0xE958`, счётчик `+0xE954`).

1. Найди чтения `+0xE958`/`+0xE954`: как из них считается позиция для WaveRT (`GetPosition` / регистр позиции)?
2. Сопоставь с kaiT2en `t2audio_handle_timestamp` / `t2audio_position_frames` (sampleTime, hostTime, latency).

**Результат:** модель часов, которой можно заменить QPC-экстраполяцию в `T2AudioStreamAdvanceLocked`.

### R7. Формат сэмплов и копирование в AppleAudio

Мы сдвигаем 32-битные сэмплы на 8 бит вправо (S24_LE, по `'pft '`). Нужно подтвердить по AppleAudio:

1. Найди цикл, который пишет PCM в кольцо BAR0 (или регистрирует свой буфер в `BufferStruct` — `FUN_1400046f8`, `FUN_140001fa4`).
2. Есть ли там `sar …, 8` / маскирование / перестановка каналов? Какой формат AppleAudio заявляет Windows (стерео PCM16) и как он раскладывает его по 6 каналам T2?

**Результат:** подтверждение или опровержение `>> 8` и **порядок каналов**, а значит, какой канал какой динамик.

### R8. Питание

1. Обработчики `IRP_MJ_POWER` / `IAdapterPowerManagement` в AppleAudio: что делается при переходе в D3 и обратно? Есть ли запись sleep marker `0x19770525` в GPR `+0x10`, `SET_REMOTE_ACCESS(OFF/ON)`, повторная регистрация буферов?
2. Сравни с kaiT2en `t2audio_quiesce`, `t2audio_suspend`, `t2audio_reset_buffers`.

### R9. Выбор получателя `0x222020` и сосуществование

1. Аргументы `IoGetDeviceInterfaces` в `0x140002477` (флаги: только включённые интерфейсы?).
2. Перечисляет ли AppleUSBVHCI интерфейсы на **каждое** сообщение или кэширует указатель? (Важно для частоты `UPDATE_TIMESTAMP`.)
3. Что будет, если в системе есть и наш, и AppleAudio интерфейс?

---

## 3. После реверса: проверка на железе (только с одобрения владельца)

Исправление R1 уже в коде (`T2AudioDeviceControl` в `src/Driver.c`). Агент `tools/T2Audio-TestAgent.ps1` по-прежнему **отказывается** от `-BceIo`. Снимать этот запрет можно только когда выполнено всё:

1. R1 подтверждает гипотезу (или дамп хотя бы не противоречит ей).
2. R2 и R3 не выявили дополнительного обязательного шага (смена тега, ответ на timestamp). Если выявили — он реализован и покрыт тестами.
3. Стадии 1–3 и стадия 5 без `-BceIo` прошли по `GEMINI.md`.
4. **Владелец проекта явно разрешил** прогон `-Stage 4 -BceIo`.

Прогон — **беззвучный**: Mute и громкость 0, без тона и плееров. Для проверки причины BSOD звук не нужен — достаточно, чтобы `START_IO` прошёл и пришли отметки времени.

Ожидаемое в логе при первом `START_IO`:

```
T2Audio: START_IO sent to device 0x39
T2Audio: VHCI UPDATE_TIMESTAMP #1 dev=0x39 t2=… seed=… sample=…
```

и **никакого** BSOD. После прогона: `-Mode Recover`, проверка AppleAudio, отчёт в `docs/logs/EXCERPT_*.md` и `CURRENT_STATE.md`.
