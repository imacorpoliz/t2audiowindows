#include "T2AudioMiniport.h"
#include "T2AudioSpeakerLogic.h"
#include "T2AudioCrashGuard.h"

// Registry subkeys under HKLM\SYSTEM\CurrentControlSet\Services. Parameters is
// persistent (diagnostic tuning). TestSession MUST be created as a volatile key
// by the test agent: the kernel discards volatile keys on reboot, so after a
// bugcheck the driver comes back with no test session and refuses to start.
#define T2AUDIO_PARAMETERS_KEY   L"T2AudioMiniport\\Parameters"
#define T2AUDIO_TESTSESSION_KEY  L"T2AudioMiniport\\TestSession"
#define T2AUDIO_PARAMETERS_PATH  L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\T2AudioMiniport\\Parameters"

// Diagnostic stages, cumulative (see T2AUDIO_DEVICE_CONTEXT::DiagStage).
#define T2AUDIO_STAGE_RAM_ONLY 1u
#define T2AUDIO_STAGE_BAR      2u
#define T2AUDIO_STAGE_BCE      3u
#define T2AUDIO_STAGE_HW_IO    4u
#define T2AUDIO_STAGE_MMIO     5u

NTSTATUS
T2AudioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject);

// PortCls has no "stop/remove" callback for the adapter, so without this hook
// the BAR mappings and the BCE file-object reference taken in StartDevice were
// never released on device removal or driver unload. Release them on every
// stop/removal path, then let PortCls process the IRP as usual.
static NTSTATUS
T2AudioPnpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_REMOVE_DEVICE:
        if (DeviceObject->DeviceExtension != NULL) {
            PT2AUDIO_DEVICE_CONTEXT context = T2AudioGetContext(DeviceObject);

            KdPrint(("T2Audio: PnP minor 0x%02X - releasing hardware\n",
                     stack->MinorFunction));

            // No SET_REMOTE_ACCESS here: AppleUSBVHCI owns remote access (it
            // sends mode 1 at init and on resume, mode 0 on power-down;
            // 0x140002864 and its callers). Turning it off from the audio
            // driver would leave AppleAudio without audio until VHCI
            // re-initializes.
            // The jack poll sends BCE requests: stop it before the transport
            // is closed with the resources.
            T2AudioStopJackPoll(context);
            T2AudioReleaseHeadphoneEvents(context);
            // The T2 must stop writing the mic ring before it is freed; this
            // needs the BCE transport, which UnmapResources closes.
            T2AudioMicUnregister(context);
            T2AudioUnmapResources(context);
            // Persistent mode: clean end of the session.
            T2AudioCrashGuardSetArmed(context, FALSE);
            // Unregister on stop too: the next start zeroes the context and
            // registers again.
            if (context->ShutdownRegistered) {
                IoUnregisterShutdownNotification(DeviceObject);
                context->ShutdownRegistered = FALSE;
            }
        }
        break;
    default:
        break;
    }

    return PcDispatchIrp(DeviceObject, Irp);
}

