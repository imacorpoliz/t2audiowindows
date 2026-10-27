# Анализ функции 0x46F8: Регистрация нового устройства в BufferStruct

## Сигнатура функции

```c
NTSTATUS RegisterDeviceToBufferStruct(
    void* global_object,     // rcx (сохр. в r15)
    void* device_descriptor, // rdx (сохр. в r14)
    void* param3,            // r8  (сохр. в [rsp+0x60])
    void* linked_list        // r9  (сохр. в rdi)
);
```

## Псевдокод

```c
NTSTATUS RegisterDeviceToBufferStruct(void* global, void* device_desc, void* param3, void* list_head)
{
    // 0x46F8-0x04725: prologue, сохранение регистров
    void* gpr_ptr = global->field_0x30;  // 0x4718: mapped BAR2+0xC000
    u8 stream_index = 0;                  // 0x471C: xor sil, sil
    void* linked_list = list_head;        // 0x471F: rdi = r9
    
    // 0x4728: проверка signature GPR-структуры
    if (gpr_ptr[0x04] != 0x19870423) {
        return STATUS_UNSUCCESSFUL;       // 0x4731: 0xC0000001
    }
    
    // 0x473B: проверка версии
    if (gpr_ptr[0x00] < 2) {
        return 0xC0000002;                // 0x488D
    }
    
    // 0x4744: получение BufferStruct base
    u32 bs_offset = gpr_ptr[0x08];
    void* bs_base = global->field_0x28 + bs_offset;  // 0x4749: mapped BAR1 + offset
    
    u8 num_devices = bs_base[0x0C];      // 0x474D
    if (num_devices == 0) {
        goto add_new_device;              // 0x4752 -> 0x4782
    }
    
    // Цикл поиска устройства по имени (0x4754-0x4780)
    for (u32 device_idx = 0; device_idx < num_devices; device_idx++) {
        void* device = bs_base + 0x10 + device_idx * 0xBDEC;
        
        // 0x476F: вызов _strnicmp (через 0x4CC0)
        int cmp_result = _strnicmp(
            device + 0x00,                // device name
            device_desc + 0x21,           // искомое имя
            0x80                          // максимальная длина сравнения
        );
        
        if (cmp_result == 0) {
            // Найдено совпадение имени
            goto found_device;            // 0x4776 -> 0x47EB
        }
    }
    
add_new_device:
    // 0x4782-0x47E9: добавление нового устройства
    u8 new_device_idx = num_devices;
    void* new_device = bs_base + 0x10 + new_device_idx * 0xBDEC;
    
    // Копирование имени устройства (128 байт через SSE)
    memcpy_sse(new_device + 0x00, device_desc + 0x21, 128);  // 0x4782-0x47E1
    
    void* target_device = new_device;
    goto register_streams;                // 0x47E9 -> 0x4861
    
found_device:
    // 0x47EB: устройство найдено
    u32 found_device_idx = device_idx;
    void* target_device = bs_base + 0x10 + found_device_idx * 0xBDEC;
    
register_streams:
    // 0x47F6-0x4836: регистрация input streams
    stream_index = 0;
    while (linked_list != NULL) {
        u32 array_idx = stream_index * 6;  // 0x47FC: lea rcx, [rax + rax*2]; shl
        
        // 0x480A, 0x4816: запись в массив buffers[] внутри input_streams[0]
        target_device->input_streams[0].buffers[stream_index].address = linked_list->field_0x08;
        target_device->input_streams[0].buffers[stream_index].size = linked_list->field_0x10;
        
        stream_index++;
        linked_list = linked_list->next;  // 0x481E: mov rdi, [rdi]
    }
    
    // 0x4826: сохранить количество буферов
    target_device->input_streams[0].num_buffers = stream_index;
    
    // 0x482D: установить флаг активности потока
    target_device->input_streams[0].active_flag = 1;  // offset 0x90 в stream
    
    // Альтернативная ветка для output streams (0x4838-0x4875)
    // [симметричный код, но записывает в другое смещение - требуется проверка через r8]
    
    // 0x4875: увеличить счётчик устройств
    bs_base[0x0C]++;
    
    // 0x4878-0x488B: вызов функции уведомления
    func_0x44E4(global, device_desc, param3, NULL);
    
    return STATUS_SUCCESS;
}
```

