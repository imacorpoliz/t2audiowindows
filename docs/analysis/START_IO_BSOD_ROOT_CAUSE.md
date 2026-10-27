# BSOD 0x7E при START_IO — корневая причина

Статический анализ, 2026-10-28. Бинарники: `AppleUSBVHCI.sys` 6.1.7500.5 (SHA256 `0438292090fc5267ca6d28829fee7d16da8b73e7657bdb64250cf8df832955c1`), `AppleAudio.sys` (SHA256 `cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150`). Адреса указаны как VA при ImageBase `0x140000000`. Сами бинарники в репозиторий не входят.

**Статус: гипотеза с сильными доказательствами из дизассемблера; на железе не подтверждена.**

## Симптом (из `CURRENT_STATE.md`, 2026-10-08)

`0x7E`, `0xC0000005`, чтение `NULL+0x18` в `ks!DispatchDeviceIoControl+0x15`. Стек: `AppleUSBVHCI+0x259a → ksthunk → ks`, системный worker thread. Падали оба варианта нашего IRP: с `FileObject = NULL` (01:35) и с выставленным `FileObject` (01:48).

## 1. Кто строит IRP, на котором происходит сбой

Функция AppleUSBVHCI, начинающаяся в `0x1400023F0`:

| VA | Что происходит |
|---|---|
| `0x14000242C`–`0x14000244B` | на стеке собирается GUID `{6994AD04-93EF-11D0-A3CC-00A0C9223196}` = **`KSCATEGORY_AUDIO`** |
| `0x140002477` | `IoGetDeviceInterfaces(&KSCATEGORY_AUDIO, …)` → список символических ссылок (MULTI_SZ) |
| `0x1400024A2`–`0x1400024EB` | перебор строк в поисках подстроки `VEN_106B` (по символам `V`,`E`,`N`,`_`,`1`,`0`,`6`,`B`) |
| `0x14000252C` | `IoGetDeviceObjectPointer(найденная ссылка, 0x80, &FileObject, &DeviceObject)` |
| `0x140002566` | `ecx = 0x222020` |
| `0x140002582` | `IoBuildDeviceIoControlRequest(0x222020, DeviceObject, rbx, [rbx+0x2000], NULL, 0, FALSE, &Event, &Iosb)` — `FileObject` в стеке IRP **не** выставляется |
| `0x140002594` | `IofCallDriver(DeviceObject, Irp)` |
| **`0x14000259A`** | **адрес возврата из `IofCallDriver` — `AppleUSBVHCI+0x259a` из дампа** |
| `0x1400025B4` | `KeWaitForSingleObject` при `STATUS_PENDING` |

Вывод: AppleUSBVHCI **сам** доставляет сообщение от bridgeOS (буфер `rbx`, длина хранится по `+0x2000`) аудиоустройству Apple. Получатель — первый интерфейс `KSCATEGORY_AUDIO` с `VEN_106B` в пути. Когда установлен T2AudioMiniport, это наш Wave/Topology-интерфейс. `IoGetDeviceObjectPointer` возвращает верх стека, то есть class upper filter `ksthunk` (Media class).

## 2. Как это обрабатывает AppleAudio

`AppleAudio.sys`, обработчик по адресу `0x140001280`. Его адрес кладётся в `DriverObject->MajorFunction[…]` в `DriverEntry` (`0x14001E323`: `lea rax, 0x140001280`).

| VA | Что происходит |
|---|---|
| `0x14000128F` | `stack = Irp->Tail.Overlay.CurrentStackLocation` |
| `0x1400012AD` | `if (stack->Parameters.DeviceIoControl.IoControlCode != 0x222020)` → `0x1400012A0`: вызов `[0x14000E228]` (передача в PortCls) |
| `0x1400012B6` | `buf = Irp->AssociatedIrp.SystemBuffer` |
| `0x1400012BF` | `msg = *(u32*)(buf + 0x0D)` — поле `base.msg` (header 13 байт) |
| `0x1400012C2` | `msg == 4` (**`UPDATE_TIMESTAMP`**) → `0x14000435C(ctx, *(u64*)(buf+5) /*device_id*/, buf)` |
| `0x1400012C7` | `msg == 13` (**`PROPERTY_CHANGED`**) → сравнение `*(u32*)(buf+0x21)` с `'outp'` / `'inpt'`, переключение флагов jack, сигнал событию |
| `0x140001349`–`0x14000134F` | `Irp->IoStatus.Status = 0`, `IofCompleteRequest` |

