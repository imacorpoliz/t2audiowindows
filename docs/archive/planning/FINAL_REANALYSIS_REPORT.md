# Финальный отчёт повторного анализа AppleAudio.sys

**Дата**: 2026-10-04  
**Задача**: Повторная независимая проверка вызовов MmMapIoSpaceEx, исправление ошибок RVA→offset преобразования, полная трассировка до использования буферов

---

## Результат проверки

### SHA256 бинарника: ✓ СОВПАДАЕТ

```
Проверенный файл: C:\Users\othysa\Desktop\mbp\MBP-Audio-driver\AppleAudio.sys
SHA256: CD8B81FABC9BE6127A082DA22C4CC046A4D46E0225EB6BAF6D27E0DC9862A150
Размер: 122752 bytes (0x1DF00)
PE Timestamp: 0x5F73BACC
```

Это **ПРАВИЛЬНЫЙ бинарник** AppleAudio.sys 6.1.8100.4.

### Исправление ошибки преобразования RVA → file offset

**Проблема в предыдущем анализе**: применение разницы (RVA - raw offset) секции .text ко всем секциям.

**Правильная таблица секций**:

```
.text    VA=0x001000  RawOff=0x000400  RawSize=0x00C800  EXEC
.rdata   VA=0x00E000  RawOff=0x00CC00  RawSize=0x003600
.data    VA=0x012000  RawOff=0x010200  RawSize=0x002800
.pdata   VA=0x015000  RawOff=0x012A00  RawSize=0x000C00
PAGE     VA=0x016000  RawOff=0x013600  RawSize=0x007200  EXEC
INIT     VA=0x01E000  RawOff=0x01A800  RawSize=0x000E00  EXEC
.rsrc    VA=0x01F000  RawOff=0x01B600  RawSize=0x000400
.reloc   VA=0x020000  RawOff=0x01BA00  RawSize=0x000400
```

**Правильное преобразование для секции PAGE**:
```
file_offset = (RVA - 0x16000) + 0x13600
```

**Проверка предоставленных RVA**:

| RVA      | Секция | File offset | Фактические байты (начало)      |
|----------|--------|-------------|----------------------------------|
| 0x17204  | PAGE   | 0x14804     | `FF 15 4E 6E FF FF 48 89 43 70` |
| 0x1721C  | PAGE   | 0x1481C     | `FF 15 36 6E FF FF 48 89 43 68` |

**Дизассемблирование**:

```asm
; RVA 0x17204 (file offset 0x14804)
call qword ptr [rip-0x91B2]    ; FF 15 4E 6E FF FF
mov [rbx+0x70], rax            ; 48 89 43 70

Целевой IAT слот: 0x17204 + 6 - 0x91B2 = 0xE058

; RVA 0x1721C (file offset 0x1481C)
call qword ptr [rip-0x91CA]    ; FF 15 36 6E FF FF
mov [rbx+0x68], rax            ; 48 89 43 68

Целевой IAT слот: 0x1721C + 6 - 0x91CA = 0xE058
```

**IAT слот 0xE058**: `ntoskrnl.exe!MmMapIoSpaceEx` ✓ ПОДТВЕРЖДЕНО

---

## Полная проверка исполняемых секций

### Сканирование всех EXEC-секций на вызовы IAT 0xE058

**Метод**: поиск инструкций `FF 15` (call [rip+offset]) во всех секциях с флагом `IMAGE_SCN_MEM_EXECUTE`.

**Результаты**:

```
.text: 152 rip-relative calls найдено, 0 → IAT 0xE058
PAGE:  227 rip-relative calls найдено, 2 → IAT 0xE058
INIT:   11 rip-relative calls найдено, 0 → IAT 0xE058
```

**Вызовы MmMapIoSpaceEx**:

