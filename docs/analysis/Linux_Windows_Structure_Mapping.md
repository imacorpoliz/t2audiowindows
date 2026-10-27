# Таблица соответствия структур Linux KAIT2EN ↔ AppleAudio.sys

**Дата**: 2026-10-04  
**Цель**: Сопоставление аппаратных структур T2 между Linux-драйвером и Windows AppleAudio.sys

---

## 1. Hardware Layout (PCI BARs)

### Linux (KAIT2EN)

```c
// audio.c:100-104
t2audio->reg_mem_bs_dma = pci_resource_start(dev, 0);  // BAR0 physical address
t2audio->reg_mem_bs = pci_iomap(dev, 0, 0);            // BAR0 mapped (buffer struct region)
t2audio->reg_mem_cfg = pci_iomap(dev, 4, 0);           // BAR4 mapped (config registers)

t2audio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) t2audio->reg_mem_cfg + 0xC000);
```

**BAR0**: Buffer memory region (DMA buffers, metadata)  
**BAR4**: Configuration registers  
**BAR4+0xC000**: GPR (General Purpose Registers) - pointer to BufferStruct

### Windows (AppleAudio.sys)

```c
// Функция 0x16FF0 (реконструкция)
// Итерация по CM_RESOURCE_LIST, фильтр по type == 3 (CmResourceTypeMemory)
// Первый memory-ресурс:
MmMapIoSpaceEx(phys1, size1, 0x404);  // -> [rbx+0x68]  // Cached|WriteCombined
// Второй memory-ресурс:
MmMapIoSpaceEx(phys2, size2, 0x204);  // -> [rbx+0x70]  // NonCached|WriteCombined

// Функция 0x45F0: запись в глобальный объект 0x14690
[global+0x28] = [rbx+0x68]              // BAR0 mapped
[global+0x30] = [rbx+0x70] + 0xC000     // BAR4+0xC000 mapped
```

**Соответствие**:

| Linux                  | Windows                        | Назначение                          |
|------------------------|--------------------------------|-------------------------------------|
| `reg_mem_bs` (BAR0)    | `[global+0x28]` ([rbx+0x68])   | Buffer memory (DMA buffers)         |
| `reg_mem_gpr` (BAR4+0xC000) | `[global+0x30]` ([rbx+0x70]+0xC000) | GPR region (BufferStruct pointer) |

---

## 2. GPR Structure (BAR4+0xC000)

### Linux (KAIT2EN)

```c
// audio.c:581-592
ver = ioread32(&a->reg_mem_gpr[0]);          // GPR[0] = version (должно быть >= 3)
sig = ioread32(&a->reg_mem_gpr[1]);          // GPR[1] = signature (0x19870423)
bs_base = ioread32(&a->reg_mem_gpr[2]);      // GPR[2] = offset BufferStruct относительно BAR0
a->bs = (struct t2audio_buffer_struct *) ((u8 *) a->reg_mem_bs + bs_base);
```

**GPR layout**:
```
Offset  Size  Field
------  ----  -----------------------
0x00    4     Version (>= 3)
0x04    4     Signature (0x19870423)
0x08    4     BufferStruct offset (relative to BAR0)
0x0C    ?     (reserved/unknown)
```

### Windows (AppleAudio.sys)

```c
// Функция 0x4718-0x4750
rax = [rcx+0x30];                          // rcx = global @ 0x14690
                                           // rax = BAR4+0xC000 (GPR region)
if ([rax+4] != 0x19870423) return ERROR;   // magic check
if ([rax+0] < 2) return ERROR;             // version check (>= 2, не >= 3!)
ebx = [rax+8];                             // offset буфера
rbx = [rcx+0x28] + ebx;                    // rbx = BAR0 + offset
```

**Соответствие GPR**:

| Offset | Linux field     | Windows check (RVA 0x4728-0x4744) | Match |
|--------|-----------------|-----------------------------------|-------|
| +0x00  | version         | `[rax]` >= 2                      | ✓ (разные минимальные версии) |
| +0x04  | signature       | `[rax+4]` == 0x19870423           | ✓     |
| +0x08  | bs_base         | `[rax+8]` = buffer offset         | ✓     |

**Различие**: Linux требует version >= 3, Windows >= 2.

---

## 3. BufferStruct (на BAR0 по смещению из GPR[2])

### Linux (KAIT2EN)

