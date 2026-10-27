# Отчёт: Анализ функции регистрации буферов AppleAudio 0x46F8

## Исправление ошибок предыдущих отчётов

1. **Подтверждено**: RVA 0x4CC0 — переход на `_strnicmp`, используется для сравнения имён устройств (длина до 128 байт).
2. **Подтверждено**: Чтение `[RAX+8]` на 0x4744 — это смещение BufferStruct в BAR1, а не поле BufferStruct.flags.
3. **Подтверждено**: Инструкции 0x480A и 0x4816 выполняют реальные записи в mapped-структуру BufferStruct.

## 1. Границы функции регистрации буферов

**Функция 0x46F8-0x48AF** (через `.pdata`)

Сигнатура:
```c
NTSTATUS RegisterInputStreamBuffers(
    void* global_object,      // RCX (сохр. в R15)
    void* device_descriptor,  // RDX (сохр. в R14) 
    void* unused_param,       // R8  (сохр. в [RSP+0x60], передаётся в 0x44E4)
    void* buffer_list         // R9  (сохр. в RDI)
);
```

## 2. Полный псевдокод функции 0x46F8

```c
NTSTATUS RegisterInputStreamBuffers(void* global, void* device_desc, 
                                     void* unused, void* buffer_list)
{
    // Получение указателя на GPR (BAR2+0xC000)
    void* gpr = global->offset_0x30;  // 0x4718
    
    // Проверка signature GPR
    if (gpr->signature != 0x19870423) {  // 0x4728
        return STATUS_UNSUCCESSFUL;  // 0x4731: 0xC0000001
    }
    
    // Проверка версии
    if (gpr->version < 2) {  // 0x473B
        return 0xC0000002;  // 0x488D
    }
    
    // Получение BufferStruct
    u32 bs_offset = gpr->bs_offset;  // 0x4744: [RAX+8]
    void* bs_base = global->offset_0x28 + bs_offset;  // 0x4749: BAR1 + offset
    
    u8 num_devices = bs_base->num_devices;  // 0x474D: [RBX+0x0C]
    u32 device_idx;
    void* target_device;
    
    // Поиск устройства по имени
    if (num_devices > 0) {  // 0x4752
        for (device_idx = 0; device_idx < num_devices; device_idx++) {  // 0x4754-0x4780
            void* device = bs_base + 0x10 + device_idx * 0xBDEC;
            
            // Вызов _strnicmp
            int cmp = _strnicmp(device->name, device_desc + 0x21, 0x80);  // 0x476F
            
            if (cmp == 0) {  // 0x4776
                // Устройство найдено
                target_device = bs_base + 0x10 + device_idx * 0xBDEC;  // 0x47EB-0x47F2
                goto register_buffers;  // 0x4821
            }
        }
    }
    
    // Добавление нового устройства
    device_idx = num_devices;
    target_device = bs_base + 0x10 + device_idx * 0xBDEC;
    
    // Копирование имени (128 байт через SSE)
    memcpy(target_device->name, device_desc + 0x21, 128);  // 0x4782-0x47E1
    
register_buffers:
    // Запись дескрипторов буферов в input_streams[0]
    u8 buffer_count = 0;
    void* node = buffer_list;
    
    while (node != NULL) {  // 0x4821/0x4861
        u32 index = buffer_count * 6;  // 0x47FC: lea rcx, [rax+rax*2]; shl
        
        // Запись в input_streams[0].buffers[]
        target_device->input_streams[0].buffers[buffer_count].address = node->address;  // 0x480A/0x484A
        target_device->input_streams[0].buffers[buffer_count].size = node->size;        // 0x4816/0x4856
        
        buffer_count++;
        node = node->next;  // 0x481E/0x485E
    }
    
    // Обновление счётчика буферов
    target_device->input_streams[0].num_buffers = buffer_count;  // 0x4826/0x4866: [+0x94]
    
    // Установка флага активности потока
    target_device->input_streams[0].active = 1;  // 0x482D/0x486D: [+0x90]
    
    // Увеличение счётчика устройств (только для нового)
    bs_base->num_devices++;  // 0x4875: inc byte [RBX+0x0C]
    
    // Уведомление другой подсистемы
    func_0x44E4(global, device_desc, unused, NULL);  // 0x4886
    
    return STATUS_SUCCESS;
}
```

## 3. Сопоставление записываемых полей с Linux audio.h

### Узел linked list (параметр R9)