| RVA     | Секция | File offset | Инструкция                  |
|---------|--------|-------------|-----------------------------|
| 0x17204 | PAGE   | 0x14804     | `FF 15 4E 6E FF FF`         |
| 0x1721C | PAGE   | 0x1481C     | `FF 15 36 6E FF FF`         |

**Вывод**: В AppleAudio.sys ровно **2 вызова MmMapIoSpaceEx**, оба в секции PAGE.

---

## Независимая проверка дизассемблером capstone

### Функция 0x16FF0–0x172FE (полный разбор)

**Сигнатура** (реконструкция):
```c
NTSTATUS InitializeMappedResources(
    void* object,           // rcx (сохр. в rbx)
    void* physicalDevice,   // rdx (сохр. в rbp)
    void* context           // r8  (сохр. в r14)
);
```

**Ключевые операции**:

#### 1. Инициализация полей объекта (0x16FF0–0x17044)

```asm
0x16FF0: push rbx/rbp/rsi/rdi/r13/r14/r15
0x16FFB: sub rsp, 0x40
0x16FFF: xor r15d, r15d                    ; r15 = 0
0x17002: mov [rcx+0x30], rdx               ; obj->field_30 = physDevice
0x17006: mov [rcx+0x50], r15               ; obj->field_50 = 0
0x17011: mov [rcx+0x28], r15               ; obj->field_28 = 0
0x17019: mov [rcx+0x58], r15               ; obj->field_58 = 0
0x17021: mov [rdi], r15                    ; rdi = obj + 0x40
0x17024: mov rbx, rcx                      ; rbx = object
0x17032: mov dword [rcx+0x48], 1          ; obj->field_48 = 1
0x17035: mov r14, r8                       ; r14 = context
0x1703E: call [rip-0x8E0C]                 ; PcGetPhysicalDeviceObject
0x17044: test eax, eax
0x17046: js error
```

#### 2. Выделение буфера и инициализация (0x1707C–0x170CF)

```asm
0x1707C: call [rip-0x8E32]                 ; неизвестная WDF/PortCls функция
0x1708A: mov edx, 0x200                    ; size = 512
0x1708F: mov ecx, 0x100                    ; pool type
0x17094: mov r8d, 0x44554141               ; tag 'AUUD'
0x1709A: call 0x13A4                       ; ExAllocatePool
0x1709F: test rax, rax
0x170A2: je error
0x170A7: call 0x17E08                      ; инициализация выделенного объекта
0x170B1: mov [rbx+0x50], rax               ; obj->field_50 = allocated_buffer
0x170CA: call 0x17E74                      ; дополнительная настройка
```

#### 3. Регистрация device interface (0x170D6–0x17247)

```asm
0x170DD: mov rdx, [rdi]
0x170E0: lea r8, [rip-0x8647]
0x170E7: mov ecx, 0x40
0x17109: call [rip-0x8EBF]                ; неизвестная WDF функция
0x17156: call [rip-0x8F0C]                ; возможно IoSetDeviceInterfaceState
0x17197: call [rip-0x8F4D]                ; ещё один вызов WDF
```

#### 4. Обнуление mapped-указателей (0x171A2–0x171AC)

```asm
0x171A2: lfence
0x171A5: mov qword ptr [rbx+0x68], r15    ; obj->mapped_bar1 = NULL
0x171AC: mov qword ptr [rbx+0x70], r15    ; obj->mapped_bar2 = NULL
```

#### 5. Итерация по ресурсам, вызовы MmMapIoSpaceEx (0x171B0–0x1722F)

