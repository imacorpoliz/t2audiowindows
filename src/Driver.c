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
    if (DeviceObject == NULL) {
        KdPrint(("T2Audio: DeviceObject is NULL\n"));
        return STATUS_INVALID_PARAMETER;
    }
    if (Irp == NULL) {
        KdPrint(("T2Audio: Irp is NULL\n"));
        return STATUS_INVALID_PARAMETER;
    }
    if (ResourceList == NULL) {
        KdPrint(("T2Audio: ResourceList is NULL\n"));
        return STATUS_INVALID_PARAMETER;
    }
    if (context == NULL) {
        KdPrint(("T2Audio: Context is NULL\n"));
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
    
    // Phase 3: Create WaveRT port and register subdevice
    // BCE transport and hardware I/O remain disabled (SpeakerDeviceId == 0)
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
    
    // Release references - PcRegisterSubdevice holds its own
    miniport->lpVtbl->Release((PUNKNOWN)miniport);
    port->lpVtbl->Release((PUNKNOWN)port);
    
    context->HardwareReady = TRUE;
    context->SpeakerDeviceId = 0; // BCE transport disabled
    
    KdPrint(("T2Audio: StartDevice SUCCESS - WaveRT registered, BCE disabled\n"));
    return STATUS_SUCCESS;
}
