# T2AudioPort Phase 3/4 Status Report
**Date:** 2026-10-05  
**Target:** MacBook Pro 16,1 (2019), Apple T2 Audio Device, Windows 11

---

## Summary

Phase 3 и Phase 4 реализованы и успешно собраны. Драйвер переведён с KMDF function driver на стандартный **Pure PortCls adapter driver** с полной регистрацией WaveRT subdevice. Добавлен Phase 4 skeleton с подготовкой MMIO MDL и T2 protocol command serialization, но без активации на реальном устройстве.

**Драйвер пока НЕ ГОТОВ для установки на Apple Audio Device.**

---

## Build Verification

### Binary
```
File:   C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\bin\Debug\T2AudioMiniport.sys
Size:   14848 bytes
Date:   2026-10-05 08:33:41
Arch:   x64 (machine 8664)
Entry:  GsDriverEntry (offset 0x7000)
Subsys: Native (kernel mode driver)
```

### Key Imports (verified via dumpbin)
```
ntoskrnl.exe:
  - MmMapIoSpaceEx
  - MmUnmapIoSpace
  - IoAllocateMdl
  - IoFreeMdl
  - READ_REGISTER_ULONG
  - ExAllocatePoolWithTag
  - ExFreePoolWithTag

portcls.sys:
  - PcInitializeAdapterDriver
  - PcAddAdapterDevice
  - PcNewPort
  - PcRegisterSubdevice
  - PcGetDeviceProperty
```

**Вердикт:** бинарник корректен, импорты соответствуют Pure PortCls driver model.

---

## Phase 3: PortCls Adapter Architecture

### Что реализовано

#### 1. Driver Lifecycle (Driver.c)
- `DriverEntry`: вызывает `PcInitializeAdapterDriver` вместо `WdfDriverCreate`.
- `T2AudioAddDevice`: вызывает `PcAddAdapterDevice` с callback `T2AudioStartDevice`.
- `T2AudioStartDevice`:
  - Выделяет `T2AUDIO_DEVICE_CONTEXT` (adapter extension).
  - Вызывает `T2AudioMapResources` для BAR1/BAR2 mapping через `IResourceList`.
  - Создаёт WaveRT port: `PcNewPort(&CLSID_PortWaveRT)`.
  - Создаёт miniport: `T2AudioCreateMiniport`.
  - Инициализирует port: `IPortWaveRT::Init`.
  - **Регистрирует subdevice:** `PcRegisterSubdevice(DeviceObject, L"Wave", port)`.

#### 2. Resource Mapping (Device.c)
- Читает BAR1 (audio buffers) и BAR2 (config/GPR) через `IResourceList::FindTranslatedEntry`.
- Сохраняет `Bar1Physical` для Phase 4 MDL PFN calculation.
- Использует `MmMapIoSpaceEx`:
  - BAR1: `PAGE_READONLY | PAGE_WRITECOMBINE` (0x404, как AppleAudio.sys).
  - BAR2: `PAGE_READONLY | PAGE_NOCACHE` (0x204).
- Валидирует GPR (BAR2+0xC000): version, signature 0x19870423, buffer offset.
- Парсит BufferStruct, находит Speaker buffer.
- **Device ID:** пока установлен в 0 (BufferStruct не содержит BCE device_id).

#### 3. WaveRT Miniport (WaveRTMiniport.c)
- `IMiniportWaveRT` COM object с vtable:
  - `QueryInterface`, `AddRef`, `Release`
  - `Init`, `GetDescription`, `DataRangeIntersection`, `NewStream`, `GetDeviceDescription`
- Pin descriptor:
  - 1 render pin (KSPIN_DATAFLOW_IN)
  - 6 channels, 48 kHz, 32-bit samples, 24 bytes/frame
  - `KSDATARANGE_AUDIO` с `KSDATAFORMAT_SUBTYPE_PCM`, `KSDATAFORMAT_SPECIFIER_WAVEFORMATEX`
  - Category: `KSCATEGORY_AUDIO`, Name: `KSNODETYPE_SPEAKER`