```asm
0x171B0: mov rcx, r14                      ; rcx = context
0x171B3: mov rax, [r14]                    ; rax = context->vtable
0x171B9: lea edx, [r8+3]                   ; edx = 3
0x171BD: mov rax, [rax+0x28]               ; rax = vtable[5] (GetResourceCount?)
0x171C1: call [rip-0x8F77]                 ; indirect call через CFG
0x171C7: mov rcx, [r14]
0x171CF: mov edx, 3
0x171D2: mov rax, [rcx+0x20]               ; rax = vtable[4] (GetResourceList?)
0x171D6: mov rcx, r14
0x171D9: call [rip-0x8F8F]                 ; indirect call через CFG
0x171DF: mov r14d, eax                     ; r14d = resource_count
0x171E2: test eax, eax
0x171E4: je 0x17231
0x171E6: add rdi, r13                      ; r13 = 4
0x171E9: cmp byte ptr [rdi-4], 3          ; проверка типа ресурса
0x171ED: jne 0x17228                       ; пропустить не-memory ресурсы
0x171EF: test esi, esi                     ; esi = index (0, 1, 2...)
0x171F1: je 0x17210                        ; if (index == 0)

; Второй memory-ресурс (index != 0, но == 2 по коду)
0x171F3: cmp esi, 2
0x171F6: jne 0x17226
0x171F8: mov edx, [rdi+8]                  ; edx = size
0x171FB: mov r8d, 0x204                    ; r8d = cache_type (MmNonCached | MmWriteCombined)
0x17201: mov rcx, [rdi]                    ; rcx = physical_address
0x17204: call [rip-0x91B2]                 ; MmMapIoSpaceEx(phys, size, 0x204)
0x1720A: mov [rbx+0x70], rax               ; obj->mapped_bar2 = rax
0x1720E: jmp 0x17226

; Первый memory-ресурс (index == 0)
0x17210: mov edx, [rdi+8]                  ; edx = size
0x17213: mov r8d, 0x404                    ; r8d = cache_type
0x17219: mov rcx, [rdi]                    ; rcx = physical_address
0x1721C: call [rip-0x91CA]                 ; MmMapIoSpaceEx(phys, size, 0x404)
0x17222: mov [rbx+0x68], rax               ; obj->mapped_bar1 = rax
0x17226: inc esi                           ; index++
0x17228: add rdi, 0x14                     ; следующий ресурс (stride 20 bytes)
0x1722C: cmp esi, r14d
0x1722F: jb 0x171E9                        ; loop
```

**Параметры вызовов MmMapIoSpaceEx**:

| Вызов | RVA     | PhysAddr      | Size         | CacheType | Результат         |
|-------|---------|---------------|--------------|-----------|-------------------|
| 1-й   | 0x1721C | `[rdi+0]`     | `[rdi+8]`    | 0x404     | `[rbx+0x68]`      |
| 2-й   | 0x17204 | `[rdi+0]`     | `[rdi+8]`    | 0x204     | `[rbx+0x70]`      |

**RDI** указывает на descriptor ресурса в списке, предоставленном через context (r14).

**Cache types**:
- `0x404` = `MmCached | MmWriteCombined`
- `0x204` = `MmNonCached | MmWriteCombined`

#### 6. Сохранение mapped-адресов в глобальный объект (0x17231–0x1724C)

```asm
0x17231: mov r8, [rbx+0x70]                ; r8 = obj->mapped_bar2
0x17235: mov rdx, [rbx+0x68]               ; rdx = obj->mapped_bar1
0x17239: add r8, 0xC000                    ; r8 = mapped_bar2 + 0xC000
0x17240: mov rcx, [rip-0x2BB7]             ; rcx = global_object @ 0x14690
0x17247: call 0x45F0                       ; SetMappedAddresses(global, bar1, bar2+0xC000)
```

**Функция 0x45F0** (3 инструкции):
```asm
0x045F0: mov [rcx+0x28], rdx               ; global->bar1_mapped = bar1
0x045F4: mov [rcx+0x30], r8                ; global->bar2_mapped = bar2 + 0xC000
0x045F8: ret
```

#### 7. Дальнейшая настройка через registry/properties (0x1724C–0x172EA)