```c
// audio.h:27-51
struct t2audio_buffer_struct_buffer {
    size_t address;        // +0x00: offset относительно BAR0
    size_t size;           // +0x08: размер буфера
    size_t pad[4];         // +0x10: padding
};  // sizeof = 0x30 (48 bytes)

struct t2audio_buffer_struct_stream {
    u8 num_buffers;                                    // +0x00
    struct t2audio_buffer_struct_buffer buffers[100];  // +0x01
    char filler[32];                                   // stride compensation
};  // stride ~= 0x12E0

struct t2audio_buffer_struct_device {
    char name[128];                                     // +0x000: device UID (e.g. "Speaker")
    u8 num_input_streams;                               // +0x080
    u8 num_output_streams;                              // +0x081
    struct t2audio_buffer_struct_stream input_streams[5];   // +0x082
    struct t2audio_buffer_struct_stream output_streams[5];  // offset varies
    char filler[128];
};  // stride ~= 0xBDEC (Linux measurement from 0x4757: imul rax, r12, 0xBDEC)

struct t2audio_buffer_struct {
    u32 version;                                        // +0x00
    u32 signature;                                      // +0x04 = 0x19870423
    u32 flags;                                          // +0x08
    u8 num_devices;                                     // +0x0C
    struct t2audio_buffer_struct_device devices[20];    // +0x10
};
```

### Windows (AppleAudio.sys)

```c
// Функция 0x4744-0x4750 (реконструкция из дизассемблирования)
ebx = [rax+8];                          // rax = GPR region
                                        // ebx = offset BufferStruct
rbx = [rcx+0x28] + ebx;                 // rbx = BAR0 + offset = BufferStruct base

// Структура BufferStruct (частично восстановленная):
[rbx+0x00]  ?     version (проверка: >= 2 на 0x473B)
[rbx+0x04]  u32   signature = 0x19870423 (проверка на 0x4728)
[rbx+0x08]  ?     offset (используется на 0x4744)
[rbx+0x0C]  u8    count (используется на 0x474D)
[rbx+0x10+] ?     массив структур размером 0xBDEC (0x475B: imul rax, r12, 0xBDEC)
```

**Соответствие BufferStruct**:

| Offset | Linux field       | Windows usage (RVA 0x4728-0x4750) | Match |
|--------|-------------------|-----------------------------------|-------|
| +0x00  | version           | Проверка `[rbx] >= 2` (0x473B)    | ✓     |
| +0x04  | signature         | Проверка `[rbx+4] == 0x19870423`  | ✓     |
| +0x08  | flags (Linux)     | `[rbx+8]` используется как offset | ⚠ РАЗНИЦА |
| +0x0C  | num_devices       | `[rbx+0xC]` = count (0x474D)      | ✓     |
| +0x10+ | devices[]         | Массив структур stride 0xBDEC     | ✓     |

**КРИТИЧЕСКОЕ РАЗЛИЧИЕ**: 
- **Linux**: offset BufferStruct хранится в **GPR[2]** (BAR4+0xC000+8)
- **Windows**: читает offset из **BufferStruct[0x08]** (field `flags` в Linux-структуре)

**Возможные объяснения**:
1. Windows использует **другую версию протокола** (version 2 vs 3)
2. Field +0x08 имеет **двойное назначение** (flags + offset)
3. Код функции 0x4744 читает **не BufferStruct напрямую**, а другую структуру на BAR4+0xC000

---

## 4. Device Entry Structure (BufferStruct.devices[])

### Linux (KAIT2EN)

```c
// audio.h:37-44
struct t2audio_buffer_struct_device {
    char name[128];                      // +0x000: "Speaker", "Microphone", etc.
    u8 num_input_streams;                // +0x080
    u8 num_output_streams;               // +0x081
    struct t2audio_buffer_struct_stream input_streams[5];
    struct t2audio_buffer_struct_stream output_streams[5];
    char filler[128];
};
// stride = 0xBDEC (48620 bytes)
```

### Windows (AppleAudio.sys)

```c
// Функция 0x4757-0x4762
lea rdx, [rbx+0x10]                     // rdx = base структуры (rbx = BufferStruct)
imul rax, r12, 0xBDEC                   // rax = index * 0xBDEC
add rdx, rax                            // rdx = devices[index]
mov r8d, 0x80                           // size = 0x80 (128 bytes)
lea rcx, [r14+0x21]                     // rcx = dest
call 0x4CC0                             // memcpy(dest, devices[i]+?, 128)
```

**Stride 0xBDEC**: ✓ СОВПАДАЕТ с Linux

**Windows копирует 128 байт** → вероятно копируется поле `name[128]` (+0x00 device entry)

---

## 5. Stream Buffer Access (PCM data path)

### Linux (KAIT2EN)