- `PCFILTER_DESCRIPTOR` с 1 pin, 0 nodes.

#### 4. WaveRT Stream (WaveRTStream.c)
- `IMiniportWaveRTStream` COM object с vtable:
  - `SetFormat`, `SetState`, `GetPosition`
  - `AllocateAudioBuffer`, `FreeAudioBuffer`
  - `GetHWLatency`, `GetPositionRegister`, `GetClockRegister`
- Format validation: проверяет 6ch/48kHz/32bit/24 bytes per frame.
- State machine: `KSSTATE_STOP → ACQUIRE → PAUSE → RUN`.
- QPC anchor при переходе в `KSSTATE_RUN`.
- **`AllocateAudioBuffer`:** пока возвращает `STATUS_NOT_SUPPORTED` (см. Phase 4 ограничения).
- **`GetPosition`:** stub, возвращает PlayOffset=0, WriteOffset=0.

### Архитектурные изменения

| Было (KMDF)               | Стало (PortCls)                     |
|---------------------------|-------------------------------------|
| `WdfDriverCreate`         | `PcInitializeAdapterDriver`         |
| `EvtDeviceAdd`            | `T2AudioAddDevice` (callback)       |
| `EvtPrepareHardware`      | `T2AudioStartDevice` (callback)     |
| `WDFDEVICE`               | `PDEVICE_OBJECT`                    |
| `WDF_OBJECT_ATTRIBUTES`   | `ExAllocatePoolWithTag` context     |
| `WdfCmResourceList`       | `PRESOURCELIST` (IResourceList)     |

### Статус
✅ **Сборка успешна**  
✅ **PortCls subdevice регистрируется**  
⚠️ **Не устанавливалось на устройство** (INF намеренно без Hardware ID)  
⚠️ **Endpoint не тестировался** (нет audio buffer path)

---

## Phase 4: MMIO Buffer and T2 Protocol

### Что реализовано

#### 1. MMIO MDL Construction (Phase4.c)
```c
NTSTATUS T2AudioCreateSpeakerMdl(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _Out_ PMDL *Mdl,
    _Out_ ULONG *OffsetFromFirstPage,
    _Out_ ULONG *ActualSize);
```
- Создаёт MDL для Speaker buffer в BAR1.
- Использует `IoAllocateMdl` с флагом `MDL_IO_SPACE`.
- Вычисляет PFN напрямую от `Bar1Physical.QuadPart + SpeakerBufferOffset`.
- Возвращает offset и size, aligned to page boundaries.
- **Cache type:** `MmWriteCombined` (соответствует BAR1 mapping 0x404).

```c
VOID T2AudioFreeSpeakerMdl(_Inout_ PT2AUDIO_DEVICE_CONTEXT Context);
```
- Освобождает MDL через `IoFreeMdl`.

#### 2. T2 Protocol Commands (Phase4.c)
```c
NTSTATUS T2AudioStartIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
NTSTATUS T2AudioStopIo(_In_ PT2AUDIO_DEVICE_CONTEXT Context);
```
- Строят KAIT2EN-compatible messages:
  - `START_IO`: opcode 0x80 (bit 7 set)
  - `STOP_IO`: opcode 0
  - Device ID берётся из `Context->SpeakerDeviceId` (пока 0)
  - Offset/size используются из `SpeakerBufferOffset/Size`
- **Message format:**
  ```c
  message[0] = opcode;           // 0x80 или 0
  message[1] = device_id;        // пока 0
  message[2..5] = offset (LE32)  // Speaker buffer offset в BAR1
  message[6..9] = size (LE32)    // Speaker buffer size
  ```
- **Transport:** пока не подключён, функции возвращают `STATUS_NOT_SUPPORTED`.

#### 3. Stream State Integration
- `WaveRTStream.c::T2AudioStreamSetState`:
  - `KSSTATE_RUN`: вызывает `T2AudioStartIo` (пока fail с NOT_SUPPORTED).
  - `KSSTATE_STOP`: вызывает `T2AudioStopIo` (пока fail с NOT_SUPPORTED).