`0x14000435C` ищет устройство по `device_id` (шаг контекста `0xE970`), увеличивает счётчик `+0xE954` и сохраняет `*(u64*)(buf+0x15)` (первое поле payload — timestamp) в `+0xE958`. **Ответ T2 не отправляется.** На Linux kaiT2en отвечает `UPDATE_TIMESTAMP_RESPONSE` (34), на Windows этим, видимо, занимается сам AppleUSBVHCI (см. открытые вопросы).

Именно поэтому AppleAudio не падает: его обработчик стоит ниже `ksthunk`, который пропускает не-KS IOCTL вниз, и перехватывает `0x222020` до PortCls/KS.

## 3. Почему падал T2AudioMiniport

`IRP_MJ_DEVICE_CONTROL` у нас целиком принадлежал PortCls (`PcDispatchIrp` → `KsDispatchIrp`). KS ищет create item по `FileObject->FsContext`, а `FileObject` у этого IRP `NULL` → чтение `NULL+0x18` → `0x7E`.

Первое такое сообщение приходит сразу после `START_IO` (`UPDATE_TIMESTAMP`; kaiT2en отмечает, что оно может прийти даже раньше ответа на `START_IO`). Это объясняет всё сразу:

* падение возникает только на `-BceIo` (раньше `START_IO` T2 ничего не присылал);
* стек начинается в worker thread AppleUSBVHCI, а не в нашем потоке;
* изменение `FileObject` в **нашем** IRP `0x222018` ничего не меняло.

## 4. Исправление в коде

`src/Driver.c`: `T2AudioDeviceControl` назначен в `MajorFunction[IRP_MJ_DEVICE_CONTROL]`. Для `0x222020` он вызывает `T2AudioHandleVhciMessage` (`src/BceTransport.c`) и завершает IRP со `STATUS_SUCCESS`. Всё остальное уходит в `PcDispatchIrp`. Обработчик разбирает `UPDATE_TIMESTAMP` (timestamp, update seed, sampleTime → кадры) и сохраняет последние значения для speaker в контексте; прочие сообщения считает и логирует. Обратно ничего не отправляет и не блокируется. Разборщики — `T2AudioBceParseIncoming`, `T2AudioBceParseUpdateTimestamp`, `T2AudioBceDecodeSampleTime` в `src/BceProtocolLogic.h`, покрыты тестами.

## 5. Открытые вопросы (для проверки)

1. **Поле `tag`.** AppleAudio кладёт в первые 4 байта порядковый номер 1..127 (`*tag = *tag % 0x7F + 1` в `FUN_140004a4c`), kaiT2en — `'S' + 3 цифры`, а мы — константу `"Audt"`. Как AppleUSBVHCI сопоставляет ответы с запросами? Не отправляет ли он ответ с непризнанным тегом тоже через `0x222020`?
2. **Ответ на `UPDATE_TIMESTAMP`.** Отвечает ли сам AppleUSBVHCI `UPDATE_TIMESTAMP_RESPONSE`, или ответ не нужен вовсе? Если T2 ждёт ответа, а его нет, он может перестать слать отметки или сбросить I/O.
3. **Формат `PROPERTY_CHANGED`.** Что лежит по `+0x21` и рядом (scope / selector / element), какие события он сигналит.
4. **Выбор получателя.** Что будет, если в системе есть и наш, и AppleAudio интерфейс (`IoGetDeviceInterfaces` возвращает их в своём порядке)? Ищет ли VHCI ещё что-то помимо `VEN_106B`?
5. **Другие сообщения.** Что ещё приходит через `0x222020`: `NOTIFICATION_ALIVE`, `NOTIFICATION_BOOT`?