```asm
0x1724C: lfence
0x1724F: mov rax, [rip-0x2B6E]
0x1727C: call [rip-0x9032]                 ; RtlQueryRegistryValues или аналог
0x172B3: call [rip-0x9069]                 ; ещё один property-вызов
0x172D9: call [rip-0x908F]                 ; ещё один property-вызов
0x172E2: call 0x19010                      ; cleanup/finalization
0x172EA: call 0x18DC0                      ; cleanup/finalization
0x172EF: add rsp, 0x40
0x172F3-0x172FD: pop r15/r14/r13/rdi/rsi/rbp/rbx; ret
```

---

## Трассировка использования mapped-адресов до буферов

### Глобальный объект 0x14690

**Поля**:
- `+0x28`: mapped BAR1 (`[rbx+0x68]` из функции 0x16FF0)
- `+0x30`: mapped BAR2 + 0xC000 (`[rbx+0x70]+0xC000` из функции 0x16FF0)

**Обращения к полям**: 347 найдено через сканирование всех EXEC-секций.

### Функция 0x4700–0x4892: работа с mapped-буфером

**Ключевой код** (RVA 0x4718–0x4750):

```asm
0x04718: mov rax, [rcx+0x30]               ; rcx = global @ 0x14690
                                           ; rax = mapped_bar2 + 0xC000
0x0471C: xor sil, sil
0x0471F: mov rdi, r9
0x04722: mov r14, rdx
0x04725: mov r15, rcx
0x04728: cmp dword ptr [rax+4], 0x19870423 ; magic signature check
0x0472F: je 0x473B
0x04731: mov eax, 0xC0000001               ; STATUS_UNSUCCESSFUL
0x04736: jmp 0x4892

0x0473B: cmp dword ptr [rax], 2            ; version check
0x0473E: jb 0x488D
0x04744: mov ebx, [rax+8]                  ; ebx = buffer_offset
0x04747: xor ebp, ebp
0x04749: add rbx, [rcx+0x28]               ; rbx = mapped_bar1 + buffer_offset
                                           ; ← RBX теперь указывает на буфер
0x0474D: mov al, [rbx+0xC]                 ; count = buffer[0xC]
0x04750: test al, al
0x04752: je 0x4782
0x04754: mov r12d, ebp
0x04757: lea rdx, [rbx+0x10]               ; rdx = buffer + 0x10
0x0475B: imul rax, r12, 0xBDEC
0x04762: mov r8d, 0x80                     ; size = 0x80
0x04768: lea rcx, [r14+0x21]               ; rcx = dest
0x0476C: add rdx, rax                      ; rdx = src (buffer entry)
0x0476F: call 0x4CC0                       ; memcpy-like function
0x04774: test eax, eax
0x04776: je 0x47EB
0x04778: movzx eax, byte ptr [rbx+0xC]
0x0477C: inc ebp                           ; index++
0x0477E: cmp ebp, eax
```

**Анализ**:

1. **Mapped BAR2 + 0xC000** содержит **метаданные**:
   - `+0x00`: version (должно быть ≥ 2)
   - `+0x04`: magic `0x19870423`
   - `+0x08`: offset буфера относительно BAR1

2. **Mapped BAR1 + offset** → **реальный буфер данных**:
   - `+0x0C`: количество элементов
   - `+0x10+`: массив структур размером `0xBDEC` каждая

3. **Использование**: копирование данных из mapped-буфера в локальный буфер драйвера через функцию 0x4CC0.

### Множественные обращения к mapped-адресам

**Функции, читающие `[global+0x28]` или `[global+0x30]`**:

- 0x29A8/0x29AC: чтение обоих BAR
- 0x3632, 0x3730, 0x3830, 0x394B, 0x3A4B, 0x3B48, 0x3C48, 0x3D48: чтение BAR1
- 0x4718–0x4750: основная работа с буферами (см. выше)
- 0x4A14, 0x4B94: чтение BAR1