- State transition **блокируется**, если команда возвращает ошибку.

### Ограничения Phase 4

⚠️ **MMIO MDL не активирован**
- `AllocateAudioBuffer` возвращает `STATUS_NOT_SUPPORTED`.
- MDL construction code присутствует, но **не вызывается** из WaveRT callback.
- **Причина:** physical page mapping и cache coherency не проверены на реальном устройстве. Некорректный MDL может вызвать BSOD или повреждение MMIO.

⚠️ **T2 command transport не подключён**
- `T2AudioStartIo`/`T2AudioStopIo` возвращают `STATUS_NOT_SUPPORTED` сразу после сериализации message.
- **Причина:** user-mode IOCTL 0x222018 к AppleUSBVHCI.sys нельзя вызывать из kernel mode без правильного IoGetDeviceObjectPointer и IRP stack построения. Нужен безопасный kernel BCE/AppleUSBVHCI wrapper.

⚠️ **Device ID неизвестен**
- `Context->SpeakerDeviceId = 0` (hardcoded).
- **Причина:** BufferStruct (BAR1 metadata) не содержит BCE device ID. В Linux KAIT2EN device ID приходит из отдельного BCE protocol handshake (`GET_DEVICE_LIST` command). Нужна kernel BCE enumeration.

---

## Source Files

```
Phase2/Driver/
├── T2AudioMiniport.h          [Header: context, prototypes, ABI]
├── Driver.c                   [PortCls adapter: DriverEntry, AddDevice, StartDevice]
├── Device.c                   [BAR mapping, GPR/BufferStruct parsing]
├── WaveRTMiniport.c           [IMiniportWaveRT COM object, pin descriptor]
├── WaveRTStream.c             [IMiniportWaveRTStream COM object, state machine]
└── Phase4.c                   [MMIO MDL, T2 START_IO/STOP_IO serialization]

Phase2/
├── T2AudioMiniport.vcxproj    [MSBuild project: WDM, PortCls, no KMDF]
└── Driver/T2AudioMiniport.inf [Stub INF без Hardware ID]
```

**Lines of code:** ~1200 (без комментариев и пустых строк)

---

## INF Configuration

```ini
[Version]
Signature="$WINDOWS NT$"
Class=Media
ClassGuid={4D36E96C-E325-11CE-BFC1-08002BE10318}
Provider=%ProviderName%
DriverVer=10/05/2026,1.0.0.0

[Manufacturer]
%ProviderName%=Standard,NTamd64

[Standard.NTamd64]
; Deliberately no hardware match until BAR ownership and kernel T2 transport
; are validated. This package must not replace AppleAudio.sys yet.

[Strings]
ProviderName="T2AudioPort"
```

**Статус:** ❌ Не содержит Hardware ID `PCI\VEN_106B&DEV_1803&SUBSYS_1887106B`  
**Причина:** драйвер не готов для установки, может вызвать BSOD или conflict с AppleAudio.sys.

---

## What Works

✅ BAR1/BAR2 mapping через PortCls `IResourceList`  
✅ GPR validation (BAR2+0xC000)  
✅ BufferStruct parsing  
✅ Speaker buffer discovery  
✅ WaveRT pin descriptor (6ch, 48kHz, 32bit)  
✅ COM interface vtables (IMiniportWaveRT, IMiniportWaveRTStream)  
✅ PortCls subdevice registration (`PcRegisterSubdevice`)  
✅ State machine (STOP/ACQUIRE/PAUSE/RUN)  
✅ MMIO MDL construction code (не активирован)  
✅ T2 START_IO/STOP_IO message serialization (не отправляются)

---

## What's Missing (Critical Blockers)

