# AppleAudio.sys Playback PCM Path Analysis

## Исправленные ошибки предыдущих отчётов

1. **BufferStruct.devices начинается с +0x10**
   - Функция 0x46F8: `base = bs_base + index*0xBDEC` указывает на 16 байт раньше device
   - Правильные смещения относительно этой базы:
     - `+0x90` = `device.num_input_streams` (НЕ active_flag)
     - `+0x94` = `input_streams[0].num_buffers`
     - `+0x98` = `input_streams[0].buffers[0].address`
     - `+0xA0` = `input_streams[0].buffers[0].size`

2. **Ветка 0x4838–0x4875 добавляет новое устройство**
   - НЕ output branch, как ошибочно утверждалось

3. **Функция 0x9D74 читает output буферы из BufferStruct**
   - `0x5F0C = 0x10 + 0x5EF8 + 0x04`: заголовок BS + output_streams + buffers[0].address
   - `0x5F14`: соответствующий buffers[0].size
   - Совпадает с Linux `offsetof(device, output_streams) = 0x5EF8`

## Подтверждённая цепочка вызовов для playback

### 1. Функция 0x1C240 — AllocateOutputBuffer (vtable slot +0xB8)

**Границы:** 0x1C240–0x1C4BA (из .pdata)

**Параметры:**
- RCX (RSI) = stream object
- RDX (EBX) = frame alignment/channels
- R8D (EBP) = buffer size in bytes
- R9 (R13) = output pointer для buffer handle

**Ключевые инструкции:**

```
0x1C414: call 0x9D74              ; Получить output buffer из BufferStruct
         ; Параметры:
         ; RCX = R15 (stream-8)
         ; RDX = [stream->a28]+0x21 (device name)
         ; R8  = GPR base + 0xC000
         ; [RSP+0x20] = &output_offset
         ; [RSP+0x28] = &output_size

0x1C422: mov RBX, [RSP+0x38]      ; RBX = size из BufferStruct
0x1C42C: mov [RSI+0x98], EBX      ; stream->buffer_size = size
0x1C459: add RAX, [RSP+0x40]      ; RAX = GPR_base + offset
0x1C45E: mov [RSI+0xA0], RAX      ; stream->buffer_ptr = виртуальный адрес
```

**Подтверждение:** инструкции точно совпадают с указанными в задании.

### 2. Функция 0x9D74 — GetOutputBufferFromBS

**Назначение:** извлечь физический offset и size из BufferStruct для output stream device.

**Код:**
```
0x9D74:  поиск устройства по имени в BufferStruct.devices[]
0x9DC8:  чтение [device_base + 0x5F0C] → output_streams[0].buffers[0].address
0x9DD0:  чтение [device_base + 0x5F14] → output_streams[0].buffers[0].size
```

**Смещение 0x5F0C:** `0x10 (BS header) + 0x5EF8 (offsetof output_streams) + 0x04 (stream.buffers[0])`

**Подтверждение Linux:** `audio.h:42` определяет `output_streams` с тем же layout.

### 3. Функция 0xB470 — Start/Stop stream handler (vtable slot +0x80)

**Границы:** 0xB470–0xB721

**Параметры:**
- RCX (RDI) = stream object
- RDX (RBP) = command: 1=start, 2=pause, 3=stop

**Критический участок при остановке (0xB5EC–0xB5FC):**
```
0xB5EC: mov R8D, [RDI+0x98]       ; buffer_size
0xB5F5: mov RCX, [RDI+0xA0]       ; buffer_ptr
0xB5FC: call 0xCD80               ; memset(buffer, 0, size)
```

**Вывод:** функция ОЧИЩАЕТ playback буфер при остановке. Подтверждает, что `[RSI+0xA0]` — реальный PCM buffer pointer.

### 4. Vtable структура stream object

**Базовый адрес vtable:** ~0xF048

**Ключевые слоты:**
- `+0x00` (slot 0): 0x068C0 — QueryInterface/AddRef
- `+0x80` (slot 16): 0x0B470 — Start/Stop handler
- `+0xB8` (slot 23): 0x1C240 — AllocateOutputBuffer

**Расположение указателя vtable:** `[stream+0x00]`

