# T2AudioPort — подробное руководство по драйверу

> Документ описывает **текущий исходный код** ветки и то, как с ним работать: устройство железа, протокол, архитектуру драйвера, потоковый тракт, модель безопасности, сборку, тестирование на железе и диагностику.
> Хронология и доказательства с железа — в [`CURRENT_STATE.md`](CURRENT_STATE.md) (единственный авторитетный статус-документ). Отчёты реверс-инжиниринга — в [`analysis/`](analysis/).

## Содержание

1. [Статус и ограничения](#1-статус-и-ограничения)
2. [Аппаратура: T2 audio controller](#2-аппаратура-t2-audio-controller)
3. [Протокол BCE «Audt»](#3-протокол-bce-audt)
4. [Архитектура драйвера](#4-архитектура-драйвера)
5. [Жизненный цикл устройства](#5-жизненный-цикл-устройства)
6. [Потоковый тракт (WaveRT → BAR0)](#6-потоковый-тракт-wavert--bar0)
7. [Синхронизация и IRQL](#7-синхронизация-и-irql)
8. [Модель безопасности, стадии и параметры реестра](#8-модель-безопасности-стадии-и-параметры-реестра)
9. [Сборка и тесты](#9-сборка-и-тесты)
10. [Установка и тест на железе](#10-установка-и-тест-на-железе)
11. [Диагностика: что должно быть в логе](#11-диагностика-что-должно-быть-в-логе)
12. [Известные проблемы](#12-известные-проблемы)
13. [Сравнение с kaiT2en](#13-сравнение-с-kait2en)
14. [Как вносить изменения](#14-как-вносить-изменения)
15. [Глоссарий](#15-глоссарий)

---

## 1. Статус и ограничения

| Что | Состояние |
|---|---|
| Сборка | В CI (`.github/workflows/build.yml`): WDK, Release x64 + хост-тесты MSVC/gcc/clang. |
| Хост-тесты | `tests/BceProtocolLogicTest.c`, `tests/T2AudioBufferLogicTest.c` — проходят. |
| Стадии 1–3 на железе (RAM, BAR, BCE discovery) | Проверялись на MacBookPro16,1 на более ранних сборках: BAR, `BufferStruct` и устройство `Speaker` `0x39` находятся. |
| `START_IO` на железе | Падал с BSOD `0x7E`. Причина найдена статически (IOCTL `0x222020` от AppleUSBVHCI), обработчик добавлен; **на железе не проверено**. Агент по-прежнему отказывается запускать `-BceIo`. |
| Запись PCM в BAR0 (стадия 5) | В текущем виде на железе **не проверялась**. |
| Сон / пробуждение | Не реализовано. **Не уводите Mac в сон во время сессии стадии ≥ 4.** |
| Микрофон, наушники, громкость | Не реализовано (только встроенные динамики). |

Драйвер **не стартует при обычной загрузке**: для запуска нужен волатильный ключ реестра `TestSession` (§8). Это сделано намеренно, чтобы падение не превратилось в бесконечную перезагрузку.

---

## 2. Аппаратура: T2 audio controller

### 2.1 Устройство

* PCI `VEN_106B&DEV_1803` (на тестовой машине `SUBSYS_1887106B&REV_01`). Это функция T2, через которую bridgeOS отдаёт хосту аудио.
* Штатный драйвер Boot Camp — `AppleAudio.sys`. Он даёт **стерео**-endpoint. Попытки открыть 6 каналов через него отклоняются (`NOTES.md` §1).
* Командный канал идёт не через этот PCI-device, а через виртуальный хост-контроллер **`AppleUSBVHCI.sys`** (BCE — Buffer Copy Engine), IOCTL `0x222018` (§3).

### 2.2 Ресурсы памяти

| Ресурс | Назначение | Как драйвер его находит |
|---|---|---|
| Memory resource `[0]` (kaiT2en: BAR0) | Окно кольцевых аудиобуферов и `BufferStruct` | Первый memory-ресурс. Мапится R/W. Если ресурс prefetchable (`CM_RESOURCE_MEMORY_PREFETCHABLE`) — write-combining, иначе uncached. |
| Config BAR (kaiT2en: BAR4) | GPR-блок по смещению `+0xC000` | Перебор всех memory-ресурсов: используется первый, у которого `GPR[1] == 0x19870423` и `GPR[0] >= 2`. Порядок ресурсов в Windows не обязан совпадать с порядком BAR. Мапится read-only. |

GPR (`config + 0xC000`, по 32 бита):

| Смещение | Поле |
|---|---|
| `+0x00` | version (драйвер требует ≥ 2, kaiT2en — ≥ 3) |
| `+0x04` | signature `0x19870423` |
| `+0x08` | смещение `BufferStruct` внутри BAR0 |
| `+0x10` | sleep marker `0x19770525` (пишется перед сном; драйвер его пока не использует) |

### 2.3 `BufferStruct` (в BAR0)

Упаковка 4 байта, все смещения проверены `C_ASSERT` в `src/T2AudioMiniport.h`:

```
T2AUDIO_BUFFER_STRUCT            (заголовок 0x10)
  +0x00 u32 Version
  +0x04 u32 Signature            = 0x19870423
  +0x08 u32 Flags
  +0x0C u8  NumDevices (+3 reserved)
  +0x10 DEVICE_METADATA Devices[20]          шаг 0xBDEC

DEVICE_METADATA                  (0xBDEC)
  +0x0000 char Name[128]          "Speaker", "Codec Output", ...
  +0x0080 u8   NumInputStreams
  +0x0081 u8   NumOutputStreams
  +0x0084 STREAM_METADATA InputStreams[5]    шаг 0x12E4
  +0x5EF8 STREAM_METADATA OutputStreams[5]
  +0xBD6C u8   Filler[128]

STREAM_METADATA                  (0x12E4)
  +0x0000 u8  NumBuffers
  +0x0004 BUFFER_ENTRY Buffers[100]          шаг 0x30
  +0x12C4 u8  Filler[32]

BUFFER_ENTRY                     (0x30)
  +0x00 u64 Address               смещение буфера ОТ НАЧАЛА BAR0
  +0x08 u64 Size
  +0x10 u64 Pad[4]
```

`T2AudioMapResources` читает `NumDevices` **один раз**. Эту копию он проверяет через `T2AudioDeviceTableFits` (таблица должна целиком помещаться в смапленный BAR) и её же передаёт в `T2AudioFindSpeakerBuffer`. Это защищает от изменения значения в device memory между проверкой и обходом. Устройство ищется по точному имени `"Speaker"` с терминатором. Его буфер — `OutputStreams[0].Buffers[0]`, а `Address + Size` должны помещаться в BAR0.

### 2.4 Кольцо динамиков

* Размер `0x61800` = **399 360 байт** = 16 640 кадров × 24 байта ≈ **346,7 мс** при 48 кГц.
* Формат (свойство `'pft '` потока `0x3A`, снят на MacBookPro16,1, `NOTES.md` §4):

| Поле | Значение |
|---|---|
| sample rate | 48 000 |
| format id | `'lpcm'` (`0x6C70636D`) |
| flags | `0x4` = SIGNED (не float, little-endian, interleaved, не packed, не aligned-high) |
| bytes per frame / packet | 24 / 24, frames per packet 1 |
| channels | **6** |
| bits per channel | **24** |

* Итого: 6 каналов по 4 байта, в каждом 24 значащих бита в **младших** трёх байтах (`S24_LE` в терминах ALSA, так это интерпретирует kaiT2en). Windows отдаёт 32-битный PCM, выровненный по **старшим** битам, поэтому при копировании каждый сэмпл сдвигается на 8 бит вправо со знаком (§6.5).
* bridgeOS читает кольцо по кругу после `START_IO`. Аппаратная позиция чтения хосту напрямую не видна: kaiT2en получает её из сообщений `UPDATE_TIMESTAMP`, а этот драйвер — пока нет (§12).

---

## 3. Протокол BCE «Audt»

### 3.1 Транспорт (`src/BceTransport.c`)

* Устройство `\Device\AppleUSBVHCI`, открывается `IoGetDeviceObjectPointer(FILE_ALL_ACCESS)`. Ссылка на file object удерживается всё время, пока транспорт открыт.
* `IOCTL 0x222018` = `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)`. Входной буфер — сообщение, выходной — ответ. `IoStatus.Information` — длина ответа (обрезается до размера буфера).
* IRP строится `IoBuildDeviceIoControlRequest`, `FileObject` в стеке IRP **не** выставляется (как у `AppleAudio.sys`; см. §12 — BSOD это не исправило).
* Все отправки сериализуются `KMUTEX`. Захват ждёт не дольше 3000 мс, сама отправка — не дольше **2000 мс**.
* **Poisoning.** Если ответ не пришёл за 2000 мс, IRP бросается как есть (без `IoCancelIrp` — это была бы гонка с освобождением IRP), контекст намеренно утекает, транспорт помечается «отравленным». После этого все отправки сразу возвращают `STATUS_IO_TIMEOUT`, а транспорт никогда не закрывается: ссылка на file object держит целевое устройство живым для зависшего IRP.
* Отправка запрещена выше `PASSIVE_LEVEL` (`STATUS_INVALID_DEVICE_STATE`).

### 3.2 Формат сообщения (little-endian, без выравнивания)

```
header (13 байт)
  +0  char tag[4]   = "Audt"
  +4  u8   type     1 = COMMAND, 2 = RESPONSE, 3 = NOTIFICATION
  +5  u64  device_id
base (8 байт)
  +13 u32  msg       код сообщения
  +17 u32  status    0 = успех
payload — с +21
```

### 3.3 Используемые команды

| Команда (msg) | Ответ | device_id | Payload запроса | Payload ответа | Где |
|---|---|---|---|---|---|
| `GET_DEVICE_LIST` (101) | 102 | 0 | — | `count u64`, `count × id u64` | `T2AudioGetDeviceList` |
| `GET_PROPERTY` (7) | 8 | устройство | `obj u64, element u32, scope u32, selector u32, qualifier_size u64` (28 байт) | `obj, element, scope, selector, data_size u64`, затем данные (с +49) | `T2AudioGetProperty` |
| `GET_OUTPUT_STREAM_LIST` (26) | 27 | устройство | — | как у списка устройств | `T2AudioGetOutputStreamList` |
| `SET_REMOTE_ACCESS` (32) | 33 | 0 | `mode u64` (0 = OFF, 2 = ON) | — | `T2AudioSetRemoteAccess` |
| `START_IO` (0) | 1 | speaker | — | — | `T2AudioStartIo` (`Phase4.c`) |
| `STOP_IO` (2) | 3 | speaker | — | — | `T2AudioStopIo` |

Каждый ответ проверяется: тег, `type == RESPONSE`, ожидаемый код, `status == 0`, длина. Для простых команд дополнительно проверяется эхо `device_id`, для свойств — эхо `obj/element/scope/selector`. Все разборщики — чистые функции в `src/BceProtocolLogic.h`, покрытые тестами.

### 3.4 Используемые свойства

| Selector | Scope | Объект | Значение |
|---|---|---|---|
| `'uid '` `0x75696420` | `'glob'` | устройство | имя, например `Speaker` (≤ 128 байт, без NUL) |
| `'pft '` `0x70667420` | `'glob'` | поток | 40-байтный дескриптор формата (§3.5) |
| `'ltnc'` `0x6C746E63` | `'glob'` / `'outp'` | поток / устройство | задержка, кадры (u32) |
| `'saft'` `0x73616674` | `'outp'` | устройство | safety offset, кадры (u32) |

### 3.5 Дескриптор `'pft '`

```
+0  u64 sample_rate   IEEE-754 double (декодируется без FPU: T2AudioBceDecodeIntegralDouble)
+8  u32 format_id     'lpcm'
+12 u32 format_flags  1 FLOAT, 2 BIG_ENDIAN, 4 SIGNED, 8 PACKED, 16 ALIGNED_HIGH,
                      32 NON_INTERLEAVED, 64 NON_MIXABLE
+16 u32 bytes_per_packet
+20 u32 frames_per_packet
+24 u32 bytes_per_frame
+28 u32 channels_per_frame
+32 u32 bits_per_channel
+36 u32 reserved
```

`T2AudioBceDeviceSampleShift` принимает только 6 каналов / 48 кГц / контейнер 4 байта / signed / little-endian / interleaved / не packed / не float:

* 32 бита → сдвиг 0;
* 24 бита + `ALIGNED_HIGH` → 0;
* 24 бита без него → **8** (случай динамиков);
* всё остальное → `-1`: копирование в MMIO запрещено.

### 3.6 Устройства на MacBookPro16,1

`0x21` Digital Mic, `0x25` Codec Output, `0x39` **Speaker** (поток `0x3A`), `0x43` Codec Input, `0x47` Bridge Loopback.

---

## 4. Архитектура драйвера

### 4.1 Файлы

| Файл | Язык | Отвечает за |
|---|---|---|
| `src/Driver.c` | C++ | `DriverEntry`, `AddDevice`, `StartDevice` (порядок стадий, регистрация портов), PnP-хук освобождения ресурсов, чтение реестра. |
| `src/Device.c` | C++ | Маппинг/размаппинг BAR, поиск GPR и `BufferStruct`, `T2AudioFindSpeakerBuffer`. |
| `src/BceTransport.c` | C | Транспорт BCE, `GET_DEVICE_LIST` / `GET_PROPERTY` / `GET_OUTPUT_STREAM_LIST` / `SET_REMOTE_ACCESS`, probe и запрос формата потока. |
| `src/Phase4.c` | C | `START_IO` / `STOP_IO`, (устаревший) hardware MDL. |
| `src/WaveRTMiniport.c` | C | `IMiniportWaveRT`: описание фильтра, пины, `DataRangeIntersection`, проверка формата. Здесь же `INITGUID`. |
| `src/WaveRTStream.c` | C | `IMiniportWaveRTStream`: буфер, состояния, позиция, копирование в BAR0, латентность. |
| `src/Topology.c` | C | `IMiniportTopology`: bridge-пин → пин динамика. |
| `src/T2AudioMiniport.h` | — | Структуры железа (`C_ASSERT` на размеры), контекст устройства, прототипы. |
| `src/BceProtocolLogic.h` | C (чистый) | Разбор/сборка BCE-сообщений, `'pft '`, сдвиг сэмплов. Компилируется и в драйвер, и в тесты. |
| `src/T2AudioBufferLogic.h` | C (чистый) | Арифметика буферов: выравнивание, куски копирования, QPC → кадры, конверсия сэмплов, решения по освобождению буфера и I/O. |

Правило проекта: вся нетривиальная арифметика и разбор вынесены в чистые заголовки без зависимостей от ядра, и каждая такая функция покрыта хост-тестом.

### 4.2 Объекты PortCls

```
Adapter (PcAddAdapterDevice, extension = PortCls + T2AUDIO_DEVICE_CONTEXT)
 ├─ Subdevice "Topology"  (PortTopology + T2AUDIO_TOPOLOGY)
 │    pin 0 bridge IN  (analog) ──► pin 1 Speaker OUT (KSNODETYPE_SPEAKER)
 ├─ Subdevice "Wave"      (PortWaveRT + T2AUDIO_MINIPORT)
 │    pin 0 render sink (PCM 6ch/32bit/48k, KSCATEGORY_AUDIO) ──► pin 1 bridge OUT
 └─ PcRegisterPhysicalConnection(Wave pin 1 → Topology pin 0)
```

Контекст устройства `T2AUDIO_DEVICE_CONTEXT` лежит сразу за `PORT_CLASS_DEVICE_EXTENSION_SIZE` в device extension (`T2AudioGetContext`). Основные поля:

| Поле | Смысл |
|---|---|
| `Bar1Mapped/Size/Physical`, `Bar1WriteCombined` | BAR0 (кольца). Имя «Bar1» историческое. |
| `Bar2Mapped/Size` | config BAR (GPR) |
| `BufferStruct`, `SpeakerBufferOffset/Size` | найденное кольцо динамиков |
| `HardwareLock`, `HardwareReady` | защита MMIO от teardown (§7) |
| `BceSpeakerDeviceId`, `SpeakerDeviceId` | найденный / «подключённый» speaker (второе — только на стадии ≥ 4) |
| `SpeakerStreamId`, `SpeakerFormatValid`, `SpeakerSampleShift`, `SpeakerLatencyFrames`, `SpeakerSafetyOffsetFrames` | результат запроса потока (§5.3) |
| `BceRemoteAccess` | `SET_REMOTE_ACCESS(ON)` подтверждён |
| `TestModeEnabled`, `DiagStage`, `MmioCopyEnabled`, `BceIoEnabled`, `ForceSystemBuffer`, `MdlVariant` | гейты и параметры (§8) |

### 4.3 Формат на пине

* Data range: `KSDATARANGE_AUDIO`, PCM, 6 каналов, 32 бита, 48 000 Гц.
* `DataRangeIntersection` (pin 0) возвращает **`KSDATAFORMAT_WAVEFORMATEXTENSIBLE`**: 6 каналов, 48 кГц, 32/32 бита, `nBlockAlign` 24, маска `KSAUDIO_SPEAKER_5POINT1_SURROUND` (`0x60F`), subformat PCM. При `OutputBufferLength == 0` возвращается требуемый размер и `STATUS_BUFFER_OVERFLOW`.
* `T2AudioValidateSixChannelFormat` принимает этот формат с маской `0x60F` или `0x3F` и `wValidBitsPerSample` от 1 до 32, а также «плоский» `WAVE_FORMAT_PCM` 6/48000/32.

Порядок каналов 5.1 Windows: FL, FR, C, LFE, SL/BL, SR/BR. Как каналы кольца соответствуют физическим динамикам (вуферы/твитеры), ещё не подтверждено — см. заметки DSP в `planning/CURRENT_STATE.md`. Эквализацию по динамикам, которую на macOS делает DSP, драйвер не воспроизводит.

---

## 5. Жизненный цикл устройства

### 5.1 `DriverEntry`

1. `T2AudioInitializeBceTransport` — инициализация мьютекса транспорта.
2. `PcInitializeAdapterDriver(…, T2AudioAddDevice)`.
3. `MajorFunction[IRP_MJ_PNP] = T2AudioPnpHandler` (обязательно **после** PortCls; хук передаёт IRP в `PcDispatchIrp`).

### 5.2 `T2AudioStartDevice` — шаг за шагом

1. `T2AudioUnmapResources` (на случай повторного старта), обнуление контекста, `KeInitializeSpinLock(HardwareLock)`.
2. Safe Mode → отказ (`STATUS_UNSUCCESSFUL`).
3. Нет `TestSession\EnableTestMode=1` → отказ (Code 10). Иначе читаются `DiagStage`, `EnableMmioCopy` (работает только на стадии ≥ 5), `EnableBceIo` (только на стадии ≥ 4).
4. `Parameters\ForceSystemBuffer`, `Parameters\MdlVariant`.
5. **Стадия ≥ 2:** `T2AudioMapResources`: маппинг BAR0, поиск config BAR по GPR, проверки, `BufferStruct`, кольцо динамиков. При успехе `HardwareReady = TRUE`.
6. Создание и регистрация Topology, затем WaveRT, затем физического соединения. При ошибке всё созданное освобождается и BAR размапливаются.
7. **Стадия ≥ 3 и `HardwareReady`:** `T2AudioProbeBceDevices` (§5.3).
8. **Стадия ≥ 4, speaker найден:** `SpeakerDeviceId = BceSpeakerDeviceId`. Если `EnableBceIo` — `SET_REMOTE_ACCESS(ON)` → `BceRemoteAccess`. Транспорт остаётся открытым. Иначе транспорт закрывается.

### 5.3 BCE probe и запрос потока (стадия ≥ 3)

```
Open \Device\AppleUSBVHCI
GET_DEVICE_LIST                       → [0x21, 0x25, 0x39, 0x43, 0x47]
для каждого: GET_PROPERTY 'uid '      → лог, "Speaker" → BceSpeakerDeviceId
GET_OUTPUT_STREAM_LIST(speaker)       → [0x3A]            → SpeakerStreamId
GET_PROPERTY(0x3A, 'pft ')            → формат → SpeakerSampleShift, SpeakerFormatValid
GET_PROPERTY(0x3A, 'ltnc')            ┐
GET_PROPERTY(speaker, outp 'ltnc')    ├→ SpeakerLatencyFrames (сумма), SpeakerSafetyOffsetFrames
GET_PROPERTY(speaker, outp 'saft')    ┘   (необязательные, ошибка = 0)
```

Если формат не прочитан или не поддерживается, драйвер продолжает работать, но **никогда не пишет PCM в BAR0**.

### 5.4 Остановка и удаление

`T2AudioPnpHandler` на `IRP_MN_STOP_DEVICE`, `IRP_MN_SURPRISE_REMOVAL`, `IRP_MN_REMOVE_DEVICE`:

1. Если `BceRemoteAccess` и это не surprise removal — `SET_REMOTE_ACCESS(OFF)` (управление возвращается bridgeOS, как в quiesce у kaiT2en).
2. `T2AudioUnmapResources`: под `HardwareLock` — `HardwareReady = FALSE` и обнуление указателей. Затем `KeFlushQueuedDpcs`, закрытие транспорта (если он не «отравлен»), освобождение MDL и `MmUnmapIoSpace`. Функция идемпотентна.
3. `PcDispatchIrp`.

---

## 6. Потоковый тракт (WaveRT → BAR0)

### 6.1 Почему зеркало, а не прямой буфер

PortCls мапит буфер WaveRT в user mode через `MmMapLockedPagesSpecifyCache(UserMode)`, а страницы устройства (BAR) так смапить нельзя. Попытка отдать MDL на BAR давала `0x8007001F` при инициализации потока. Поэтому:

* `AllocateAudioBuffer` выделяет обычную память `AllocatePagesForMdl` ровно запрошенного размера (с округлением до кадра). Ядерный адрес получается через `MapAllocatedPages(MmCached)`, буфер заполняется нулями.
* Драйвер сам копирует данные из этого буфера в кольцо BAR0.
* `FreeAudioBuffer` / финальный `Release`: остановка таймера → отвязка буфера под `StreamLock` → `UnmapAllocatedPages` → `FreePagesFromMdl`.

Типичный размер, который просит движок, — 11 520 байт (480 кадров = 10 мс).

### 6.2 Часы и «потреблённый» курсор

* При входе в RUN фиксируется `AnchorQpc` — **до** `START_IO`, потому что T2 не может начать читать раньше. Вместе с ним задаётся `AnchorFrames = LastCopiedFrame`.
* Текущий кадр источника: `playFrame = AnchorFrames + TicksToFrames(QPC − AnchorQpc)`. `T2AudioTicksToFrames` не переполняется при любой длительности.
* `LastCopiedFrame` — **потреблённый курсор**: все кадры до него уже скопированы в BAR0 (или пропущены, если MMIO выключен). Именно он сообщается движку как позиция воспроизведения.

### 6.3 Алгоритм `T2AudioStreamAdvanceLocked`

Вызывается под `StreamLock` и только в RUN — из `GetPosition`, из DPC (таймер 2 мс как подстраховка) и при выходе из RUN (финальный сброс).

```
playFrame = AnchorFrames + elapsed
frame     = LastCopiedFrame
если playFrame <= frame → ничего
pending = playFrame - frame
если pending > кадров_в_буфере_клиента:        # клиент уже перезаписал старые кадры
    frame = playFrame - кадров_в_буфере_клиента
если можно писать в MMIO (под HardwareLock):
    пока остались кадры:
        кусок = T2AudioCalculateCopyChunk(
                    src = frame,
                    dst = frame + LEAD (720),    # +15 мс в кольце T2
                    …)                           # не пересекает конец ни одного кольца
        T2AudioConvertSamples(dst, src, кусок, SpeakerSampleShift)
    _mm_sfence()                                 # слить WC-буферы
LastCopiedFrame = playFrame
```

Свойства алгоритма:

* **Движок не может перезаписать кадр раньше, чем тот скопирован.** Позиция, которую он видит, — это уже скопированный курсор, и `GetPosition` сам докопирует данные перед ответом. Поэтому тракт не зависит от того, что системный таймер по умолчанию тикает раз в 15,6 мс, а не каждые 2 мс.
* Кадр источника `F` попадает в кольцо T2 по индексу `(F + 720) mod 16640`. Если T2 читает кольцо с той же скоростью от момента `START_IO`, данные опережают его на ~15 мс.
* На PAUSE → RUN продолжение идёт ровно с `LastCopiedFrame`: ни пропусков, ни повторов.

### 6.4 Состояния (`SetState`)

| Переход | Действия |
|---|---|
| → RUN (из STOP/ACQUIRE/PAUSE) | нужен выделенный буфер. Если `LastCopiedFrame == 0` (свежий запуск) — кольцо BAR0 обнуляется (под `HardwareLock`, только при разрешённом MMIO). Затем QPC-якорь. `START_IO`, если `EnableBceIo` и speaker подключён и I/O ещё не запущен (ошибка `START_IO` = ошибка перехода). Потом публикация якоря, `State = RUN` и старт таймера. |
| RUN → PAUSE/ACQUIRE/STOP | остановка таймера (`KeCancelTimer` + `KeFlushQueuedDpcs`), финальный `Advance`, `AnchorFrames = LastCopiedFrame`. На STOP курсоры обнуляются, кольцо BAR0 обнуляется. `STOP_IO`, если I/O был запущен этим потоком; при ошибке флаг всё равно сбрасывается. |

`KSSTATE`: 0 STOP, 1 ACQUIRE, 2 PAUSE, 3 RUN — эти числа видны в логе `SetState %u`.

### 6.5 Конверсия сэмплов

`T2AudioConvertSamples(dst, src, n, shift)` делает одну выровненную 32-битную запись на сэмпл и арифметический сдвиг вправо с переносимой обработкой знака:

| Вход (Windows, 32 бит) | Выход в кольцо (S24_LE) |
|---|---|
| `0x7FFFFF00` (+full scale) | `0x007FFFFF` |
| `0x80000000` (−full scale) | `0xFF800000` |
| `0x00000100` (+1 LSB 24-бит) | `0x00000001` |

### 6.6 Позиция и латентность

* `GetPosition`: `PlayOffset = (LastCopiedFrame × 24) mod размер_буфера`, `WriteOffset = PlayOffset + 512 кадров`.
* `GetPositionRegister` / `GetClockRegister` → `STATUS_NOT_SUPPORTED`: регистр в обычной памяти ядра пришлось бы отдавать в user mode, поэтому движок опрашивает `GetPosition`.
* `GetHWLatency`: `FifoSize = 720 × 24` байт (опережение в кольце), `CodecDelay = (latency потока + latency устройства) × 10⁷ / 48000` в единицах 100 нс.

### 6.7 Числа на одном примере

Буфер клиента 11 520 байт (480 кадров), кольцо 399 360 байт (16 640 кадров):

* через 100 мс после старта `playFrame = 4800`;
* кадр источника 4800 лежит по смещению `(4800 mod 480) × 24 = 0`;
* в кольцо он пишется по смещению `((4800 + 720) mod 16640) × 24 = 132 480`;
* после кадра 16 639 запись в кольцо идёт снова с 0 — `T2AudioCalculateCopyChunk` режет кусок на границе.

---

## 7. Синхронизация и IRQL

| Объект | Тип | Защищает | Кто берёт |
|---|---|---|---|
| `g_BceLock` | `KMUTEX` (PASSIVE) | транспорт BCE, весь IOCTL целиком | все отправки, open/close |
| `StreamLock` (на поток) | spin lock | `State`, якоря, `LastCopiedFrame`, указатели буфера | `GetPosition`, DPC, `SetState`, освобождение буфера |
| `HardwareLock` (на устройство) | spin lock | `HardwareReady` + маппинги BAR0 во время записи | копирование, обнуление кольца, `T2AudioUnmapResources` |

* Порядок захвата: **`StreamLock` → `HardwareLock`**. Обратного порядка нигде нет.
* Все BCE-отправки — только на `PASSIVE_LEVEL` (проверяется явно).
* Запись в BAR0 — только под `HardwareLock` и только при `HardwareReady`. Teardown снимает флаг под тем же локом до `MmUnmapIoSpace`, поэтому размаппинг посреди записи невозможен.
* Таймер копирования: `KeSetTimerEx` с периодом 2 мс. Остановка — `KeCancelTimer` + `KeFlushQueuedDpcs` на PASSIVE (`SetState` и `FreeAudioBuffer` — PASSIVE-колбэки).
* Поток держит ссылку на `PortStream` (AddRef при создании, Release при уничтожении).

---

## 8. Модель безопасности, стадии и параметры реестра

### 8.1 `TestSession` (волатильный)

`HKLM\SYSTEM\CurrentControlSet\Services\T2AudioMiniport\TestSession`. Создаётся **только** агентом `tools/T2Audio-TestAgent.ps1` как volatile-ключ. Ядро стирает его при любой перезагрузке, в том числе после BSOD.

| Значение (DWORD) | Смысл | По умолчанию |
|---|---|---|
| `EnableTestMode` | 1 = драйвер может стартовать | нет → Code 10 |
| `DiagStage` | 1–5, стадии накопительные | 1 |
| `EnableMmioCopy` | 1 = разрешить запись PCM в BAR0 (только стадия 5) | 0 |
| `EnableBceIo` | 1 = разрешить `SET_REMOTE_ACCESS(ON)`, `START_IO`, `STOP_IO` (стадия ≥ 4) | 0 |

### 8.2 Стадии

| Стадия | Что добавляется | Что трогает железо |
|---|---|---|
| 1 RAM only | endpoint и поток на системной памяти, позиция по QPC | ничего |
| 2 BAR metadata | маппинг BAR, чтение GPR и `BufferStruct` | только чтение device memory |
| 3 BCE discovery | открытие транспорта, `GET_DEVICE_LIST`, `uid`, список потоков, `'pft '`/`'ltnc'`/`'saft'` | только запросы-чтения по BCE |
| 4 hardware I/O | speaker подключён, транспорт держится открытым. С `EnableBceIo`: `SET_REMOTE_ACCESS`, `START_IO`/`STOP_IO` | команды управления (**BSOD-путь**) |
| 5 MMIO copy | с `EnableMmioCopy` и валидным форматом: запись PCM в BAR0, обнуление кольца | запись в device memory |

Поднимайте стадию по одной, чтобы любой сбой можно было приписать одному шагу.

### 8.3 `Parameters` (постоянный)

`…\T2AudioMiniport\Parameters`:

| Значение | Смысл |
|---|---|
| `ForceSystemBuffer` | 1 = никогда не делать hardware I/O (чисто программный поток) |
| `MdlVariant` | эксперименты с MDL на BAR (`Phase4.c`). Текущий тракт всегда использует системный буфер, так что практически не используется. |

### 8.4 Почему нет «нормального режима»

Пока `START_IO` может уронить систему, автозапуск на обычной загрузке означал бы BSOD при каждом старте Windows. Волатильный ключ гарантирует, что после падения система загрузится без драйвера. Ранние записи в документации, где утверждалось обратное, исправлены.

---

## 9. Сборка и тесты

### 9.1 CI

`.github/workflows/build.yml` запускается на каждый push и PR:

* **Linux:** оба теста с gcc и clang, `-Wall -Wextra -Werror`.
* **windows-2022:** установка WDK, `msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.26100.0`, тесты на MSVC (`/W4`), выгрузка **неподписанного** `T2AudioMiniport.sys` и `.pdb` как артефакта.

### 9.2 Локально

Нужны Visual Studio 2022 (или новее) с C++ и WDK той же версии SDK.

```powershell
msbuild src\T2AudioMiniport.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild
# результат: src\bin\Release\T2AudioMiniport.sys
```

В проекте указан `WindowsTargetPlatformVersion 10.0.28000.0`. Если такого SDK нет, передайте установленную версию через `/p:WindowsTargetPlatformVersion=…`, как это делает CI.

### 9.3 Хост-тесты

```powershell
cl /nologo /W4 /Fe:tests\BceProtocolLogicTest.exe   tests\BceProtocolLogicTest.c
cl /nologo /W4 /Fe:tests\T2AudioBufferLogicTest.exe tests\T2AudioBufferLogicTest.c
tests\BceProtocolLogicTest.exe
tests\T2AudioBufferLogicTest.exe
```

```bash
gcc -Wall -Wextra -o /tmp/bce tests/BceProtocolLogicTest.c && /tmp/bce
gcc -Wall -Wextra -o /tmp/buf tests/T2AudioBufferLogicTest.c && /tmp/buf
```

Что покрыто:

| Набор | Проверки |
|---|---|
| `BceProtocolLogicTest` | LE-аксессоры, список устройств, ответ `GET_PROPERTY` (границы, эхо, статус), ответы команд, `uid == "Speaker"`, дескриптор `'pft '` с железа, декодер double, выбор сдвига. |
| `T2AudioBufferLogicTest` | выравнивание по кадру, решения о буфере и I/O, `DeviceTableFits` (переполнения), куски копирования (обе границы, раздельные src/dst, опережение), QPC → кадры, конверсия S24. |

---

## 10. Установка и тест на железе

> Перед каждым прогоном читайте последнюю запись в [`CURRENT_STATE.md`](CURRENT_STATE.md). Держите под рукой восстановление `AppleAudio.sys`.

### 10.1 Подготовка (один раз)

1. Включите test signing (`bcdedit /set testsigning on`, перезагрузка). Сертификат проекта — `docs/T2AudioCert_thumbprint.txt`.
2. Подпишите собранный `.sys` и каталог в `packaging/`: INF `T2AudioMiniport.inf`, CDF `t2audio.cdf`. При изменении `.sys` каталог нужно пересоздать и переподписать, а `DriverVer` в INF — поднять.
3. `tools/PreInstall-Check.ps1` — проверки и резервные копии.

### 10.2 Установка

* `tools/Install-T2AudioDriver.ps1` по умолчанию только добавляет пакет в driver store (`pnputil /add-driver` без `/install`) и **не** привязывает его к устройству.
* Привязка к устройству выполняется явно (см. `tools/INSTALL_SCRIPTS_README.md`). После неё может понадобиться перезагрузка (Code 14).

### 10.3 Прогон

```powershell
.\tools\T2Audio-TestAgent.ps1 -Mode Status
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 1 -ExpectedSysHash <SHA256 .sys>
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 2 -ExpectedSysHash <…>
.\tools\T2Audio-TestAgent.ps1 -Mode Test -Stage 3 -ExpectedSysHash <…>   # формат потока
.\tools\T2Audio-TestAgent.ps1 -Mode Recover                               # после сбоя/перезагрузки
```

Агент создаёт сессию, включает устройство, пишет лог ядра (DbgView) в `docs/logs/capture_*`, проверяет WASAPI-рендер (`tools/Test-WasapiRender.ps1`). В блоке `finally` он **всегда** выключает устройство и удаляет сессию. Отказывается работать, если хэш установленного `.sys` не совпадает с `-ExpectedSysHash`. `-BceIo` сейчас заблокирован (§12).

Дополнительно: `tools/Capture-T2AudioLog.ps1` (захват лога), `tools/Test-WasapiTone.ps1` (тестовый тон), `tools/Set-DefaultAudioDevice.ps1` (endpoint по умолчанию), `tools/PostInstall-Validate.ps1`.

### 10.4 Откат

* `tools/Rollback-T2AudioDriver.ps1` — удаление пакета T2AudioPort (работает и из Safe Mode).
* `tools/Restore-AppleAudioDriver.ps1` — скачивает оригинальный `AppleAudio` из Boot Camp (Brigadier) и ставит обратно.
* После любого падения: `-Mode Recover`, затем проверить, что устройство снова на `AppleAudio` и в состоянии Started.

---

## 11. Диагностика: что должно быть в логе

Все сообщения начинаются с `T2Audio:`. Ожидаемая последовательность по стадиям (значения — с MacBookPro16,1):

| Стадия | Строка | Норма |
|---|---|---|
| все | `diagnostic session active stage=N mmioCopy=X bceIo=Y` | соответствует аргументам агента |
| ≥ 2 | `Resource[0] mapped at … (write-combining\|uncached)` | любой из двух вариантов |
| ≥ 2 | `GPR read: version=… signature=0x19870423 bufferOffset=…` | signature совпадает |
| ≥ 2 | `Device[i]: Name='Speaker' …`, `Speaker buffer[0]: Address=… Size=0x61800` | размер `0x61800` |
| ≥ 2 | `MapResources SUCCESS: buffer=… size=0x61800` | |
| ≥ 3 | `BCE probe: 5 device(s)`, `device 0x39 uid="Speaker"` | |
| ≥ 3 | `speaker stream 0x3A pft: rate=48000 id=0x6C70636D flags=0x4 bpp=24 fpp=1 bpf=24 ch=6 bits=24` | **обязательно перед стадией 5** |
| ≥ 3 | `speaker sample shift=8 latency=A+B frames safety=C frames` | **shift=8** |
| ≥ 3 | `BCE probe done; speaker id 0x39 stream 0x3A format=1` | `format=1` |
| ≥ 4 | `audio path ENABLED for speaker 0x39 (bceIo=…)` | |
| ≥ 4 + BceIo | `SET_REMOTE_ACCESS(1) acknowledged` | |
| поток | `AllocateAudioBuffer: system MDL=… size=11520 …`, `SetState 3 …` | |
| остановка | `PnP minor 0x02 - releasing hardware`, `SET_REMOTE_ACCESS(OFF) on teardown: 0x00000000` | 0x02 = REMOVE, 0x04 = STOP |

Типичные проблемы:

| Симптом | Где смотреть |
|---|---|
| `MapResources FAIL_F: no memory resource exposes the GPR signature` | порядок/размер ресурсов, вывод `Resource[i] GPR probe` |
| `'pft ' unavailable` или `format unsupported; MMIO copy disabled` | ответ T2 на `GET_PROPERTY`; копирование в BAR0 будет пропускаться |
| `BCE send timed out; transport poisoned` | AppleUSBVHCI не ответил за 2 с. Все дальнейшие команды до перезагрузки драйвера → `STATUS_IO_TIMEOUT` |
| Code 10 сразу после установки | нормально без `TestSession` (§8) |
| Минидамп `0x7E` в `ks!DispatchDeviceIoControl` | известная проблема `START_IO` (§12) |

---

## 12. Известные проблемы

1. **BSOD `0x7E` при `START_IO` — причина найдена статически, исправление в коде, на железе не проверено.** AppleUSBVHCI доставляет сообщения bridgeOS (`UPDATE_TIMESTAMP` после `START_IO` и др.) как `IOCTL 0x222020` в первый `KSCATEGORY_AUDIO`-интерфейс Apple (`VEN_106B`), без `FileObject`. Раньше этот IOCTL уходил в PortCls/KS → `NULL+0x18`. Теперь его перехватывает `T2AudioDeviceControl` (`src/Driver.c`), как это делает AppleAudio. Подробности: [`analysis/START_IO_BSOD_ROOT_CAUSE.md`](analysis/START_IO_BSOD_ROOT_CAUSE.md), оставшийся реверс: [`REVERSE_PLAN.md`](REVERSE_PLAN.md).
2. **Аппаратные часы пока не используются.** `UPDATE_TIMESTAMP` теперь принимается через `0x222020` и сохраняется в контексте, но позиция по-прежнему экстраполируется по QPC (модель часов — R6 в `REVERSE_PLAN.md`). На долгих прогонах возможен дрейф относительно часов T2: щелчки, переполнение/опустошение опережения.
3. **Сон.** kaiT2en выключает remote access на этапе prepare (иначе bridgeOS перестаёт отвечать, пока VHCI засыпает), пишет sleep marker в GPR `+0x10`, а при пробуждении перерегистрирует буферы. Здесь этого нет.
4. **Кольцо не отдаётся движку напрямую** (ограничение PortCls) — лишняя копия и зависимость от точности курсора.
5. **Предположение о курсоре T2.** Принимается, что T2 начинает читать кольцо с 0 в момент `START_IO` и идёт с номинальной скоростью. На железе это не подтверждено.

---

## 13. Сравнение с kaiT2en

Справочная реализация — [kaiT2en/KaiT2en-Fedora](https://github.com/kaiT2en/KaiT2en-Fedora) `modules/t2bce_audio` (GPL-2.0).

| kaiT2en | Здесь |
|---|---|
| GPR/`BufferStruct`, поиск по имени | ✅ |
| `'pft '` → формат ALSA (`S24_LE`) | ✅ формат читается, сэмплы сдвигаются `>> 8` |
| latency, safety offset | ✅ → `GetHWLatency` |
| обнуление кольца в prepare, якорь до `START_IO` | ✅ |
| `pci_iomap_wc` для prefetchable BAR | ✅ WC + `sfence` |
| `SET_REMOTE_ACCESS(ON)` до I/O, `OFF` в quiesce | ✅ (OFF — на PnP stop/remove) |
| `NOTIFICATION_ALIVE` перед командами | ❌ (§12.2) |
| позиция из `UPDATE_TIMESTAMP` | ❌ (§12.2) |
| кольцо bridgeOS = буфер ALSA | ❌ ограничение PortCls |
| suspend/resume, sleep marker | ❌ (§12.3) |
| jack (`'jack'` + property listener), входы | ❌ |

Код kaiT2en не копировался. Перенесены протокольные факты и порядок операций (см. [`THIRD_PARTY_LICENSES.md`](../THIRD_PARTY_LICENSES.md)).

---

## 14. Как вносить изменения

1. **Арифметику и разбор — в чистые заголовки** (`BceProtocolLogic.h`, `T2AudioBufferLogic.h`), без API ядра. На каждое ветвление — тест в `tests/`. Используйте типы фиксированной ширины по смыслу: `unsigned long` = ULONG только на Windows (на Linux он 64-битный, поэтому для 32-битных сэмплов там используется `unsigned int`).
2. **Новая команда BCE** — только на `PASSIVE_LEVEL`, через `T2AudioSendBceMessage`, с полной проверкой ответа (тег, тип, код, статус, эхо, длина). Разборщик добавляйте в `BceProtocolLogic.h` вместе с тестом.
3. **Любой новый доступ к железу — за гейтом стадии** (§8). Запись в BAR0 — только под `HardwareLock` и при `HardwareReady`.
4. **Не держите spin lock во время BCE-отправки** и не берите `HardwareLock` раньше `StreamLock`.
5. Перед push: тесты локально. CI соберёт драйвер с WDK. Новый бинарь на железе — только через агента с `-ExpectedSysHash`, по стадиям.
6. Каждое изменение статуса фиксируйте датированной записью в [`CURRENT_STATE.md`](CURRENT_STATE.md). Не пишите «исправлено», пока это не подтверждено на железе.

---

## 15. Глоссарий

| Термин | Значение |
|---|---|
| **T2 / bridgeOS** | сопроцессор Apple и его ОС; управляет аудиотрактом и DSP динамиков |
| **BCE** | Buffer Copy Engine — канал сообщений хост ↔ T2; в Windows доступен через `AppleUSBVHCI.sys` |
| **BAR0 / «Bar1»** | окно памяти с кольцами и `BufferStruct`; в коде поле исторически называется `Bar1*` |
| **GPR** | регистры в config BAR по `+0xC000`: версия, сигнатура, смещение `BufferStruct` |
| **`BufferStruct`** | таблица устройств/потоков/буферов в BAR0, которую заполняет bridgeOS |
| **WaveRT** | модель аудиодрайвера Windows: движок пишет в циклический буфер и опрашивает позицию |
| **Потреблённый курсор** | `LastCopiedFrame`: кадры до него уже скопированы в кольцо T2; сообщается движку как позиция |
| **Lead** | опережение записи в кольце T2 относительно курсора, 720 кадров = 15 мс |
| **S24_LE** | 24-битный сэмпл в младших трёх байтах 32-битного контейнера |
| **Стадия (DiagStage)** | уровень разрешённого доступа к железу в тестовой сессии, 1–5 |
| **Poisoned transport** | транспорт BCE после таймаута: не принимает новых команд до перезагрузки драйвера |