**Паттерн**: глобальный объект 0x14690 служит **центральным хранилищем** mapped-адресов для всего драйвера.

---

## Ответы на вопросы задачи

### 1. Что именно отображается?

**BAR1** (первый memory-ресурс):
- Размер: `[rdi+8]` (из CM_RESOURCE_LIST)
- Cache type: 0x404 (Cached | WriteCombined)
- Назначение: **основной буфер данных** (метаданные + массив структур)

**BAR2** (второй memory-ресурс):
- Размер: `[rdi+8]`
- Cache type: 0x204 (NonCached | WriteCombined)
- Назначение: **control registers** (метаданные на offset +0xC000 указывают на offset в BAR1)

**Физические адреса** берутся из WDF-ресурсов (передаются через context r14 в функцию 0x16FF0).

### 2. Откуда берутся физические адреса и размеры?

**Источник**: WDF framework предоставляет список ресурсов через context-объект (параметр r8 функции 0x16FF0).

**Механизм**:
1. Вызов vtable[5] (0x171BD): получение количества ресурсов
2. Вызов vtable[4] (0x171D2): получение списка ресурсов
3. Итерация по списку с проверкой типа (byte ptr [rdi-4] == 3 → memory resource)

**Формат дескриптора ресурса**:
- `+0x00`: physical address (QWORD)
- `+0x08`: size (DWORD)
- `+0x0C`: flags/type
- Stride: 0x14 (20 bytes)

### 3. Какой объект находится в RBX?

**RBX** = локальный объект драйвера (параметр rcx функции 0x16FF0).

**Структура** (частичная реконструкция):

```c
struct LocalDriverObject {
    // ...
    void*  field_28;        // +0x28
    void*  field_30;        // +0x30
    void*  field_38;        // +0x38
    void*  field_40;        // +0x40 (rdi = rcx + 0x40)
    DWORD  field_48;        // +0x48 = 1
    void*  allocated_buffer;// +0x50 (tag 'AUUD', size 512)
    void*  field_58;        // +0x58
    void*  mapped_bar1;     // +0x68 ← первый MmMapIoSpaceEx
    void*  mapped_bar2;     // +0x70 ← второй MmMapIoSpaceEx
    // ...
};
```

**Назначение**: временное хранение mapped-адресов перед копированием в глобальный объект 0x14690.

### 4. Зачем результаты отображения сохраняются в `[rbx+0x70]` и `[rbx+0x68]`?

**Цель**: сохранение результатов `MmMapIoSpaceEx` перед передачей в глобальный объект.

**Поток данных**:
```
MmMapIoSpaceEx (0x1721C) → [rbx+0x68]
MmMapIoSpaceEx (0x17204) → [rbx+0x70]
           ↓                     ↓
      (0x17231-0x17235)    (чтение)
           ↓                     ↓
      функция 0x45F0
           ↓
   [global_0x14690 + 0x28] = [rbx+0x68]
   [global_0x14690 + 0x30] = [rbx+0x70] + 0xC000
```

**Почему offset 0xC000 добавляется?**
- BAR2 содержит control registers и таблицы метаданных.
- Offset 0xC000 указывает на **BufferStruct** (аналогично GPR-регистрам из Linux kait2en).
- Эта структура содержит указатель на offset буфера в BAR1.

### 5. Куда идут mapped-адреса дальше?

**Глобальный объект 0x14690**:
- Поля `+0x28` и `+0x30` доступны из 347 мест в коде.
- Основная функция работы с буферами: **0x4700–0x4892**.
- Операция: **чтение структур данных из mapped-памяти и копирование в локальные буферы**.

**Связь с аудио-буферами**:
- Функция 0x16FF0 вызывается при инициализации устройства (DriverEntry/AddDevice).
- Глобальный объект 0x14690 служит центральным хранилищем для доступа к T2 hardware.
- Буферы данных (через BAR1) содержат **метаданные устройства, не PCM-буферы напрямую**.