// System shutdown/restart (persistent mode only): the session ends cleanly,
// so the next boot must not count it as a crash.
static NTSTATUS
T2AudioShutdown(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    if (DeviceObject->DeviceExtension != NULL) {
        T2AudioCrashGuardSetArmed(T2AudioGetContext(DeviceObject), FALSE);
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// AppleUSBVHCI pushes every message bridgeOS sends on its own (for example
// UPDATE_TIMESTAMP right after START_IO) to the first KSCATEGORY_AUDIO
// interface whose path contains VEN_106B, as IOCTL 0x222020 built with
// IoBuildDeviceIoControlRequest - no FileObject. AppleAudio.sys intercepts
// that IOCTL in its own IRP_MJ_DEVICE_CONTROL handler. Passed on to PortCls,
// KS dereferences the NULL FileObject (FsContext at +0x18): this is the 0x7E
// bugcheck at ks!DispatchDeviceIoControl+0x15 seen on every START_IO run
// (AppleUSBVHCI+0x259a is the return from that IofCallDriver). Handle it here
// and complete it; everything else goes to PortCls.
static NTSTATUS
T2AudioDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    // Only AppleUSBVHCI's own IRPs (IoBuildDeviceIoControlRequest: kernel
    // mode, no FileObject) carry bridgeOS messages. A user-mode caller of the
    // same code on our KS filter must not be able to feed us fake timestamps;
    // its IRP has a FileObject and goes to KS like any other request.
    if (stack->Parameters.DeviceIoControl.IoControlCode ==
            IOCTL_APPLE_VHCI_AUDIO_MESSAGE &&
        Irp->RequestorMode == KernelMode) {
        PT2AUDIO_DEVICE_CONTEXT context = NULL;

        if (DeviceObject->DeviceExtension != NULL) {
            context = T2AudioGetContext(DeviceObject);
        }
        T2AudioHandleVhciMessage(context,
                                 (const UCHAR *)Irp->AssociatedIrp.SystemBuffer,
                                 stack->Parameters.DeviceIoControl.InputBufferLength);

        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    return PcDispatchIrp(DeviceObject, Irp);
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    
    KdPrint(("T2Audio: DriverEntry called\n"));

    // Initialize the BCE transport lock before any device can be started.
    T2AudioInitializeBceTransport();

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, T2AudioAddDevice);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcInitializeAdapterDriver failed: 0x%08X\n", status));
        return status;
    }

    // Must come after PcInitializeAdapterDriver, which installs PortCls's own
    // dispatch table; T2AudioPnpHandler forwards to PcDispatchIrp.
    DriverObject->MajorFunction[IRP_MJ_PNP] = T2AudioPnpHandler;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = T2AudioDeviceControl;
    // Only sent to a device that registered (persistent mode, see StartDevice).
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = T2AudioShutdown;

    KdPrint(("T2Audio: PortCls driver initialized\n"));
    return STATUS_SUCCESS;
}

NTSTATUS
T2AudioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;
    
    KdPrint(("T2Audio: AddDevice called\n"));

    status = PcAddAdapterDevice(
        DriverObject, 
        PhysicalDeviceObject,
        T2AudioStartDevice, 
        16,
        PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(T2AUDIO_DEVICE_CONTEXT));
        
    KdPrint(("T2Audio: PcAddAdapterDevice returned: 0x%08X\n", status));
    return status;
}

static NTSTATUS
T2AudioDwordQuery(
    _In_ PWSTR ValueName,
    _In_ ULONG ValueType,
    _In_ PVOID ValueData,
    _In_ ULONG ValueLength,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID EntryContext)
{
    UNREFERENCED_PARAMETER(ValueName);
    UNREFERENCED_PARAMETER(Context);

    if (EntryContext != NULL && ValueType == REG_DWORD &&
        ValueData != NULL && ValueLength >= sizeof(ULONG)) {
        *(PULONG)EntryContext = *(PULONG)ValueData;
    }
    return STATUS_SUCCESS;
}

// Reads a DWORD from
//   HKLM\SYSTEM\CurrentControlSet\Services\<SubKey>\<ValueName>
// returning DefaultValue when the value or the subkey is absent.
static ULONG
T2AudioReadDwordParameter(
    _In_ PCWSTR SubKey,
    _In_ PCWSTR ValueName,
    _In_ ULONG DefaultValue)
{
    RTL_QUERY_REGISTRY_TABLE table[2];
    ULONG value = DefaultValue;

    RtlZeroMemory(table, sizeof(table));
    table[0].QueryRoutine = T2AudioDwordQuery;
    table[0].Flags = 0;
    table[0].Name = (PWSTR)ValueName;
    table[0].EntryContext = &value;

    (VOID)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 (PWSTR)SubKey,
                                 table, NULL, NULL);
    return value;
}

// A registry DWORD that holds a signed dB value, clamped to -12..+12.
static LONG
T2AudioClampSignedDb(_In_ ULONG Raw)
{
    LONG v = (LONG)Raw;

    if (v > 12) {
        return 12;
    }
    if (v < -12) {
        return -12;
    }
    return v;
}

// Writes a DWORD to HKLM\SYSTEM\CurrentControlSet\Services\<SubKey>\<ValueName>.
static NTSTATUS
T2AudioWriteDwordParameter(
    _In_ PCWSTR SubKey,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value)
{
    return RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, SubKey, ValueName,
                                 REG_DWORD, &Value, sizeof(Value));
}