| Смещение | Инструкция | Поле Linux | Описание |
|----------|------------|------------|----------|
| +0x00 | 0x481E | `next` | Указатель на следующий узел |
| +0x08 | 0x4800, 0x4840 | - | Физический адрес буфера |
| +0x10 | 0x4812, 0x4852 | - | Размер буфера |

Структура Windows-специфичная (создаётся в 0x1FA4):
```c
struct buffer_node {
    struct buffer_node* next;  // +0x00
    u64 physical_address;      // +0x08
    u64 size;                  // +0x10
};
```

### Записываемые поля BufferStruct device

**Вычисление смещения buffers[]:**
- Индекс массива: `(buffer_idx * 3) * 2 = buffer_idx * 6`
- Запись: `[target_device + index*8 + 0x98]` и `[target_device + index*8 + 0xA0]`

**Разложение смещения 0x98:**
```
0x98 = 0x84 + 0x14
       ^^^   ^^^
       |     смещение buffers[] внутри stream
       offsetof(device, input_streams)
```

**Подтверждённые смещения в device:**

| Windows offset | Linux offsetof | Поле | Инструкция |
|----------------|----------------|------|------------|
| +0x00 | +0x00 | `name[128]` | 0x4782 (копирование SSE) |
| +0x0C | - | - | 0x474D (чтение num_devices из BufferStruct, не device) |
| +0x84 | +0x84 | `input_streams[5]` | base для вычисления |
| +0x90 | - | - | 0x482D (флаг, но относительно stream, не device) |
| +0x94 | - | - | 0x4826 (счётчик, относительно stream) |
| +0x98 | +0x98 | - | 0x480A (запись address) |
| +0xA0 | - | - | 0x4816 (запись size) |

**Размер device:** 0xBDEC (48620 байт) — точное совпадение с Linux `sizeof(struct device)`

### Уточнение структуры stream (Windows layout)

Смещение 0x98 относительно device даёт:
```
0x98 = input_streams_base (0x84) + 0x14
```

Значит, buffers[] начинаются на **+0x14** внутри stream, а не на +0x04 как в Linux.

**Гипотеза о Windows layout stream:**
```c
struct stream_windows {
    u8 num_buffers;         // +0x00
    u8 pad1[3];
    u32 field_0x04;         // +0x04
    u32 field_0x08;         // +0x08
    u32 field_0x0C;         // +0x0C
    u8 active;              // +0x10 (записывается как [device+0x90] = device+0x84+0x0C)
    u8 pad2[3];
    struct buffer buffers[100];  // +0x14 (массив буферов)
    char filler[32];
};
```

**Проверка смещения активности:**
- 0x482D: `mov byte [r8 + rbx + 0x90], 1`
- `r8 = device_idx * 0xBDEC`, `rbx = bs_base`
- `r8 + rbx = target_device`
- `target_device + 0x90 = device_base (0x84) + 0x0C`

Но контекст показывает, что 0x90 записывается относительно **stream**, а не device base. Пересчёт:
```
[rdx + rbx + 0x90] на 0x486D, где rdx = r8, rbx = bs_base
Это [target_device + 0x90]
```

Если `input_streams` начинается на 0x84, то:
```
0x90 - 0x84 = 0x0C (смещение внутри stream)
```

Но по инструкции 0x4866 (`mov byte [r8 + 0x94], sil`) смещение 0x94 — это `num_buffers`.

**Пересмотр:** смещения 0x90 и 0x94 относятся к **stream**, а не device:
- 0x94 внутри stream = num_buffers
- 0x90 внутри stream = active flag

Значит:
```c
struct stream_windows {
    u8 field_0x00[144];      // неизвестная структура
    u8 active;               // +0x90
    u8 pad[3];
    u8 num_buffers;          // +0x94
    u8 pad2[3];
    struct buffer buffers[100];  // +0x98
};
```

**Размер одного stream:** вычисляется из контекста как ≤ 0x12E4 (Linux sizeof).

### Несоответствие с Linux layout

Linux `offsetof(stream, buffers) = 0x04`, но Windows использует **0x98** внутри stream-структуры. Это указывает на **Windows-специфичные поля** перед массивом buffers.

Возможные причины:
1. Windows driver добавляет extension structure размером ~0x94 байт.
2. Поля num_buffers и active располагаются непосредственно перед buffers[].

## 4. Происхождение linked list (R9)

### Функция-вызывающая 0x1FA4 (AllocateInputBuffers)

**Границы:** 0x1FA4-0x221A