```c
// audio.c:710-732 (t2audio_init_bs_stream)
for (i = 0; i < strm->buffer_cnt; i++) {
    strm->buffers[i].dma_addr = a->reg_mem_bs_dma + bs_strm->buffers[i].address;
    strm->buffers[i].ptr = a->reg_mem_bs + bs_strm->buffers[i].address;
    strm->buffers[i].size = bs_strm->buffers[i].size;
    strm->buffers[i].type = T2AUDIO_DMA_BUF_IOMEM;
}

// pcm.c:224-226 (t2audio_pcm_hw_params)
// ALSA напрямую использует mapped-память T2:
substream->runtime->dma_area = astream->buffers[0].ptr;      // CPU pointer
substream->runtime->dma_addr = astream->buffers[0].dma_addr; // DMA address
substream->runtime->dma_bytes = astream->buffers[0].size;
```

**Механизм**: ALSA **прямо пишет в mapped BAR0 память** (zero-copy).

**Путь данных Linux**:
```
User space PCM write
    ↓
ALSA kernel copy_to_user
    ↓
substream->runtime->dma_area (= mapped BAR0 + offset)
    ↓
T2 hardware читает из своей памяти (shared memory, не DMA transfer)
```

### Windows (AppleAudio.sys)

**НЕ НАЙДЕНО**: механизм записи PCM-сэмплов в mapped-буфер.

**Найденные операции с mapped-памятью**:
1. **Функция 0x16FF0**: инициализация mapped-адресов (startup)
2. **Функция 0x4700**: **чтение метаданных** из BufferStruct (не запись PCM)
3. **Функция 0x45F0**: сохранение mapped-адресов в глобальный объект
4. **347 обращений** к `[global+0x28]` и `[global+0x30]` в коде

**Функция 0x4700-0x4750 выполняет**:
```
1. Читает GPR[2] (offset BufferStruct)
2. Проверяет magic/version BufferStruct
3. Итерирует по devices[] (stride 0xBDEC)
4. Копирует device name (128 bytes) в локальный буфер
```

**Это НЕ PCM data path** — это чтение метаданных устройства.

---

## 6. Missing Piece: Windows PCM Write Path

### Что должно существовать (по аналогии с Linux):

```c
// Гипотетический Windows-код (НЕ НАЙДЕН в AppleAudio.sys)
void WritePCMSamples(DEVICE_CONTEXT* ctx, void* samples, size_t size) {
    void* mapped_buffer = ctx->global->bar0_mapped;  // [global+0x28]
    size_t offset = GetCurrentBufferOffset(ctx);
    
    // Прямая запись в mapped memory:
    memcpy((u8*)mapped_buffer + offset, samples, size);
    // или через write-combining stores для производительности
}
```

### Почему не найдено:

**Гипотеза 1**: Использует **PortCls framework**
- KS pin callback (WaveSpeaker) предоставляет буфер через `IMiniportWaveRTStream::GetBuffer()`
- PortCls копирует данные напрямую через mapped pointer
- Копирование происходит в **portcls.sys**, не в AppleAudio.sys

**Гипотеза 2**: Использует **WaveRT shared memory mode**
- Mapped BAR0 регион экспортируется как WaveRT buffer
- User-mode приложение пишет напрямую через memory mapping
- Никакой kernel-side копии не требуется

**Гипотеза 3**: Dispatch через **vtable callback** (не найдено xref)
- Функция 0x16FF0 вызывается через interface pointer (context->vtable[N])
- PCM write также через vtable, но на другой интерфейс
- Статический анализ не находит indirect calls

---

## 7. Сравнительная таблица: полная реконструкция

| Компонент | Linux (KAIT2EN) | Windows (AppleAudio.sys) | Status |
|-----------|-----------------|--------------------------|--------|
| **BAR0 mapping** | `pci_iomap(dev, 0)` | `MmMapIoSpaceEx(phys1, size1, 0x404)` | ✓ FOUND |
| **BAR4 mapping** | `pci_iomap(dev, 4)` | `MmMapIoSpaceEx(phys2, size2, 0x204)` | ✓ FOUND |
| **GPR access** | `reg_mem_cfg + 0xC000` | `[global+0x30]` (= BAR4+0xC000) | ✓ FOUND |
| **GPR[0] version** | `ioread32(&gpr[0])` >= 3 | `[rax]` >= 2 | ✓ FOUND (разные версии) |
| **GPR[1] signature** | `ioread32(&gpr[1])` == 0x19870423 | `[rax+4]` == 0x19870423 | ✓ FOUND |
| **GPR[2] bs_offset** | `ioread32(&gpr[2])` | `[rax+8]` (?) | ⚠ UNCERTAIN |
| **BufferStruct base** | `BAR0 + GPR[2]` | `BAR0 + [GPR+8]` | ⚠ UNCERTAIN |
| **BufferStruct.signature** | `bs->signature` == 0x19870423 | `[rbx+4]` == 0x19870423 | ✓ FOUND |
| **BufferStruct.num_devices** | `bs->num_devices` | `[rbx+0xC]` | ✓ FOUND |
| **Device entry stride** | 0xBDEC | 0xBDEC | ✓ FOUND |
| **Device name copy** | `strcpy(dev->name)` | `memcpy(dest, src, 0x80)` | ✓ FOUND |
| **Stream buffer setup** | `strm->buffers[i].ptr = BAR0 + offset` | ? | ✗ NOT FOUND |
| **ALSA DMA area** | `runtime->dma_area = buffers[0].ptr` | ? | ✗ NOT FOUND |
| **PCM write** | ALSA → dma_area (zero-copy) | ? | ✗ NOT FOUND |

