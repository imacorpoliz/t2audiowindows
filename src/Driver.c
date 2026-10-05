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
    
    // Return success without creating ports - just test basic startup
    KdPrint(("T2Audio: StartDevice returning success (test mode)\n"));
    return STATUS_SUCCESS;
}