### 5. Функция 0x184B8 — Volume/Control handler

**НЕ ОТНОСИТСЯ к PCM playback data path.**

Эта функция вызывает vtable slot +0xA0 объекта **другого типа** (не stream), передавая параметры громкости (R9) через CFG dispatch. Это управление T2, а не запись PCM.

## Реальный playback PCM path

### Откуда берётся PCM для playback?

**Linux driver (kait2en/modules/t2bce_audio/pcm.c):**

```c
// pcm.c:259-260
/* Publish userspace prefill before starting the remote consumer. */
wmb();
```

Memory barrier **перед** вызовом `t2audio_cmd_start_io()`.

**Вывод:**
1. **Userspace** (ALSA application) заполняет буфер через `mmap()` на shared memory
2. Linux driver отправляет команду START_IO через BCE transport
3. **T2 chip читает PCM напрямую** из shared memory через DMA
4. BufferStruct содержит физические адреса этих буферов
5. Windows AppleAudio получает эти адреса из BufferStruct и отображает их в user mode

### Windows playback path

1. Windows WaveRT application записывает PCM в cyclic buffer через `mmap()`
2. AppleAudio предоставляет буфер из `[stream+0xA0]`, полученный от T2
3. T2 читает данные напрямую через DMA (аналогично Linux)
4. AppleAudio управляет только жизненным циклом (START/STOP)

**Ключевой момент:** драйвер НЕ копирует PCM. Это zero-copy архитектура через shared memory.

## Сопоставление с Linux структурами

### Точные совпадения

| Windows offset | Linux offsetof | Поле | Подтверждение |
|---|---|---|---|
| device+0x84 | 0x84 | input_streams | Функция 0x46F8 |
| device+0x5EF8 | 0x5EF8 | output_streams | Функция 0x9D74 |
| sizeof(device) | 0xBDEC | | Stride в 0x46F8 |
| sizeof(buffer) | 0x30 | | Индекс *8 в 0x480A |

### Несоответствие

**stream.buffers offset:**
- Linux: `+0x04` (после `u8 num_buffers` + padding)
- Windows читает по `+0x98` внутри stream

**Объяснение:** Windows использует **stream-relative offset внутри device**, а не структуру `t2audio_buffer_struct_stream` напрямую. Смещение 0x98 = 0x84 (input_streams base) + 0x04 (buffers offset) + выравнивание + индексация.

## Формат, каналы, Speaker

**Код выделения буфера (0x1C240):**
- Параметр R8D (EBP) — размер буфера в байтах
- Параметр RDX (EBX) — frame alignment (вероятно channels * bytes_per_sample)

**Разделение каналов не обнаружено в коде драйвера.**

Если T2 предоставляет один буфер для устройства Speaker, то все 6 каналов передаются **interleaved** в одном буфере. Разделение на независимые потоки потребовало бы:
- Несколько output_streams[] для одного device
- Или несколько devices с одинаковым именем

**Проверка BufferStruct:**
```c
// audio.h:41-42
struct t2audio_buffer_struct_stream output_streams[5];
```

T2 поддерживает до 5 output streams на device. Если Speaker использует один stream, каналы interleaved. Если несколько — возможно разделение.

**Следующий конкретный пробел:** определить, сколько output_streams регистрирует T2 для device "Speaker", и формат PCM (16-bit/24-bit, 44.1/48kHz, interleaved/planar).

## Команды для проверки (Linux)

```bash
# Подключить MacBook и загрузить kait2en
modprobe t2bce_audio

# Проверить BufferStruct
cat /sys/kernel/debug/t2audio/buffer_struct | hexdump -C

# Найти "Speaker" device и посмотреть num_output_streams
# offset +0x81 относительно начала device

# Запустить playback
aplay -D hw:CARD=AppleT2,DEV=0 test.wav

# Проверить реальные адреса буферов в dmesg
dmesg | grep t2audio
```

## Оставшиеся вопросы

1. Сколько output_streams для "Speaker"? (1 interleaved или 6 раздельных?)
2. Формат PCM: S16_LE, S24_3LE, S32_LE?
3. Sample rate: 44100, 48000, 96000?
4. Как Windows узнаёт формат? (запрос к T2 или hardcoded в драйвере?)