---

## 8. Недостающий механизм в Windows

### Что НЕ НАЙДЕНО:

1. **Функция, которая читает stream buffer offsets** из `BufferStruct.devices[i].output_streams[j].buffers[k]`
2. **Функция, которая копирует PCM-сэмплы** в `BAR0 + buffer_offset`
3. **WaveRT buffer registration**: вызов `portcls!PcRegisterWaveRTBuffer` или аналог
4. **KS pin SetFormat callback**: настройка stream buffer адреса
5. **Direct write** через mapped pointer в IRQ/DPC handler

### Где искать дальше:

1. **Consumers глобального объекта 0x14690**: 347 обращений к полям +0x28/+0x30
   - Разобрать функции, читающие `[global+0x28]` (BAR0 base)
   - Найти функции с арифметикой: `BAR0 + offset` + запись данных

2. **PortCls callbacks**: проверить vtable-dispatch функций
   - `IMiniportWaveRTStream::AllocateAudioBuffer`
   - `IMiniportWaveRTStream::GetHWLatency`
   - `IMiniportWaveCyclic::NewStream`

3. **Функции с параметрами audio format** (sample rate, channels, bit depth)
   - Найти код, вычисляющий buffer size = `bytes_per_sample * channels * frame_count`
   - Проследить куда передаётся вычисленный size

4. **DMA/MMIO write loops**: паттерны типа
   ```asm
   mov [rdi], eax
   add rdi, 4
   loop ...
   ```

5. **Runtime debugging**:
   - Breakpoint на `[global+0x28]` read access
   - Trace callstack функций, использующих BAR0 pointer
   - Monitor memory writes в BAR0 region во время audio playback

---

## 9. Практический вывод: Windows implementation strategy

### Вариант A: Reverse engineer существующий интерфейс Apple

**Pros**:
- Переиспользование существующих WDF/PortCls компонентов
- Совместимость с существующими user-mode компонентами (если есть)

**Cons**:
- **Блокер**: PCM write path не найден статическим анализом
- Требуется runtime debugging (WinDbg, kernel breakpoints)
- Может зависеть от закрытых WDF/PortCls внутренних API
- Сложность обратной инженерии vtable dispatch и PortCls callbacks

**Оценка трудоёмкости**: 
- 40-80 часов runtime debugging
- 20-40 часов реимплементации после понимания протокола
- Риск: может оказаться, что критичная логика в firmware T2 (недоступна)

### Вариант B: Портировать Linux-логику на Windows

**Pros**:
- **Полный исходный код** Linux-драйвера доступен
- **Проверенный протокол**: работает на реальном hardware
- Прямой контроль над всей цепочкой (BAR mapping → PCM write)
- Можно адаптировать только audio-часть (не нужен весь T2BCE stack)

**Cons**:
- Необходимо переписать ALSA-часть на WaveRT/WaveCyclic
- Портирование Linux DMA API → Windows (IoAllocateMdl, MmMapLockedPages)
- Портирование workqueue/spinlock → Windows (KDPC, KeAcquireSpinLock)
- Тестирование на реальном hardware (риск повреждения устройства)

**Оценка трудоёмкости**:
- 20-40 часов портирования core audio logic (pcm.c → Windows)
- 10-20 часов адаптации WaveRT interface
- 10-20 часов отладки на реальном hardware
- Риск: меньше, т.к. логика известна

### Вариант C: Гибридный подход (рекомендуется)

**Стратегия**:
1. **Взять из Linux** (проверенное):
   - BufferStruct layout и парсинг (audio.c:574-642)
   - Stream buffer setup (audio.c:710-741)
   - PCM timing logic (pcm.c:150-160, timestamp handling)