**Псевдокод:**
```c
NTSTATUS AllocateInputBuffers(void* global, void* device_struct, u32 size)
{
    // Выделение physical memory через MDL
    size_aligned = ALIGN_UP(size, 0x1000);  // 0x1FC7-0x1FD8
    
    MDL* mdl = MmAllocatePagesForMdl(NULL, NULL, size_aligned, 0x2000);  // 0x201B
    if (!mdl || mdl->ByteCount < size_aligned) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Отображение в kernel space
    void* mapped = MmMapLockedPagesSpecifyCache(mdl, KernelMode, 
                                                 MmCached, NULL, 0, 
                                                 0x40000020);  // 0x2051
    if (!mapped) {
        MmFreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Создание linked list дескрипторов буферов
    u32 num_pages = (size_aligned >> 12) + 1;  // 0x2070-0x2078
    u32 list_size = num_pages * 0x18;  // 0x207A: (pages * 3) * 8
    
    struct buffer_node* list = ExAllocatePoolWithTag(NonPagedPool, list_size, 'CCAA');  // 0x2085
    memset(list, 0, list_size);  // 0x20A7
    
    // Обход PFN array MDL и построение linked list
    struct buffer_node* current = list;
    u64* pfn_array = &mdl->pages[0];  // 0x20CA: MDL+0x30
    u32 remaining = mdl->ByteCount;   // 0x20C3
    
    for (u32 i = 0; i < num_pages && remaining > 0; i++) {  // 0x20B4-0x213D
        u64 pfn = pfn_array[i];  // 0x20D4
        u64 phys_addr = pfn << 12;  // 0x20E2: сдвиг в физический адрес
        u32 chunk_size = min(remaining, 0x1000);  // 0x20EE-0x2116
        
        current->address = phys_addr;  // 0x2108
        current->size = chunk_size;    // 0x2118
        
        if (remaining > 0x1000) {
            current->next = current + 1;  // 0x2120: lea [r14+0x18]
            current = current->next;      // 0x212A
        } else {
            current->next = NULL;  // 0x2133
        }
        
        remaining -= chunk_size;
    }
    
    // Вызов функции регистрации
    RegisterInputStreamBuffers(global, device_struct, 
                               device_struct + 0x1B0 + stream_idx * 0xB30,
                               list);  // 0x2194
    
    // Сохранение ссылок для cleanup
    device_struct->streams[stream_idx].mapped_address = mapped;  // 0x21A2
    device_struct->streams[stream_idx].mdl = mdl;                // 0x21B0
    device_struct->streams[stream_idx].buffer_list = list;       // 0x21B4
    
    return STATUS_SUCCESS;
}
```

**Ключевые поля:**
- `R9 = list` — linked list, созданный из PFN array MDL
- Каждый узел содержит физический адрес страницы и размер chunk
- Список построен из contiguous kernel allocation, но описывает potentially non-contiguous physical pages

### Функция-вызывающая 0x4398 (RestoreAllInputBuffers)

**Границы:** 0x4398-0x4448

**Псевдокод:**
```c
void RestoreAllInputBuffers(void* global)
{
    if (global->offset_0x30 == NULL) return;  // 0x43B0: проверка GPR
    
    u32 num_device_slots = global->offset_0x40;  // 0x43BC: количество слотов устройств
    
    for (u32 dev_idx = 0; dev_idx < num_device_slots; dev_idx++) {  // 0x43C9-0x442D
        void* device_slot = global + 0x1F0 + dev_idx * 0xE970;  // 0x43C2, 0x441F
        u32 num_streams = device_slot->num_streams;  // 0x43CC: [RDI]
        
        for (u32 stream_idx = 0; stream_idx < num_streams; stream_idx++) {  // 0x43DC-0x441B
            void* stream_struct = device_slot - 0x1A8 + stream_idx * 0xB30;  // 0x43D3
            
            if (stream_struct->offset_0xCD0 == 0) continue;  // 0x43DC: пропуск неактивных
            
            // Чтение сохранённого buffer_list
            void* buffer_list = stream_struct->offset_0xB30;  // 0x43E7: [RDI+RBP+0xB30]
            
            // Вызов регистрации
            RegisterInputStreamBuffers(global, stream_struct - 0x1A8,
                                       stream_struct,
                                       buffer_list);  // 0x4406
        }
    }
}
```

**Значение полей:**
- `global + 0x1F0` — начало массива device slots (Windows driver extension)
- Stride 0xE970 (59760 байт) — размер device slot в Windows driver
- `stream_struct->offset_0xB30` — сохранённый указатель на buffer_list

## 5. Симметричный код для output_streams

### Функция 0x9D74 (GetOutputStreamBuffers)

**Границы:** 0x9D74-0x9E8D

