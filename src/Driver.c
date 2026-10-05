#include "T2AudioMiniport.h"

NTSTATUS
T2AudioAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject);

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    
    KdPrint(("T2Audio: DriverEntry called\n"));

    status = PcInitializeAdapterDriver(DriverObject, RegistryPath, T2AudioAddDevice);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcInitializeAdapterDriver failed: 0x%08X\n", status));
        return status;
    }

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

NTSTATUS
T2AudioStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PRESOURCELIST ResourceList)
{
    PT2AUDIO_DEVICE_CONTEXT context = T2AudioGetContext(DeviceObject);
    NTSTATUS status;

    PAGED_CODE();
    KdPrint(("T2Audio: StartDevice entry\n"));
    
    // Validate parameters
    if (DeviceObject == NULL || Irp == NULL || ResourceList == NULL || context == NULL) {
        KdPrint(("T2Audio: Invalid parameters\n"));
        return STATUS_INVALID_PARAMETER;
    }
    
    KdPrint(("T2Audio: All parameters valid\n"));
    RtlZeroMemory(context, sizeof(*context));
    context->DeviceObject = DeviceObject;

    KdPrint(("T2Audio: Mapping resources\n"));
    status = T2AudioMapResources(context, ResourceList);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: MapResources failed: 0x%08X\n", status));
        return status;
    }
    KdPrint(("T2Audio: Resources mapped successfully\n"));
    
    // Phase 3.1: Register topology FIRST (defines audio path)
    PUNKNOWN topologyPortUnknown = NULL;
    PPORTTOPOLOGY topologyPort = NULL;
    PMINIPORTTOPOLOGY topologyMiniport = NULL;
    
    KdPrint(("T2Audio: Creating Topology port\n"));
    status = PcNewPort(&topologyPortUnknown, &CLSID_PortTopology);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcNewPort(Topology) failed: 0x%08X\n", status));
        T2AudioUnmapResources(context);
        return status;
    }
    
    status = topologyPortUnknown->lpVtbl->QueryInterface(topologyPortUnknown, 
                                                          &IID_IPortTopology, 
                                                          (PVOID*)&topologyPort);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: QueryInterface IPortTopology failed: 0x%08X\n", status));
        topologyPortUnknown->lpVtbl->Release(topologyPortUnknown);
        T2AudioUnmapResources(context);
        return status;
    }
    topologyPortUnknown->lpVtbl->Release(topologyPortUnknown);
    
    KdPrint(("T2Audio: Creating Topology miniport\n"));
    status = T2AudioCreateTopology(context, &topologyMiniport);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: CreateTopology failed: 0x%08X\n", status));
        topologyPort->lpVtbl->Release((PUNKNOWN)topologyPort);
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Initializing Topology port (topology FIRST, UnknownAdapter=NULL)\n"));
    status = ((PPORT)topologyPort)->lpVtbl->Init((PPORT)topologyPort, DeviceObject, 
                                                  Irp, (PUNKNOWN)topologyMiniport, 
                                                  NULL, ResourceList);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: Topology Port Init failed: 0x%08X\n", status));
        topologyMiniport->lpVtbl->Release((PUNKNOWN)topologyMiniport);
        topologyPort->lpVtbl->Release((PUNKNOWN)topologyPort);
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Registering Topology subdevice\n"));
    status = PcRegisterSubdevice(DeviceObject, L"Topology", (PUNKNOWN)topologyPort);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcRegisterSubdevice(Topology) failed: 0x%08X\n", status));
        topologyMiniport->lpVtbl->Release((PUNKNOWN)topologyMiniport);
        topologyPort->lpVtbl->Release((PUNKNOWN)topologyPort);
        T2AudioUnmapResources(context);
        return status;
    }
    
    topologyMiniport->lpVtbl->Release((PUNKNOWN)topologyMiniport);
    topologyPort->lpVtbl->Release((PUNKNOWN)topologyPort);
    
    // Phase 3: Create WaveRT port (connects to topology)
    PUNKNOWN portUnknown = NULL;
    PPORTWAVERT port = NULL;
    PMINIPORTWAVERT miniport = NULL;
    
    KdPrint(("T2Audio: Creating WaveRT port\n"));
    status = PcNewPort(&portUnknown, &CLSID_PortWaveRT);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcNewPort failed: 0x%08X\n", status));
        T2AudioUnmapResources(context);
        return status;
    }
    
    status = portUnknown->lpVtbl->QueryInterface(portUnknown, &IID_IPortWaveRT, (PVOID*)&port);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: QueryInterface IPortWaveRT failed: 0x%08X\n", status));
        portUnknown->lpVtbl->Release(portUnknown);
        T2AudioUnmapResources(context);
        return status;
    }
    portUnknown->lpVtbl->Release(portUnknown);
    
    KdPrint(("T2Audio: Creating WaveRT miniport\n"));
    status = T2AudioCreateMiniport(context, &miniport);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: CreateMiniport failed: 0x%08X\n", status));
        port->lpVtbl->Release((PUNKNOWN)port);
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Initializing WaveRT miniport\n"));
    status = ((PPORT)port)->lpVtbl->Init((PPORT)port, DeviceObject, Irp, 
                                          (PUNKNOWN)miniport, NULL, ResourceList);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: Port Init failed: 0x%08X\n", status));
        miniport->lpVtbl->Release((PUNKNOWN)miniport);
        port->lpVtbl->Release((PUNKNOWN)port);
        T2AudioUnmapResources(context);
        return status;
    }
    
    KdPrint(("T2Audio: Registering WaveRT subdevice\n"));
    status = PcRegisterSubdevice(DeviceObject, L"Wave", (PUNKNOWN)port);
    if (!NT_SUCCESS(status)) {
        KdPrint(("T2Audio: PcRegisterSubdevice failed: 0x%08X\n", status));
        miniport->lpVtbl->Release((PUNKNOWN)miniport);
        port->lpVtbl->Release((PUNKNOWN)port);
        T2AudioUnmapResources(context);
        return status;
    }
    
    miniport->lpVtbl->Release((PUNKNOWN)miniport);
    port->lpVtbl->Release((PUNKNOWN)port);
    
    context->HardwareReady = TRUE;
    context->SpeakerDeviceId = 0; // BCE transport disabled
    
    KdPrint(("T2Audio: StartDevice SUCCESS - Topology + WaveRT registered (topology FIRST), BCE disabled\n"));
    return STATUS_SUCCESS;
}
