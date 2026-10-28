#include "T2AudioMiniport.h"
#include "T2AudioSpeakerLogic.h"

// Registry subkeys under HKLM\SYSTEM\CurrentControlSet\Services. Parameters is
// persistent (diagnostic tuning). TestSession MUST be created as a volatile key
// by the test agent: the kernel discards volatile keys on reboot, so after a
// bugcheck the driver comes back with no test session and refuses to start.
#define T2AUDIO_PARAMETERS_KEY   L"T2AudioMiniport\\Parameters"
#define T2AUDIO_TESTSESSION_KEY  L"T2AudioMiniport\\TestSession"

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
            T2AudioUnmapResources(context);
        }
        break;
    default:
        break;
    }

    return PcDispatchIrp(DeviceObject, Irp);
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

NTSTATUS
T2AudioStartDevice(
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
    KeInitializeSpinLock(&context->HardwareLock);
    KeInitializeSpinLock(&context->ClockLock);

    // Never start in Safe Mode. This driver programs device memory, and a fault
    // here must not make the recovery environment unusable. SafeBootMode is set
    // by the kernel for every Safe Mode variant.
    if (SharedUserData->SafeBootMode) {
        KdPrint(("T2Audio: Safe Mode detected - refusing to start device\n"));
        return STATUS_UNSUCCESSFUL;
    }

    // The experimental driver must not activate hardware on a normal boot.
    // Only an explicitly enabled volatile diagnostic session may start it.
    {
        ULONG sessionFlag = T2AudioReadDwordParameter(T2AUDIO_TESTSESSION_KEY,
                                                      L"EnableTestMode", 0);
        context->TestModeEnabled = (sessionFlag != 0);
        if (!context->TestModeEnabled) {
            KdPrint(("T2Audio: test session disabled - refusing to start device\n"));
            return STATUS_UNSUCCESSFUL;
        }
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
    // Off by default: the hardware-verified plain speaker path.
    context->SpeakerDsp = T2AudioReadDwordParameter(T2AUDIO_PARAMETERS_KEY, L"SpeakerDsp", 0);
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
    KdPrint(("T2Audio: mode parameters: ForceSystemBuffer=%u MdlVariant=%u TweeterMode=%u OutputAttenuation=%u (-%u dB) ClockRateTracking=%u SpeakerDsp=%u DspGainDb=%u TweeterTrimDb=%u WooferLimitDb=%u\n",
             context->ForceSystemBuffer ? 1u : 0u,
             context->MdlVariant, context->TweeterMode,
             context->OutputAttenuation, context->OutputAttenuation * 6u,
             context->ClockRateTracking ? 1u : 0u,
             context->SpeakerDsp, context->DspGainDb, context->TweeterTrimDb,
             context->WooferLimitDb));

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
    status = T2AudioCreateTopology(context, &topologyMiniport);
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
    status = T2AudioCreateMiniport(context, &miniport);
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
    } else {
        T2AudioCloseBceTransport();
        KdPrint(("T2Audio: audio path stays disabled (stage %u)\n",
                 context->DiagStage));
    }

    KdPrint(("T2Audio: StartDevice SUCCESS - Topology + WaveRT registered and physically connected\n"));
    return STATUS_SUCCESS;
}
