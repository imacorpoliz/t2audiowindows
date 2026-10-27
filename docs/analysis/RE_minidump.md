# RE_minidump: Анализ минидампов BSOD при START_IO (Задача R1)

> **Дата анализа:** 28 октября 2026 г.  
> **Инструмент:** WinDbg / CDB (`10.0.28000.2526 AMD64`), символы Microsoft Symbol Server (`srv*https://msdl.microsoft.com/download/symbols`).  
> **Анализируемые дампы:**
> 1. `C:\Windows\Minidump\100826-15859-01.dmp` (2026-10-08 01:35:07)
> 2. `C:\Windows\Minidump\100826-23546-01.dmp` (2026-10-08 01:48:20)  
> **Статус выводов:** **ДОКАЗАНО** (полное совпадение обоих дампов и машинного кода).

---

## 1. Сводка падения ядра

Оба минидампа фиксируют на 100% идентичную аварию ядра:

* **Код BugCheck:** `SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (7e)`
* **Параметр 1 (ExceptionCode):** `0xC0000005` (`STATUS_ACCESS_VIOLATION`)
* **Параметр 2 (ExceptionAddress):** `ks!DispatchDeviceIoControl+0x15`
* **Параметр 3 (ExceptionRecord):** `ffffbb0180d1d3d8` (дамп 23546) / `fffff484865053d8` (дамп 15859)
* **Параметр 4 (ContextRecord):** `ffffbb0180d1cc10` (дамп 23546) / `fffff48486504c10` (дамп 15859)
* **Поток:** Системный рабочий поток (`PROCESS_NAME: System`)

---

## 2. Дизассемблер точки падения в `ks.sys`

Контекст регистров в момент аварии:
```text
rax=0000000000000000 rbx=ffffe586444dd040 rcx=ffffe58643ca17f0
rdx=ffffe586444dd040 rsi=ffffe586444dd2c0 rdi=ffffe58642431560
rip=fffff8008fbb0bf5 rsp=ffffbb0180d1d610 rbp=ffffbb0180d1d6d0
```

Листинг `ks!DispatchDeviceIoControl`:
```assembly
ks!DispatchDeviceIoControl:
fffff800`8fbb0be5 8b82b8000000    mov     eax, dword ptr [rdx+0B8h]  ; rdx = IRP, [rdx+0xB8] = CurrentStackLocation
fffff800`8fbb0beb 4c8bc2          mov     r8, rdx
fffff800`8fbb0bee 4c8bc9          mov     r9, rcx
fffff800`8fbb0bf1 488b4030        mov     rax, qword ptr [rax+30h]   ; rax = CurrentStackLocation->FileObject
fffff800`8fbb0bf5 488b5018        mov     rdx, qword ptr [rax+18h]   ; rdx = FileObject->FsContext -> КРАХ!
```

### Доказанный факт:
1. `CurrentStackLocation->FileObject` равен строго `0x0000000000000000` (`NULL`).
2. `ks.sys` выполняет чтение `[rax + 0x18]`, где `rax == 0`, порождая `ACCESS_VIOLATION` при чтении по адресу `0x0000000000000018`.
3. В `ks.sys` полностью отсутствует проверка на `NULL` для `FileObject` в начале `DispatchDeviceIoControl`.

---

## 3. Стек вызовов и источник IRP

Стек вызовов в обоих дампах:
```text
# Child-SP          RetAddr               Call Site
00 ffffbb01`80d1d610 fffff800`e8f597bb     ks!DispatchDeviceIoControl+0x15
01 ffffbb01`80d1d640 fffff800`7df31534     nt!IofCallDriver+0x4b
02 ffffbb01`80d1d680 fffff800`7df31253     ksthunk!CKernelFilterDevice::DispatchIrp+0xf4
03 ffffbb01`80d1d6e0 fffff800`e8f597bb     ksthunk!CKernelFilterDevice::DispatchIrpBridge+0x13
04 ffffbb01`80d1d710 fffff800`7def259a     nt!IofCallDriver+0x4b
05 ffffbb01`80d1d750 ffffffff`80001ec8     AppleUSBVHCI+0x259a
```

### Доказанный факт:
1. Адрес возврата `AppleUSBVHCI+0x259a` точно соответствует инструкции после вызова `IofCallDriver` в функции `AppleUSBVHCI.sys` по смещению `0x2594`:
   * `0x140002594`: `call IofCallDriver`
   * `0x14000259A`: `cmp eax, 0x103` (`STATUS_PENDING`)
2. `AppleUSBVHCI.sys` формирует этот IRP через `IoBuildDeviceIoControlRequest(0x222020, DeviceObject, ...)`:
   * `IoBuildDeviceIoControlRequest` стандартно оставляет `FileObject == NULL`.
3. IRP направляется в интерфейс `KSCATEGORY_AUDIO` устройства с подстрокой `"VEN_106B"`.
4. Верхним фильтром устройства являлся `ksthunk.sys` (Kernel Streaming Thunk), который пропустил IRP с кодом `0x222020` дальше вниз в стек PortCls/KS.
5. Поскольку в `T2AudioPort` обработчик `IRP_MJ_DEVICE_CONTROL` ранее без изменений передавал IRP в `PcDispatchIrp` -> `ks!DispatchDeviceIoControl`, `ks.sys` падал при обращении к `NULL FileObject`.

---

## 4. Состояние буфера IRP в дампе

* В минидампах (`Mini Kernel Dump File: Only registers and stack trace are available`) физические страницы, содержавшие тело IRP (`rdx = ffffe586444dd040`, `rdi = ffffe58642431560` — DeviceObject) и SystemBuffer, не были включены в срез памяти (`Could not read Irp`).
* Тем не менее, регистры в кадре `ks!DispatchDeviceIoControl` однозначно фиксируют адрес IRP в `rdx`, извлечение `CurrentStackLocation` в `rax` и чтение `CurrentStackLocation->FileObject == NULL`. Цепочка вызовов `AppleUSBVHCI+0x259a` -> `ksthunk` -> `ks!DispatchDeviceIoControl+0x15` полностью согласуется со статическим анализом в `START_IO_BSOD_ROOT_CAUSE.md`.

---

## 5. Итог задачи R1

| Вопрос | Ответ | Статус |
| :--- | :--- | :--- |
| Кто источник падающего IRP? | `AppleUSBVHCI.sys` по смещению `0x2594` | **ДОКАЗАНО** |
| Какой код IOCTL? | `0x222020` (зашит в `AppleUSBVHCI.sys` по адресу `0x140002566`) | **ДОКАЗАНО** |
| Почему `FileObject == NULL`? | Создан через `IoBuildDeviceIoControlRequest`, который не привязывает `FileObject` | **ДОКАЗАНО** |
| Почему падал KS? | `ks!DispatchDeviceIoControl` безусловно читает `FileObject->FsContext` (`+0x18`) | **ДОКАЗАНО** |
| Корректно ли исправление в `src/Driver.c`? | Перехват `0x222020` в `DriverEntry` до PortCls/KS исключает попадание IRP в `ks.sys` | **вероятно; не проверено на железе** |