2. **Взять из AppleAudio.sys** (уже работает):
   - BAR mapping через MmMapIoSpaceEx (функция 0x16FF0)
   - Global object structure (0x14690)
   - Device initialization sequence (function call order)

3. **Написать заново** (минимально):
   - WaveRT buffer registration:
     ```c
     NTSTATUS SetupWaveRTBuffer(DEVICE_CTX* ctx) {
         // Используем mapped BAR0 pointer из AppleAudio.sys
         void* bar0 = ctx->global->bar0_mapped;
         size_t offset = GetStreamBufferOffset(ctx);  // из BufferStruct
         
         // Экспортируем в WaveRT как user-accessible memory
         MmMapIoSpace(bar0_phys + offset, size, MmNonCached);
     }
     ```

   - PCM write callback (из Linux logic):
     ```c
     void WritePCMData(STREAM_CTX* stream, void* samples, size_t frames) {
         void* dma_area = stream->buffer_ptr;  // mapped BAR0 + offset
         size_t bytes = frames * stream->bytes_per_frame;
         
         // Direct write (как в Linux pcm.c:224)
         memcpy(dma_area, samples, bytes);
         wmb();  // write barrier
     }
     ```

**Оценка трудоёмкости**:
- 10 часов: изучение существующего AppleAudio.sys device init
- 15 часов: портирование BufferStruct parsing из Linux
- 20 часов: реализация WaveRT interface поверх mapped memory
- 15 часов: тестирование и debugging
- **ИТОГО: ~60 часов** (realistic estimate для опытного Windows kernel developer)

---

## 10. Конкретные рекомендации

### Immediate next steps (runtime analysis):

1. **WinDbg session на live system**:
   ```
   kd> bp AppleAudio+0x16FF0  ".echo MmMapIoSpaceEx init; g"
   kd> ba r8 AppleAudio!global_object+0x28  "!analyze -v; kv"
   ```
   Цель: поймать функции, читающие BAR0 pointer после инициализации

2. **Memory monitoring во время playback**:
   ```
   kd> !address <BAR0_address>
   kd> ba w4 <BAR0_address>+<offset>  "kv; g"
   ```
   Цель: найти функцию, пишущую PCM-данные

3. **PortCls trace**:
   ```
   kd> !devnode 0 1 AppleAudio
   kd> !devstack <PDO>
   kd> bp portcls!PcNewMiniport
   kd> bp portcls!PcRegisterSubdevice
   ```
   Цель: перехватить PortCls callbacks и vtable setup

### If runtime analysis blocked (no test hardware):

**Рекомендация**: Портировать Linux-логику.

**Minimal viable implementation**:
1. Скопировать `t2audio_buffer_struct` определения из audio.h
2. Портировать `t2audio_init_bs()` (audio.c:574-642) на Windows
3. Портировать `t2audio_init_bs_stream()` (audio.c:710-741)
4. Написать WaveRT miniport с direct memory access:
   ```c
   runtime->dma_area = mapped_bar0 + stream_offset;  // Linux pcm.c:224
   ```
5. Минимум протокола: только playback, только Speaker device

**Expected result**: Proof-of-concept драйвер, воспроизводящий аудио через T2 без полного протокола.

**Risk mitigation**: Начать с read-only операций (enumerate devices, read BufferStruct), не трогать запись в BAR0 до понимания протокола.

---

## 11. Final verdict

| Критерий | Apple interface RE | Linux port | Hybrid |
|----------|-------------------|------------|--------|
| Трудоёмкость | 100-160 ч | 60-80 ч | **40-60 ч** |
| Риск (hardware) | Средний | Высокий | Средний |
| Риск (software) | Высокий | Низкий | **Низкий** |
| Полнота функционала | 100% (если найдено) | 90% (audio only) | **80%** (достаточно) |
| Maintainability | Низкая (black box) | Средняя | **Высокая** |

**Рекомендация**: **Вариант C (Гибридный)**

**Причины**:
1. Linux-код даёт **гарантированно работающую логику** работы с BufferStruct
2. AppleAudio.sys даёт **проверенную инициализацию** BAR mapping на Windows
3. Комбинация минимизирует **reverse engineering effort** и **implementation risk**
4. Можно начать с **safe read-only operations** (enumerate, parse metadata)
5. PCM write — простая операция `memcpy()` после setup, не требует сложного RE

**Next action**: 
1. Портировать `t2audio_buffer_struct` definitions → Windows headers
2. Написать BufferStruct parser на базе Linux audio.c:574-642
3. Создать minimal WaveRT miniport с mapped memory (без PortCls abstraction слоя)
4. Тестировать на виртуальной машине с mock hardware (если нет реального MacBook)