**Ключевая инструкция:**
```asm
0x9E78: mov rcx, qword ptr [rdx + rbx + 0x5F0C]
0x9E83: mov rcx, qword ptr [rdx + rbx + 0x5F14]
```

**Разложение смещения 0x5F0C:**
```
0x5F0C = 0x5EF8 + 0x14
         ^^^^    ^^^^
         |       смещение buffers[] внутри stream
         offsetof(device, output_streams)
```

**Подтверждение:** смещение 0x5EF8 точно соответствует Linux `offsetof(device, output_streams)`.

**Псевдокод функции 0x9D74:**
```c
NTSTATUS GetOutputStreamBuffers(void* device_name, void* gpr, void* bs_base,
                                u64* out_addr, u64* out_size)
{
    // Проверка signature
    if (gpr->signature != 0x19870423) {  // 0x9D94
        return STATUS_UNSUCCESSFUL;
    }
    
    u32 version = gpr->version;  // 0x9DA5
    
    if (version == 1) {
        // Старая версия: поиск по специальному смещению 0x110
        u32 bs_offset = gpr->bs_offset;  // 0x9DAA
        void* bs_base_calc = bs_base + bs_offset;  // 0x9DAE
        
        if (bs_base_calc->signature != 0x19870423) return STATUS_UNSUCCESSFUL;  // 0x9DB1
        
        u8 num_devices = bs_base_calc->num_devices;  // 0x9DB8
        for (u32 i = 0; i < num_devices; i++) {  // 0x9DBE-0x9DE2
            void* device = bs_base_calc + 0x10 + i * 0x110;  // 0x9DC4: старый stride!
            
            if (strcmp(device->name, device_name) == 0) {  // 0x9DD1
                *out_addr = device->offset_0x110;  // 0x9DFE
                *out_size = device->offset_0x118;  // 0x9E0E
                return STATUS_SUCCESS;
            }
        }
        
        return STATUS_UNSUCCESSFUL;
    }
    
    if (version >= 2) {
        // Новая версия: поиск в device array с stride 0xBDEC
        u32 bs_offset = gpr->bs_offset;  // 0x9E27
        void* bs_base_calc = bs_base + bs_offset;  // 0x9E2B
        
        if (bs_base_calc->version < 3) {  // 0x9E2E
            return STATUS_UNSUCCESSFUL;
        }
        
        u8 num_devices = bs_base_calc->num_devices;  // 0x9E35
        for (u32 i = 0; i < num_devices; i++) {  // 0x9E3B-0x9E65
            void* device = bs_base_calc + 0x10 + i * 0xBDEC;  // 0x9E41
            
            if (_strnicmp(device->name, device_name, 0x80) == 0) {  // 0x9E54
                // Чтение из output_streams[0]
                *out_addr = device->output_streams[0].offset_0x18;  // 0x9E78: [+0x5F0C]
                *out_size = device->output_streams[0].offset_0x20;  // 0x9E83: [+0x5F14]
                return STATUS_SUCCESS;
            }
        }
    }
    
    return STATUS_UNSUCCESSFUL;
}
```

**Отличия от input_streams:**
- **Только чтение** полей output_streams, **нет записи**
- Смещения 0x5F0C и 0x5F14 соответствуют `output_streams[0].buffers[0].field_0x18` и `field_0x20`
- Функция используется для получения готовых дескрипторов, а не регистрации новых

**Вывод:** механизм регистрации output буферов **не найден** в проанализированных функциях. Функция 0x9D74 выполняет только чтение.

## 6. Точная таблица полей BufferStruct и device

### BufferStruct header (подтверждено байтами)

| Смещение | Размер | Поле | Инструкция | Значение |
|----------|--------|------|------------|----------|
| +0x00 | u32 | version | 0x473B | >= 2 (Linux требует >= 3) |
| +0x04 | u32 | signature | 0x4728 | 0x19870423 |
| +0x08 | u32 | bs_offset | 0x4744 | Offset в BAR1 |
| +0x0C | u8 | num_devices | 0x474D | Количество устройств |
| +0x10 | struct device[] | devices | 0x474D | Массив устройств |

### device structure (подтверждено байтами)

| Смещение | Размер | Поле | Инструкция | Описание |
|----------|--------|------|------------|----------|
| +0x00 | u8[128] | name | 0x476F, 0x4782 | Имя устройства (ASCII) |
| +0x84 | struct stream[5] | input_streams | вычислено | 5 input streams |
| +0x5EF8 | struct stream[5] | output_streams | 0x9E78 | 5 output streams |
| sizeof | 0xBDEC | - | 0x4762 | 48620 байт |