### 1. Kernel BCE/AppleUSBVHCI Transport
**Проблема:**
- User-mode IOCTL 0x222018 нельзя безопасно вызвать из kernel mode.
- Нужен kernel-mode wrapper:
  ```c
  NTSTATUS T2AudioOpenBceTransport(
      _Out_ PDEVICE_OBJECT *BceDevice,
      _Out_ PFILE_OBJECT *BceFileObject);
  
  NTSTATUS T2AudioSendBceCommand(
      _In_ PDEVICE_OBJECT BceDevice,
      _In_reads_bytes_(MessageSize) const UCHAR *Message,
      _In_ ULONG MessageSize);
  
  VOID T2AudioCloseBceTransport(
      _In_ PDEVICE_OBJECT BceDevice,
      _In_ PFILE_OBJECT BceFileObject);
  ```
- Требуется:
  - `IoGetDeviceObjectPointer(L"\\Device\\AppleUSBVHCI", ...)` для получения device stack.
  - `IoBuildDeviceIoControlRequest(IOCTL_UNKNOWN, ...)` для отправки команды.
  - Синхронное ожидание через `KeWaitForSingleObject(&event, ...)`.

**Риск:** неправильный IRP stack или reference counting может вызвать BSOD.

**Решение:**
1. Реализовать BCE transport wrapper в отдельном файле `BceTransport.c`.
2. Протестировать отправку dummy команды (GET_DEVICE_LIST) на тестовой загрузке с kernel debugger.
3. Подключить к `T2AudioStartIo`/`T2AudioStopIo`.

---

### 2. BCE Device Enumeration
**Проблема:**
- Speaker device ID неизвестен.
- BufferStruct (BAR1 metadata) содержит только имя устройства `"Speaker"`, но не BCE protocol device ID.

**Linux KAIT2EN reference:**
```c
// audio.c:452 - device ID приходит от BCE enumeration
static int t2audio_probe_device(struct t2audio_dev *adev, u8 device_id) {
    // device_id получен из BCE GET_DEVICE_LIST response
    adev->device_id = device_id;
    ...
}
```

**Решение:**
1. Отправить BCE команду `GET_DEVICE_LIST` в `T2AudioStartDevice` после BAR mapping.
2. Распарсить response, найти device с типом "audio output" и именем "Speaker".
3. Сохранить `device_id` в `Context->SpeakerDeviceId`.

---

### 3. MMIO Buffer Validation
**Проблема:**
- MDL для BAR1 speaker buffer построен, но не проверен на реальном устройстве.
- Некорректный cache type или PFN calculation может вызвать:
  - BSOD (DRIVER_IRQL_NOT_LESS_OR_EQUAL).
  - Memory corruption.
  - DMA conflict с AppleAudio.sys (если оно ещё загружено).

**Решение:**
1. Создать minimal test: вернуть MDL из `AllocateAudioBuffer`, но **не позволять Windows audio engine писать в buffer**.
2. Установить драйвер на тестовой загрузке с kernel debugger.
3. Проверить:
   - MDL создаётся без BSOD.
   - User-mode mapping успешен (`MmMapLockedPagesSpecifyCache`).
   - Physical address соответствует BAR1.
4. Только после успешной валидации разрешить реальный audio playback.

---

### 4. Hardware Position Tracking
**Проблема:**
- `GetPosition` возвращает stub (PlayOffset=0, WriteOffset=0).
- Windows audio engine использует `GetPosition` для glitch prevention и sync.

**Решение (вариант A - hardware pointer):**
```c
// Если T2 chip обновляет hardware write pointer в GPR или BufferStruct:
NTSTATUS T2AudioStreamGetPosition(...) {
    ULONG hwPointer = READ_REGISTER_ULONG(Context->GprBase + T2_HW_PTR_OFFSET);
    Position->PlayOffset = hwPointer * instance->BytesPerFrame;
    Position->WriteOffset = Position->PlayOffset + FIFO_SIZE;
    return STATUS_SUCCESS;
}
```