// Forces Parameters to disk, so the crash-guard flag survives a bugcheck
// that follows within seconds (the registry otherwise flushes lazily).
static VOID
T2AudioFlushParametersKey(VOID)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attributes;
    HANDLE key;

    RtlInitUnicodeString(&path, T2AUDIO_PARAMETERS_PATH);
    InitializeObjectAttributes(&attributes, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_WRITE, &attributes))) {
        (VOID)ZwFlushKey(key);
        ZwClose(key);
    }
}

// Persistent mode only: mark the hardware session as running (Armed) or as
// ended cleanly (not armed; also clears the strike counter). PASSIVE_LEVEL
// only (registry); silently skipped above it. See T2AudioCrashGuard.h.
VOID
T2AudioCrashGuardSetArmed(
    _In_ PT2AUDIO_DEVICE_CONTEXT Context,
    _In_ BOOLEAN Armed)
{
    if (Context == NULL || !Context->ProductionMode ||
        KeGetCurrentIrql() != PASSIVE_LEVEL || Context->CrashGuardArmed == Armed) {
        return;
    }
    (VOID)T2AudioWriteDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CrashGuardArmed", Armed ? 1u : 0u);
    if (!Armed) {
        (VOID)T2AudioWriteDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CrashGuardStrikes", 0);
    }
    T2AudioFlushParametersKey();
    Context->CrashGuardArmed = Armed;
    KdPrint(("T2Audio: crash guard %s\n", Armed ? "armed" : "disarmed (clean end of session)"));
}

static NTSTATUS
T2AudioStartDeviceInner(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList);

// Second topology + WaveRT pair for the headphones ("TopologyHeadphone",
// "WaveHeadphone"), wired like the speaker pair (Parameters\HeadphoneOutput).
static NTSTATUS
T2AudioRegisterHeadphoneFilters(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList,
    _In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    PPORT base = NULL;
    PPORTTOPOLOGY topoPort = NULL;
    PPORTWAVERT wavePort = NULL;
    PMINIPORTTOPOLOGY topoMiniport = NULL;
    PMINIPORTWAVERT waveMiniport = NULL;
    NTSTATUS status;

    PAGED_CODE();

    status = PcNewPort(&base, CLSID_PortTopology);
    if (NT_SUCCESS(status)) {
        status = base->QueryInterface(IID_IPortTopology, (PVOID *)&topoPort);
        base->Release();
        base = NULL;
    }
    if (NT_SUCCESS(status)) {
        status = T2AudioCreateTopology(Context, TRUE, &topoMiniport);
    }
    if (NT_SUCCESS(status)) {
        status = topoPort->Init(DeviceObject, Irp, (PUNKNOWN)topoMiniport, NULL, ResourceList);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"TopologyHeadphone", (PUNKNOWN)topoPort);
    }
    if (NT_SUCCESS(status)) {
        status = PcNewPort(&base, CLSID_PortWaveRT);
    }
    if (NT_SUCCESS(status)) {
        status = base->QueryInterface(IID_IPortWaveRT, (PVOID *)&wavePort);
        base->Release();
        base = NULL;
    }
    if (NT_SUCCESS(status)) {
        status = T2AudioCreateMiniport(Context, TRUE, &waveMiniport);
    }
    if (NT_SUCCESS(status)) {
        status = wavePort->Init(DeviceObject, Irp, (PUNKNOWN)waveMiniport, NULL, ResourceList);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(DeviceObject, L"WaveHeadphone", (PUNKNOWN)wavePort);
    }
    if (NT_SUCCESS(status)) {
        status = PcRegisterPhysicalConnection(DeviceObject,
                                              (PUNKNOWN)wavePort, T2AUDIO_WAVE_PIN_BRIDGE,
                                              (PUNKNOWN)topoPort, T2AUDIO_TOPO_PIN_BRIDGE);
    }
    if (waveMiniport != NULL) {
        waveMiniport->Release();
    }
    if (wavePort != NULL) {
        wavePort->Release();
    }
    if (topoMiniport != NULL) {
        topoMiniport->Release();
    }
    if (topoPort != NULL) {
        topoPort->Release();
    }
    return status;
}

// Start wrapper: a start that fails after the crash guard was armed is a
// clean end, not a crash.
NTSTATUS
T2AudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList)
{
    NTSTATUS status;

    PAGED_CODE();
    status = T2AudioStartDeviceInner(DeviceObject, Irp, ResourceList);
    if (!NT_SUCCESS(status) && DeviceObject != NULL && DeviceObject->DeviceExtension != NULL) {
        T2AudioCrashGuardSetArmed(T2AudioGetContext(DeviceObject), FALSE);
    }
    return status;
}