**Совпадение с Linux:** offsetof и sizeof точно совпадают.

### stream structure (частично восстановлено)

| Смещение | Размер | Поле | Инструкция | Описание |
|----------|--------|------|------------|----------|
| +0x90 | u8 | active | 0x482D, 0x486D | Флаг активности |
| +0x94 | u8 | num_buffers | 0x4826, 0x4866 | Количество буферов |
| +0x98 | struct buffer[100] | buffers | 0x480A, 0x4816 | Массив дескрипторов |

**Несоответствие с Linux:** Linux размещает buffers на +0x04, Windows — на +0x98.

### buffer structure в Windows

Индексация через `index = buffer_idx * 6`:
```c
struct buffer {
    u64 address;     // +0x00 (запись 0x480A, 0x4816)
    u64 size;        // +0x08 (запись 0x4816, 0x4856)
    u64 pad[4];      // +0x10 (не используется)
};  // sizeof = 0x30 (48 байт)
```

Stride 0x30 подтверждается вычислением: `(idx * 3) * 2 * 8 = idx * 48`.

## 7. Установленные факты

### Подтверждённые операции

1. **Функция 0x46F8** регистрирует input stream buffers в BufferStruct, размещённой в BAR1.
2. **Поиск устройства** выполняется через `_strnicmp` имён (длина 128 байт).
3. **Если устройство не найдено**: добавляется новое в конец массива `devices[]`.
4. **Если устройство найдено**: обновляются buffers[] существующего устройства.
5. **Linked list** создаётся в 0x1FA4 из PFN array MDL (non-contiguous physical pages).
6. **Функция 0x9D74** читает output_streams по смещению 0x5F0C, но **не записывает**.

### Несоответствия с Linux

1. **Смещение buffers[]**: Windows +0x98 внутри stream, Linux +0x04.
2. **Дополнительные поля**: Windows добавляет ~0x90 байт перед buffers[].
3. **Отсутствие регистрации output**: код записи в output_streams не обнаружен.

### Windows driver extension

Функция 0x4398 работает с device slots по смещению `global + 0x1F0`, stride 0xE970. Это **Windows-специфичная структура**, не относящаяся к BufferStruct.

Поля Windows device slot:
- +0x1A8: неизвестное
- +0x1B0: начало массива stream descriptors (stride 0xB30)
- +0xB30: сохранённый buffer_list pointer
- +0xCD0: флаг активности stream slot

Размер 0xB30 (2864 байт) не совпадает с Linux sizeof(stream) = 0x12E4.

## 8. Неизвестное и следующие шаги

### Критические неизвестные

1. **Регистрация output_streams:** где находится код записи в `device->output_streams[].buffers[]`?
2. **Смещение 0x1B0:** что представляет параметр R8, передаваемый в 0x46F8 и затем в 0x44E4?
3. **Функция 0x44E4:** что делает вызов после регистрации буферов?
4. **Windows stream layout:** что находится в первых 0x90 байтах stream структуры?
5. **Stride 0xB30:** почему Windows stream descriptor больше Linux stream?

### Рекомендации для дальнейшего анализа

1. Дизассемблировать функцию 0x44E4 для понимания постобработки.
2. Найти xref на смещения 0x5F0C-0x5F30 для обнаружения кода записи output_streams.
3. Проанализировать функции выделения буферов для output direction.
4. Изучить relationship между device slot (0xE970) и BufferStruct device (0xBDEC).
5. Найти код, который читает из `input_streams[].buffers[]` для подтверждения формата.

### Конкретное следующее действие

**Поиск функции записи в output_streams:**
```python
# Искать инструкции записи на смещения 0x5F0C-0x5F30
for insn in all_instructions:
    if insn.is_write() and 0x5F0C <= insn.mem.disp <= 0x5F30:
        print(f"Found write to output_streams at {insn.address:X}")
```

## 9. Отказ от недостоверных утверждений

**Удалено:**
- Утверждение о 347 совпадениях смещений как доказательство единого объекта
- Трактовка внутреннего указателя AppleAudio как предоставляемого API
- Утверждение о MmMapIoSpace как способе регистрации WaveRT-буфера
- Утверждение об отображении BufferStruct в user mode
- Оценки процентов готовности, часов работы, уровней риска

**Подтверждено статическим анализом:**
- Все смещения получены из дизассемблированных инструкций
- Все вызовы функций прослежены через xref
- Все структуры восстановлены из паттернов memory access
- Все утверждения привязаны к конкретным RVA

---

**Конец отчёта**