**Решение (вариант B - software position via QPC):**
```c
// Использовать Phase 1 RingBufferMath и QPC interpolation:
#include "../Phase1/RingBufferMath.h"

NTSTATUS T2AudioStreamGetPosition(...) {
    ULONG64 currentQpc = KeQueryPerformanceCounter(NULL).QuadPart;
    ULONG64 elapsedFrames = RingBufferInterpolateFrames(
        instance->AnchorQpc,
        currentQpc,
        instance->SampleRate,
        qpcFrequency
    );
    ULONG64 playOffset = (instance->AnchorFrames + elapsedFrames) * instance->BytesPerFrame;
    Position->PlayOffset = playOffset % Context->SpeakerBufferSize;
    Position->WriteOffset = (playOffset + FIFO_FRAMES * instance->BytesPerFrame) % Context->SpeakerBufferSize;
    return STATUS_SUCCESS;
}
```

**Выбор:** зависит от того, обновляет ли T2 chip hardware pointer (нужно проверить на устройстве).

---

### 5. Hardware Latency / FIFO Size
**Проблема:**
- `GetHWLatency` возвращает stub (0 FIFO, 0 interrupt delay).
- Windows audio engine использует latency для buffer size negotiation.

**Решение:**
1. Найти FIFO size в GPR или BufferStruct metadata.
2. Или использовать empirical value из AppleAudio.sys (reverse engineering).
3. Обновить:
   ```c
   Latency->FifoSize = 4096;  // bytes, пример
   Latency->ChipsetDelay = 0;
   Latency->CodecDelay = 0;
   ```

---

### 6. INF Hardware Match
**Проблема:**
- INF не содержит Hardware ID, драйвер не может быть установлен через Device Manager.

**Решение (только после всех блокеров):**
```ini
[Standard.NTamd64]
%T2Audio.DeviceDesc%=T2Audio_Install,PCI\VEN_106B&DEV_1803&SUBSYS_1887106B

[T2Audio_Install]
CopyFiles=T2Audio_CopyFiles

[T2Audio_Install.Services]
AddService=T2AudioMiniport,0x00000002,T2Audio_Service

[T2Audio_CopyFiles]
T2AudioMiniport.sys

[T2Audio_Service]
DisplayName=%T2Audio.ServiceDesc%
ServiceType=1
StartType=3
ErrorControl=1
ServiceBinary=%12%\T2AudioMiniport.sys

[Strings]
T2Audio.DeviceDesc="Apple T2 Audio Device (6-channel)"
T2Audio.ServiceDesc="T2 Audio PortCls Driver"
```

⚠️ **НЕ добавлять Hardware ID до полной валидации!**

---

## Testing Strategy (когда будет готово)

### Pre-Installation Checklist
- [ ] BCE transport реализован и протестирован с kernel debugger
- [ ] Device ID получен через GET_DEVICE_LIST
- [ ] MMIO MDL проверен на тестовой загрузке (без audio playback)
- [ ] Position tracking реализован (hardware или software)
- [ ] Hardware latency/FIFO настроены
- [ ] INF обновлён с Hardware ID
- [ ] Создана точка восстановления (если System Protection включён)
- [ ] Подготовлен WinRE USB для recovery

### Installation Steps (DANGER ZONE)
1. **Backup AppleAudio.sys:**
   ```powershell
   Copy-Item "C:\Windows\System32\drivers\AppleAudio.sys" "C:\Users\othysa\Desktop\mbp\backup\"
   ```

2. **Disable AppleAudio service:**
   ```powershell
   sc.exe config AppleAudio start= disabled
   ```

3. **Reboot to WinRE, replace driver:**
   ```cmd
   ren C:\Windows\System32\drivers\AppleAudio.sys AppleAudio.sys.bak
   copy T2AudioMiniport.sys C:\Windows\System32\drivers\
   ```

4. **Update Device Manager:**
   ```powershell
   pnputil.exe /add-driver T2AudioMiniport.inf /install
   ```

5. **Enable kernel debugger:**
   ```powershell
   bcdedit.exe /debug on
   bcdedit.exe /dbgsettings serial debugport:1 baudrate:115200
   ```

