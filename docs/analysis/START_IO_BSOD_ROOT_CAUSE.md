# START_IO BSOD Root Cause: IOCTL 0x222020 Handshake via AppleUSBVHCI

> **Date:** 2026-10-28  
> **Status:** Static analysis proven by disassembly of `AppleUSBVHCI.sys` 6.1.7500.5 and minidumps `100826-15859-01.dmp`, `100826-23546-01.dmp`. Runtime fix implemented in `Driver.c` / `BceTransport.c` (вероятно; не проверено на железе).  
> **Scope:** Resolves the `0x7E` bugcheck at `ks!DispatchDeviceIoControl+0x15` on `START_IO`.

---

## 1. Сводка

На тестах 2026-10-08 прогон `-Stage 4 -BceIo` дважды привёл к BSOD `0x7E` (`STATUS_ACCESS_VIOLATION`) на инструкции:
```text
ks!DispatchDeviceIoControl+0x15:
mov rdx, qword ptr [rax+18h]    ; rax = CurrentStackLocation->FileObject == NULL -> чтение 0x18
```
Стек вызовов в дампе:
```text
ks!DispatchDeviceIoControl+0x15
nt!IofCallDriver+0x4b
ksthunk!CKernelFilterDevice::DispatchIrp+0xf4
ksthunk!CKernelFilterDevice::DispatchIrpBridge+0x13
nt!IofCallDriver+0x4b
AppleUSBVHCI+0x259a
```

Попытка исправить крах через проставление `FileObject` в исходящем IRP `T2AudioSendBceMessage` эффекта не дала, потому что **падающий IRP отправляет не наш драйвер**. Его создаёт `AppleUSBVHCI.sys`.

---

## 2. Механизм доставки bridgeOS → Host через AppleUSBVHCI

Дизассемблирование `AppleUSBVHCI.sys` (версия 6.1.7500.5, SHA256 `0438292090fc...55c1`):

1. **Приём пакета от bridgeOS:** `AppleUSBVHCI` регистрирует прерывание / очередь сообщений BCE. При приёме вызывается обработчик входящих пакетов (`0x140001CF0`).
2. **Фильтрация типов сообщений (`0x140001D72`–`0x140001D9A`):**
   * `type == 1` (COMMAND): доставляется в аудиодрайвер через `0x140002274` **только при `msg == 4`** (`UPDATE_TIMESTAMP`), иначе отбрасывается.
   * `type == 2` (RESPONSE): не уходит в аудиодрайвер, а вызывает `KeSetEvent(0x27448)`, пробуждая синхронный `IOCTL 0x222018`.
   * `type == 3` (NOTIFICATION): доставляется через `0x140002274` для всех сообщений, **кроме `msg == 100`** (`NOTIFICATION_ALIVE`), которое также будит `KeSetEvent(0x27448)`.
3. **Поиск аудиодрайвера (`0x140002274`):**
   * `AppleUSBVHCI` вызывает `IoGetDeviceInterfaces(&KSCATEGORY_AUDIO, ...)` и ищет первый интерфейс, символическая ссылка которого содержит подстроку `"VEN_106B"`.
   * Открывает целевой `DEVICE_OBJECT` через `IoGetDeviceObjectPointer`.
4. **Формирование IRP (`0x140002540`–`0x140002594`):**
   * Вызывает `IoBuildDeviceIoControlRequest(IoControlCode = 0x222020, TargetDeviceObject, InputBuffer = msg, ...)`.
   * **`IoBuildDeviceIoControlRequest` оставляет `FileObject == NULL`.**
   * Отправляет IRP вызовом `IofCallDriver(TargetDeviceObject, Irp)`.
   * Адрес возврата — `AppleUSBVHCI+0x259a`.

---

## 3. Почему штатный `AppleAudio.sys` не падает

В `AppleAudio.sys` (RVA `0x1280`, `IRP_MJ_DEVICE_CONTROL`):
```assembly
0x140001280:
...
cmp eax, 0x222020
je  handle_vhci_message
...
; handle_vhci_message (0x1400012BF):
; msg == 4  -> 0x14000435C (сохранение UPDATE_TIMESTAMP)
; msg == 13 -> 0x1400044B8 (PROPERTY_CHANGED: jack status)
; завершает IRP со STATUS_SUCCESS без передачи в PortCls / KS
```
Штатный драйвер **перехватывает `0x222020` в собственной точке входа `IRP_MJ_DEVICE_CONTROL`** и завершает его самостоятельно, не пропуская в `ks.sys`.

---

## 4. Почему падал `T2AudioPort`

`T2AudioPort` инициализировался через PortCls (`PcInitializeAdapterDriver`). Все IRP, включая `IRP_MJ_DEVICE_CONTROL`, уходили в `PcDispatchIrp` -> `ks!DispatchDeviceIoControl`.  
Поскольку `AppleUSBVHCI` сформировал IRP через `IoBuildDeviceIoControlRequest` с `FileObject == NULL`, `ks.sys` безусловно разыменовывал `NULL + 0x18`, вызывая BSOD `0x7E`.

---

## 5. Исправление и открытые вопросы

1. **Перехват `0x222020`:**
   В `DriverEntry` (`src/Driver.c`) назначен хук `DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = T2AudioDeviceControl`.
   Для кода `0x222020` вызывается `T2AudioHandleVhciMessage` (`src/BceTransport.c`), IRP завершается со `STATUS_SUCCESS`.
   Разрешён только для IRP режима ядра (`Irp->RequestorMode == KernelMode`).
2. **Статус:** Анализ причин подтверждён декомпиляцией и минидампами ядра. Работоспособность обработчика на реальном железе предстоит подтвердить на этапах D/E (после согласования).