static NTSTATUS
T2AudioStartDeviceInner(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList)
{
    PT2AUDIO_DEVICE_CONTEXT context;
    NTSTATUS status;

    PAGED_CODE();
    KdPrint(("T2Audio: StartDevice entry\n"));

    // Validate parameters
    if (DeviceObject == NULL || Irp == NULL || ResourceList == NULL ||
        DeviceObject->DeviceExtension == NULL) {
        KdPrint(("T2Audio: Invalid parameters\n"));
        return STATUS_INVALID_PARAMETER;
    }
    context = T2AudioGetContext(DeviceObject);

    KdPrint(("T2Audio: All parameters valid\n"));

    // Release any mapping/transport left over from a previous start/stop cycle
    // before the context is reused. Without this a re-start would zero the
    // context and permanently leak the old BAR mappings and BCE file-object
    // reference. On the first start the device extension is already zeroed, so
    // this is a no-op.
    T2AudioUnmapResources(context);

    RtlZeroMemory(context, sizeof(*context));
    context->DeviceObject = DeviceObject;
    KeInitializeMutex(&context->StreamMutex, 0);
    KeInitializeSpinLock(&context->HardwareLock);
    KeInitializeSpinLock(&context->ClockLock);

    // Never start in Safe Mode. This driver programs device memory, and a fault
    // here must not make the recovery environment unusable. SafeBootMode is set
    // by the kernel for every Safe Mode variant.
    if (SharedUserData->SafeBootMode) {
        KdPrint(("T2Audio: Safe Mode detected - refusing to start device\n"));
        return STATUS_UNSUCCESSFUL;
    }

    // The driver activates hardware only in one of two modes:
    //  - a volatile diagnostic session (TestSession, made by the test agent,
    //    wiped on every reboot), or
    //  - the persistent mode (Parameters\EnableProduction=1, set by the
    //    installer) under the crash guard (T2AudioCrashGuard.h).
    // Otherwise a normal boot never touches the hardware.
    {
        ULONG sessionFlag = T2AudioReadDwordParameter(T2AUDIO_TESTSESSION_KEY,
                                                      L"EnableTestMode", 0);
        context->TestModeEnabled = (sessionFlag != 0);
        if (!context->TestModeEnabled &&
            T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"EnableProduction", 0) == 1) {
            ULONG armed = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CrashGuardArmed", 0);
            ULONG strikes = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CrashGuardStrikes", 0);
            unsigned int newStrikes = 0;
            int allow = T2AudioCrashGuardAllowStart(armed, strikes, &newStrikes);

            (VOID)T2AudioWriteDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CrashGuardStrikes", newStrikes);
            if (!allow) {
                (VOID)T2AudioWriteDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CrashGuardArmed", 0);
                T2AudioFlushParametersKey();
                KdPrint(("T2Audio: persistent mode: %u unclean ends in a row - refusing to start "
                         "(reset with the installer)\n", newStrikes));
                return STATUS_UNSUCCESSFUL;
            }
            if (armed) {
                KdPrint(("T2Audio: persistent mode: previous session did not end cleanly "
                         "(strike %u of %u)\n", newStrikes, T2AUDIO_CRASHGUARD_MAX_STRIKES));
            }
            // Same configuration as the hardware-verified diagnostic stage 5
            // with BCE I/O (stereo endpoint only).
            context->ProductionMode = TRUE;
            context->DiagStage = T2AUDIO_STAGE_MMIO;
            context->MmioCopyEnabled = TRUE;
            context->BceIoEnabled = TRUE;
            context->RawSpeakerChannels = FALSE;
            T2AudioCrashGuardSetArmed(context, TRUE);
            if (DeviceObject->DeviceExtension != NULL &&
                NT_SUCCESS(IoRegisterShutdownNotification(DeviceObject))) {
                context->ShutdownRegistered = TRUE;
            }
            KdPrint(("T2Audio: persistent mode active (stage %u, mmioCopy=1, bceIo=1)\n",
                     context->DiagStage));
        } else if (!context->TestModeEnabled) {
            KdPrint(("T2Audio: no test session and persistent mode off - refusing to start device\n"));
            return STATUS_UNSUCCESSFUL;
        }
    }
    if (context->TestModeEnabled) {
        context->DiagStage =
            T2AudioReadDwordParameter(T2AUDIO_TESTSESSION_KEY, L"DiagStage",
                                      T2AUDIO_STAGE_RAM_ONLY);
        context->MmioCopyEnabled =
            (context->DiagStage >= T2AUDIO_STAGE_MMIO) &&
            (T2AudioReadDwordParameter(T2AUDIO_TESTSESSION_KEY,
                                       L"EnableMmioCopy", 0) != 0);
        context->BceIoEnabled =
            (context->DiagStage >= T2AUDIO_STAGE_HW_IO) &&
            (T2AudioReadDwordParameter(T2AUDIO_TESTSESSION_KEY,
                                       L"EnableBceIo", 0) != 0);
        // Raw 6-channel endpoint: per-speaker diagnostics only (the client
        // drives the tweeters unfiltered). Volatile like the other gates.
        context->RawSpeakerChannels =
            (T2AudioReadDwordParameter(T2AUDIO_TESTSESSION_KEY,
                                       L"RawSpeakerChannels", 0) != 0);
        KdPrint(("T2Audio: diagnostic session active stage=%u mmioCopy=%u bceIo=%u rawChannels=%u\n",
                 context->DiagStage,
                 context->MmioCopyEnabled ? 1u : 0u,
                 context->BceIoEnabled ? 1u : 0u,
                 context->RawSpeakerChannels ? 1u : 0u));
    }

    context->ForceSystemBuffer =
        (T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY,
                                   L"ForceSystemBuffer", 0) != 0);
    context->MdlVariant =
        T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"MdlVariant", 0);
    // Stereo path: tweeters high-passed unless TweeterMode=0 (muted). Any
    // other value falls back to the high-pass.
    context->TweeterMode =
        (T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"TweeterMode",
                                   T2AUDIO_TWEETER_HIGHPASS) == T2AUDIO_TWEETER_MUTE)
            ? T2AUDIO_TWEETER_MUTE : T2AUDIO_TWEETER_HIGHPASS;
    // Safety cut in 6 dB steps before any sample reaches the speakers.
    context->OutputAttenuation = T2AudioClampAttenuation(
        T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"OutputAttenuation",
                                  T2AUDIO_ATTENUATION_DEFAULT));
    // Off by default: the hardware-verified path re-aims on drift only.
    context->ClockRateTracking =
        (T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"ClockRateTracking", 0) != 0);
    // Default 3 (KAIT2EN chain + virtual bass + multiband compressor), the
    // level the owner approved on hardware (2026-10-28, 1.0.11.0); 0 is the
    // plain path.
    context->SpeakerDsp = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"SpeakerDsp", 3);
    // Make-up gain for the DSP path (see T2AudioDsp.h); clamped to 0..12 dB.
    context->DspGainDb = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"DspGainDb", 6);
    if (context->DspGainDb > 12) {
        context->DspGainDb = 12;
    }
    context->TweeterTrimDb = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"TweeterTrimDb", 0);
    if (context->TweeterTrimDb > 12) {
        context->TweeterTrimDb = 12;
    }
    context->WooferLimitDb = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"WooferLimitDb", 0);
    if (context->WooferLimitDb > 12) {
        context->WooferLimitDb = 12;
    }
    // Autogain in front of the DSP chain (T2AudioDsp.h, KAIT2EN "Norm");
    // 0/1, default 0 (off: the chain is unchanged).
    context->DspAutoGain = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"DspAutoGain", 0) ? 1u : 0u;
    // Tone controls: a DWORD holding a signed dB value (PowerShell writes -3
    // as 0xFFFFFFFD), clamped to +-12; and the compressor detector style.
    context->BassDb = T2AudioClampSignedDb(T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"BassDb", 0));
    context->TrebleDb = T2AudioClampSignedDb(T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"TrebleDb", 0));
    context->ClarityDb = T2AudioClampSignedDb(T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"ClarityDb", 0));
    context->CompStyle = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"CompStyle", 0) ? 1u : 0u;

    // Groundwork for headphones and microphones (docs/ROADMAP.md sections 4-5),
    // all 0/1 and off by default: ExtendedProbe logs every BCE device and the
    // BufferStruct table (read-only); JackEvents subscribes to the headphone
    // jack and tracks it; HeadphoneAutoMute (with JackEvents) silences the
    // speakers while headphones are plugged in.
    context->ExtendedProbe = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"ExtendedProbe", 0) ? 1u : 0u;
    context->JackEvents = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"JackEvents", 0) ? 1u : 0u;
    // Experiment (read-only, needs JackEvents): read the jack at 8 addresses.
    context->JackProbeAll = (context->JackEvents &&
        T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"JackProbeAll", 0)) ? 1u : 0u;
    context->HeadphoneAutoMute = (context->JackEvents &&
        T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"HeadphoneAutoMute", 0)) ? 1u : 0u;
    // Headphone endpoint (ROADMAP 4.4) and resume after sleep (2.1): both off
    // by default, so the default configuration is unchanged.
    context->HeadphoneOutput = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"HeadphoneOutput", 0) ? 1u : 0u;
    context->HeadphoneAttenuation = T2AudioClampAttenuation(
        T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"HeadphoneAttenuation", 2));
    context->ResumeAfterSleep = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"ResumeAfterSleep", 0) ? 1u : 0u;
    // Built-in microphones (ROADMAP 5): off by default; MicDsp 1 = kaiT2en chain.
    context->MicInput = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"MicInput", 0) ? 1u : 0u;
    context->MicDsp = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"MicDsp", 1) ? 1u : 0u;
    // START_IO on the microphone only with this second, explicit gate: the
    // first try powered the Mac off (EXCERPT_20261029_part2_mics.md).
    context->MicStartIo = (context->MicInput &&
        T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"MicStartIo", 0)) ? 1u : 0u;
    context->ExtraFormats = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"ExtraFormats", 0) ? 1u : 0u;
    KdPrint(("T2Audio: headphone/mic/power parameters: HeadphoneOutput=%u HeadphoneAttenuation=%u (-%u dB) MicInput=%u MicStartIo=%u MicDsp=%u ExtraFormats=%u ResumeAfterSleep=%u JackProbeAll=%u\n",
             context->HeadphoneOutput, context->HeadphoneAttenuation,
             context->HeadphoneAttenuation * 6u, context->MicInput, context->MicStartIo, context->MicDsp,
             context->ExtraFormats, context->ResumeAfterSleep, context->JackProbeAll));
    KdPrint(("T2Audio: mode parameters: ForceSystemBuffer=%u MdlVariant=%u TweeterMode=%u OutputAttenuation=%u (-%u dB) ClockRateTracking=%u SpeakerDsp=%u DspGainDb=%u TweeterTrimDb=%u WooferLimitDb=%u DspAutoGain=%u BassDb=%d TrebleDb=%d ClarityDb=%d CompStyle=%u ExtendedProbe=%u JackEvents=%u HeadphoneAutoMute=%u\n",
             context->ForceSystemBuffer ? 1u : 0u,
             context->MdlVariant, context->TweeterMode,
             context->OutputAttenuation, context->OutputAttenuation * 6u,
             context->ClockRateTracking ? 1u : 0u,
             context->SpeakerDsp, context->DspGainDb, context->TweeterTrimDb,
             context->WooferLimitDb, context->DspAutoGain,
             context->BassDb, context->TrebleDb, context->ClarityDb, context->CompStyle,
             context->ExtendedProbe, context->JackEvents, context->HeadphoneAutoMute));

    // Stage 2 and up map the BARs and locate the speaker buffer. Below that the
    // driver runs purely on system memory and never touches a BAR.
    if (context->DiagStage >= T2AUDIO_STAGE_BAR) {
        KdPrint(("T2Audio: Mapping resources (stage %u)\n", context->DiagStage));
        status = T2AudioMapResources(context, ResourceList);
        if (!NT_SUCCESS(status)) {
            KdPrint(("T2Audio: MapResources failed: 0x%08X\n", status));
            return status;
        }
        KdPrint(("T2Audio: Resources mapped successfully\n"));
    } else {
        KdPrint(("T2Audio: stage %u - RAM only, no BAR mapping\n",
                 context->DiagStage));
    }
    
    // Phase 3.1: Register topology FIRST (defines audio path)
    PPORT topologyPortBase = NULL;
    PPORTTOPOLOGY topologyPort = NULL;
    PMINIPORTTOPOLOGY topologyMiniport = NULL;
    
    KdPrint(("T2Audio: Creating Topology port\n"));
    status = PcNewPort(&topologyPortBase, CLSID_PortTopology);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcNewPort(Topology) failed: 0x%08X\n", status));
        T2AudioUnmapResources(context);
        return status;
    }
    
    status = topologyPortBase->QueryInterface(IID_IPortTopology,
                                              (PVOID*)&topologyPort);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: QueryInterface IPortTopology failed: 0x%08X\n", status));
        topologyPortBase->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    topologyPortBase->Release();
    
    KdPrint(("T2Audio: Creating Topology miniport\n"));
    status = T2AudioCreateTopology(context, FALSE, &topologyMiniport);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: CreateTopology failed: 0x%08X\n", status));
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Initializing Topology port (topology FIRST, UnknownAdapter=NULL)\n"));
    status = topologyPort->Init(DeviceObject,
                                Irp, (PUNKNOWN)topologyMiniport,
                                NULL, ResourceList);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: Topology Port Init failed: 0x%08X\n", status));
        topologyMiniport->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Registering Topology subdevice\n"));
    status = PcRegisterSubdevice(DeviceObject, L"Topology", (PUNKNOWN)topologyPort);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcRegisterSubdevice(Topology) failed: 0x%08X\n", status));
        topologyMiniport->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    topologyMiniport->Release();
    topologyMiniport = NULL;

    // Phase 3: Create WaveRT port (connects to topology)
    PPORT portBase = NULL;
    PPORTWAVERT port = NULL;
    PMINIPORTWAVERT miniport = NULL;
    
    KdPrint(("T2Audio: Creating WaveRT port\n"));
    status = PcNewPort(&portBase, CLSID_PortWaveRT);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcNewPort failed: 0x%08X\n", status));
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    status = portBase->QueryInterface(IID_IPortWaveRT, (PVOID*)&port);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: QueryInterface IPortWaveRT failed: 0x%08X\n", status));
        portBase->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    portBase->Release();
    
    KdPrint(("T2Audio: Creating WaveRT miniport\n"));
    status = T2AudioCreateMiniport(context, FALSE, &miniport);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: CreateMiniport failed: 0x%08X\n", status));
        port->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Initializing WaveRT miniport\n"));
    status = port->Init(DeviceObject, Irp,
                        (PUNKNOWN)miniport, NULL, ResourceList);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: Port Init failed: 0x%08X\n", status));
        miniport->Release();
        port->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Registering WaveRT subdevice\n"));
    status = PcRegisterSubdevice(DeviceObject, L"Wave", (PUNKNOWN)port);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcRegisterSubdevice failed: 0x%08X\n", status));
        miniport->Release();
        port->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }
    
    miniport->Release();
    miniport = NULL;

    // Wire the WaveRT bridge pin to the topology bridge pin. This is what
    // makes the audio stack expose a speaker endpoint. Both port objects are
    // still referenced at this point (topologyPort and port).
    KdPrint(("T2Audio: Registering physical connection (Wave bridge -> Topology bridge)\n"));
    status = PcRegisterPhysicalConnection(DeviceObject,
                                          (PUNKNOWN)port, T2AUDIO_WAVE_PIN_BRIDGE,
                                          (PUNKNOWN)topologyPort, T2AUDIO_TOPO_PIN_BRIDGE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcRegisterPhysicalConnection failed: 0x%08X\n", status));
        port->Release();
        topologyPort->Release();
        T2AudioUnmapResources(context);
        return status;
    }

    port->Release();
    topologyPort->Release();

    // Headphone filter pair (Parameters\HeadphoneOutput=1 and the "Codec
    // Output" ring found). A failure only leaves the headphones out.
    if (context->HeadphoneOutput && context->HeadphoneBufferSize != 0) {
        NTSTATUS hpStatus = T2AudioRegisterHeadphoneFilters(DeviceObject, Irp, ResourceList, context);

        context->HeadphoneFiltersRegistered = NT_SUCCESS(hpStatus) ? TRUE : FALSE;
        KdPrint(("T2Audio: headphone endpoint %s (0x%08X)\n",
                 NT_SUCCESS(hpStatus) ? "registered" : "NOT registered", hpStatus));
    }

    // Microphone filter pair (Parameters\MicInput=1 and "Digital Mic" in
    // BufferStruct). A failure only leaves the microphone out.
    if (context->MicInput && context->HardwareReady && T2AudioMicPresentInBufferStruct(context)) {
        NTSTATUS micStatus = T2AudioRegisterMicFilters(DeviceObject, Irp, ResourceList, context);

        context->MicFiltersRegistered = NT_SUCCESS(micStatus) ? TRUE : FALSE;
        KdPrint(("T2Audio: microphone endpoint %s (0x%08X)\n",
                 NT_SUCCESS(micStatus) ? "registered" : "NOT registered", micStatus));
    }

    // STOP_IO before D3 (as AppleAudio does), no MMIO while powered down.
    // Failure is not fatal: streams still stop I/O when they leave RUN.
    (VOID)T2AudioRegisterPowerManagement(DeviceObject, context);

    // HardwareReady was set by a successful MapResources (stage >= 2). Do NOT
    // force it here: below stage 2 the context must stay "not hardware ready" so
    // the DPC and the I/O paths never touch a BAR.
    context->SpeakerDeviceId = 0;
    context->BceSpeakerDeviceId = 0;
    context->BceProbed = FALSE;

    // Enumerate BCE devices and log their UIDs (stage 3+). On success the
    // transport is left open so START_IO/STOP_IO can use it.
    if (context->DiagStage >= T2AUDIO_STAGE_BCE && context->HardwareReady) {
        (VOID)T2AudioProbeBceDevices(context);
        if (context->ExtendedProbe) {
            T2AudioLogBufferStruct(context);
        }
    } else {
        KdPrint(("T2Audio: stage %u - BCE discovery skipped\n",
                 context->DiagStage));
    }

    // Wire the discovered speaker into the audio path only at stage 4+. Below
    // that the endpoint stays in system-buffer mode and START_IO/STOP_IO cannot
    // run (SpeakerDeviceId stays 0). The transport is held open at stage 4 even
    // when EnableBceIo=0, so a run can prove that holding it open across the
    // stream lifecycle is safe before any command is sent.
    if (context->DiagStage >= T2AUDIO_STAGE_HW_IO &&
        context->BceProbed && context->BceSpeakerDeviceId != 0) {
        context->SpeakerDeviceId = context->BceSpeakerDeviceId;

        // Remote access is not requested here. AppleUSBVHCI already sends
        // NOTIFICATION_ALIVE and SET_REMOTE_ACCESS(mode 1) itself when it
        // initializes and on resume (0x140002690 / 0x140002864), and
        // AppleAudio.sys never sends SET_REMOTE_ACCESS. Sending kaiT2en's
        // mode 2 would change a state the Windows stack set up.

        KdPrint(("T2Audio: audio path ENABLED for speaker 0x%I64X (bceIo=%u, transport held open)\n",
                 context->SpeakerDeviceId, context->BceIoEnabled ? 1u : 0u));
        if (context->HeadphoneFiltersRegistered && context->HeadphoneFormatValid &&
            context->BceHeadphoneDeviceId != 0) {
            context->HeadphoneDeviceId = context->BceHeadphoneDeviceId;
            KdPrint(("T2Audio: audio path ENABLED for headphones 0x%I64X ring 0x%Ix+0x%Ix\n",
                     context->HeadphoneDeviceId, context->HeadphoneBufferOffset,
                     context->HeadphoneBufferSize));
        }
        // Microphones: register the host ring with the T2 (writes BufferStruct
        // and sends msg 18), only in the BCE I/O configuration.
        if (context->MicFiltersRegistered && context->MicFormatValid &&
            context->BceMicDeviceId != 0 && context->BceIoEnabled) {
            NTSTATUS micStatus = T2AudioMicRegister(context);

            if (NT_SUCCESS(micStatus)) {
                context->MicDeviceId = context->BceMicDeviceId;
            }
            KdPrint(("T2Audio: audio path %s for microphones 0x%I64X (0x%08X)\n",
                     NT_SUCCESS(micStatus) ? "ENABLED" : "NOT enabled",
                     context->BceMicDeviceId, micStatus));
        }
    } else {
        T2AudioCloseBceTransport();
        KdPrint(("T2Audio: audio path stays disabled (stage %u)\n",
                 context->DiagStage));
    }

    KdPrint(("T2Audio: StartDevice SUCCESS - Topology + WaveRT registered and physically connected\n"));
    return STATUS_SUCCESS;
}