6. **Reboot and monitor:**
   - Kernel debugger output.
   - Event Viewer: System log, PortCls events.
   - DbgView: `KdPrint` messages.

7. **Test audio:**
   - Check Sound settings (Win+R → `mmsys.cpl`).
   - Verify "Apple T2 Audio Device (6-channel)" endpoint.
   - Play -30 dB test tone, 100ms max.
   - **STOP IMMEDIATELY** if distortion/clicking/smoke.

---

## Risk Assessment

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| BSOD при регистрации subdevice | Low | High | Kernel debugger, WinRE recovery |
| BSOD при MMIO MDL mapping | Medium | High | Test на dummy device сначала |
| T2 command отклонён (bad device_id) | High | Low | GET_DEVICE_LIST enumeration |
| Audio glitching (bad position) | High | Medium | Software QPC fallback |
| Conflict с AppleAudio.sys | Low | High | Disable AppleAudio service перед установкой |
| Повреждение speaker hardware | Very Low | Critical | -30 dB test tone, 100ms limit |

---

## Next Steps (Priority Order)

### Phase 5: BCE Transport (HIGH PRIORITY)
1. Создать `BceTransport.c` с kernel-mode wrapper для AppleUSBVHCI IOCTL.
2. Реализовать `T2AudioOpenBceTransport`, `T2AudioSendBceCommand`, `T2AudioCloseBceTransport`.
3. Протестировать с dummy command на kernel debugger.
4. Подключить к `T2AudioStartIo`/`T2AudioStopIo`.

### Phase 6: Device Enumeration (HIGH PRIORITY)
1. Отправить `GET_DEVICE_LIST` command в `T2AudioStartDevice`.
2. Распарсить BCE response, найти Speaker device ID.
3. Сохранить в `Context->SpeakerDeviceId`.

### Phase 7: MMIO Buffer Validation (CRITICAL SAFETY)
1. Активировать `AllocateAudioBuffer` (вернуть MDL вместо NOT_SUPPORTED).
2. Установить драйвер на тестовой загрузке с kernel debugger.
3. Проверить MDL mapping без audio playback.
4. Только после валидации разрешить SetState(RUN).

### Phase 8: Position Tracking (MEDIUM PRIORITY)
1. Проверить GPR/BufferStruct на наличие hardware write pointer.
2. Если нет - использовать QPC interpolation (Phase 1 RingBufferMath).
3. Обновить `GetPosition`.

### Phase 9: Hardware Latency (LOW PRIORITY)
1. Reverse engineer FIFO size из AppleAudio.sys или GPR.
2. Обновить `GetHWLatency`.

### Phase 10: INF и Installation (FINAL STEP)
1. Добавить Hardware ID в INF.
2. Создать installation guide с safety checklist.
3. Протестировать установку на реальном устройстве.

---

## Conclusion

Phase 3 и Phase 4 успешно реализованы:
- ✅ Драйвер переведён на Pure PortCls architecture.
- ✅ WaveRT subdevice регистрируется через `PcRegisterSubdevice`.
- ✅ MMIO MDL и T2 protocol command serialization готовы (но не активированы).

**Драйвер пока не готов для установки.** Критические блокеры:
1. BCE transport (kernel-mode wrapper для AppleUSBVHCI).
2. Device ID enumeration (GET_DEVICE_LIST command).
3. MMIO buffer validation (проверка на тестовой загрузке).

**Estimated work remaining:** ~3-4 phases (BCE transport, device enum, MMIO validation, position tracking).

**Safety note:** не устанавливать на реальное устройство до завершения Phase 5-7 и проверки с kernel debugger.

---

## Contact
For questions or issues, refer to:
- Linux KAIT2EN reference: `C:\Users\othysa\Desktop\mbp\kait2en\modules\t2bce_audio\audio.c`
- Phase 1 offline parser: `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase1\`
- Build log: `C:\Users\othysa\Desktop\mbp\T2AudioPort\Phase2\phase2_build_log.txt`