**Не найдено**: связь с WaveSpeaker KS pin callback или PCM-рендерингом.

---

## Исправления предыдущих ошибок

### 1. RVA → file offset преобразование ✓ ИСПРАВЛЕНО

**Было**: применение offset .text ко всем секциям.  
**Стало**: индивидуальное преобразование по таблице секций.

### 2. "MmMapIoSpaceEx не используется" ✗ БЫЛО НЕВЕРНО

**Было**: "IAT@0xE058 не используется, 0 вызовов найдено".  
**Факт**: **2 вызова** найдено в секции PAGE (RVA 0x17204, 0x1721C).  
**Причина ошибки**: поиск только в секции .text, пропуск PAGE и INIT.

### 3. "RVA 0x17204 = epilogue функции" ✗ БЫЛО НЕВЕРНО

**Было**: "0x17204: mov rbp, [rsp+0x38]; add rsp, 0x20; pop rdi; ret".  
**Факт**: это было содержимое RVA 0x171F4 (ошибка в вычислении offset).  
**Правильно**: RVA 0x17204 = `call [rip-0x91B2]` → MmMapIoSpaceEx.

### 4. Offset +0x68/+0x70 "не используются" ✗ БЫЛО НЕВЕРНО

**Было**: "Паттерны `48 89 43 68` и `48 89 43 70` не найдены".  
**Факт**: найдены на RVA 0x17222 и 0x1720A соответственно.  
**Причина ошибки**: поиск только в .text, пропуск секции PAGE.

### 5. Функция 0x1FA0 MmMapLockedPagesSpecifyCache ≠ буферы T2

**Было в BUFFER_PATH_CORRECTIONS.md**: предположение о связи с PCM-буферами.  
**Факт**: функция 0x1FA0 не вызывается из функции 0x16FF0 (разные цепочки инициализации).  
**Вывод**: MmMapLockedPagesSpecifyCache (0x2051) и MmMapIoSpaceEx (0x17204/0x1721C) — **разные механизмы**, не связаны напрямую.

---

## Воспроизводимый скрипт проверки

### Verify_RVA.ps1

**Файл**: `C:\Users\othysa\Desktop\mbp\Verify_RVA.ps1`

**Функции**:
1. Разбор PE-заголовков (секции, импорты)
2. Правильное RVA → file offset преобразование (per-section)
3. Dump байтов по file offset
4. Полный скан всех EXEC-секций на `call [rip+offset]` → IAT targets
5. Идентификация IAT-слотов через Import Directory Table

**Запуск**:
```powershell
pwsh -NoProfile -File Verify_RVA.ps1
```

**Ожидаемый результат**:
```
=== Scan of ALL EXEC sections ===
.text: 152 rip-relative calls found
PAGE:  227 rip-relative calls found
INIT:   11 rip-relative calls found

--- Calls targeting IAT slot 0xE058 (MmMapIoSpaceEx) ---
  PAGE: RVA 0x17204 (file 0x14804)  bytes: FF 15 4E 6E FF FF
  PAGE: RVA 0x1721C (file 0x1481C)  bytes: FF 15 36 6E FF FF
```

### disasm2.py

**Файл**: `C:\Users\othysa\Desktop\mbp\disasm2.py`

**Функции**:
1. Независимый disassembly через capstone
2. Автоматическая аннотация IAT-вызовов и rip-relative данных
3. Xref-поиск (call/jmp/lea ссылок на RVA)
4. Field-scan (поиск обращений к смещениям 0x68/0x70)

**Запуск**:
```bash
py disasm2.py dis 16FF0 172FE    # дизассемблирование функции
py disasm2.py xref 16FF0          # поиск xrefs
py disasm2.py field               # сканирование полей +0x68/+0x70
```

**Проверка вызовов MmMapIoSpaceEx**:
```bash
py disasm_func.py 16FF0 172FE | Select-String "17204|1721C"
```