## Сопоставление с Linux audio.h

### Поля BufferStruct (подтверждено)

| Windows RVA | Смещение | Linux поле | Значение |
|-------------|----------|------------|----------|
| 0x4728 | +0x04 | signature | 0x19870423 (T2AUDIO_SIG) |
| 0x473B | +0x00 | version | >= 2 (Linux требует >= 3) |
| 0x4744 | +0x08 | bs_offset | Offset BufferStruct в BAR1 |
| 0x474D | +0x0C | num_devices | Количество устройств |

### Поля Device (подтверждено)

| Windows offset | Linux offsetof | Поле |
|----------------|----------------|------|
| +0x00 | +0x00 | name[128] |
| +0x80 | +0x80 | num_input_streams |
| +0x81 | +0x81 | num_output_streams |
| +0x84 | +0x84 | input_streams[5] |
| +0x5EF8 | +0x5EF8 | output_streams[5] |

**Stride**: 0xBDEC (48620 bytes) - точное совпадение

### Поля Stream

Смещение 0x98 и 0xA0 записываются через индекс `rcx*8`:
- `rcx = (stream_index * 3) * 2` (0x47FC, 0x4804)
- Массив из 6 QWORD на буфер: `address, size, pad[4]`

Вычисление смещения buffers[] в stream:
```
base_offset = 0x98 - (stream_index * 6 * 8)
При stream_index=0: base_offset = 0x98
```

**Проблема**: Linux offsetof(stream, buffers) = **0x04**, но Windows записывает в **0x98**.

### Анализ несоответствия

Смещение 0x98 относительно начала device:
```
0x98 = 0x84 + 0x14
       ^^^   ^^^
       |     смещение внутри stream
       offsetof(device, input_streams)
```

Ожидаемое: `0x84 + 0x04 = 0x88` (если buffers начинаются на +4 в stream)

**0x98 = 0x84 + 0x14** означает смещение +0x14 внутри stream.

### Пересчёт структуры stream (Windows layout)

Linux:
```c
struct stream {
    u8 num_buffers;              // +0x00
    struct buffer buffers[100];  // +0x04 (после padding до 4-byte align)
    char filler[32];
};
```

Возможный Windows layout:
```c
struct stream {
    u8 num_buffers;              // +0x00
    u8 pad1[3];                  // +0x01
    u32 flags_or_reserved;       // +0x04
    u32 additional_field1;       // +0x08
    u32 additional_field2;       // +0x0C
    u8 active_flag;              // +0x10 (записывается на 0x482D как [rdx+0x90])
    u8 pad2[3];
    struct buffer buffers[100];  // +0x14
    char filler[32];
};
```

**Подтверждение**: 
- 0x480A записывает в `[rdx + rcx*8 + 0x98]`
- rdx = device base
- 0x98 = 0x84 (input_streams) + 0x14 (buffers в stream)

### Поля узла linked list (R9)

```c
struct buffer_node {
    struct buffer_node* next;  // +0x00 (читается на 0x481E)
    size_t address;            // +0x08 (читается на 0x4800, 0x480A)
    size_t size;               // +0x10 (читается на 0x4812, 0x4816)
};
```

## Ключевые выводы

1. **Функция 0x46F8** регистрирует input streams для устройства в BufferStruct
2. **Поиск по имени** через _strnicmp (длина 128 байт)
3. **Если устройство найдено**: обновляет buffers[] существующего устройства
4. **Если не найдено**: добавляет новое устройство в конец массива
5. **Linked list (R9)** содержит дескрипторы буферов (address, size, next)
6. **Смещения записи**: 0x98, 0xA0 = buffers[i].address, buffers[i].size

## Неизвестное

1. **Происхождение linked list**: откуда берётся параметр R9?
2. **Output streams**: есть ли симметричный код для output_streams (offset 0x5EF8)?
3. **Функция 0x44E4**: что делает вызов после регистрации?
4. **Смещение 0x90**: флаг активности stream или другое поле?