**Результат**:
```
0x17204: call     qword ptr [rip - 0x91b2]   ; -> IAT 0xE058 = ntoskrnl.exe!MmMapIoSpaceEx
0x1721C: call     qword ptr [rip - 0x91ca]   ; -> IAT 0xE058 = ntoskrnl.exe!MmMapIoSpaceEx
```

---

## Заключение

### Подтверждённые факты

✓ AppleAudio.sys **использует MmMapIoSpaceEx** (2 вызова в функции 0x16FF0)  
✓ Отображаются **2 memory-ресурса T2** (BAR1 + BAR2)  
✓ BAR1: основной буфер данных (Cached|WriteCombined)  
✓ BAR2+0xC000: метаданные и control registers (NonCached|WriteCombined)  
✓ Mapped-адреса сохраняются в глобальном объекте 0x14690 (поля +0x28, +0x30)  
✓ Глобальный объект доступен из **347 мест** в коде  
✓ Основная работа с буферами: функция 0x4700 (чтение метаданных + копирование структур)  
✓ Magic signature `0x19870423` подтверждает структурированный протокол T2

### Что НЕ найдено

✗ **Прямая связь с PCM-буферами**: функция 0x16FF0 выполняется при инициализации устройства, не при воспроизведении аудио  
✗ **WaveSpeaker pin callback**: не прослежена цепочка от KS audio rendering до mapped-буферов  
✗ **Запись PCM-данных**: функция 0x4700 **читает** из mapped-памяти, не пишет  
✗ **Связь с IOCTL 0x222018**: message path (0x4A60) и buffer path (0x16FF0) — независимые механизмы

### Неразрешённые вопросы

1. **Откуда вызывается функция 0x16FF0?** Xref-поиск не нашёл прямых вызовов (вероятно, dispatch через vtable или callback).
2. **Где происходит запись PCM-данных?** Mapped-буферы используются для чтения метаданных, не для записи audio samples.
3. **Связь BAR1-буферов с WaveSpeaker?** Не установлена в рамках статического анализа.
4. **Назначение BufferStruct на BAR2+0xC000?** Содержит offset, version, magic — аналогично Linux GPR-механизму, но детали структуры не полностью восстановлены.

### Итоговая оценка

**MmMapIoSpaceEx вызовы: НАЙДЕНЫ и ПОДТВЕРЖДЕНЫ** (2 вызова, RVA 0x17204 + 0x1721C).

**Mapped-адреса: прослежены до глобального объекта и функций работы с буферами**.

**PCM audio path: НЕ НАЙДЕН** — требуется runtime debugging или дополнительный статический анализ функций, использующих глобальный объект 0x14690.

**Следующий шаг**: runtime breakpoint на MmMapIoSpaceEx (0x17204/0x1721C) для захвата физических адресов и размеров mapped-регионов.

---

## Сопоставление с Linux KAIT2EN

Подробная таблица соответствия структур, полей и механизмов между Linux-драйвером и AppleAudio.sys: **[Linux_Windows_Structure_Mapping.md](Linux_Windows_Structure_Mapping.md)**

**Ключевые находки**:
- ✓ BAR mapping: Linux BAR0/BAR4 ↔ Windows [rbx+0x68]/[rbx+0x70]
- ✓ GPR structure: Linux reg_mem_gpr+0xC000 ↔ Windows [global+0x30]
- ✓ BufferStruct signature 0x19870423: найдена в обоих драйверах
- ✓ Device stride 0xBDEC: совпадает точно
- ✗ PCM write path: найден в Linux (zero-copy ALSA), НЕ НАЙДЕН в Windows

**Рекомендация**: гибридный подход — использовать проверенную Linux-логику BufferStruct parsing + существующую Windows BAR mapping инициализацию. Оценка трудоёмкости: 40-60 часов для minimal viable PCM playback driver.
