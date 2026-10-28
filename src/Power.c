#include "T2AudioMiniport.h"

// IAdapterPowerManagement for the adapter (R8, docs/analysis/RE_appleaudio_protocol.md
// section 7). AppleAudio.sys stops hardware I/O in QueryPowerChangeState(D3)
// - while AppleUSBVHCI can still carry the command - and again in
// PowerChangeState(D1..D3); on D0 it only re-registers its input buffers. We
// do the same for the speaker: STOP_IO before the device leaves D0, and no
// MMIO writes while it is powered down.
//
// The object is a static singleton (there is one T2 audio device), fully
// initialized at compile time: no pool allocation, no constructor, and it
// survives the device-extension reset in T2AudioStartDevice. Reference
// counting is therefore a no-op.

typedef struct _T2AUDIO_POWER_OBJECT {
    IAdapterPowerManagement Interface;
    PT2AUDIO_DEVICE_CONTEXT Context;
} T2AUDIO_POWER_OBJECT;

static T2AUDIO_POWER_OBJECT g_T2AudioPower;

static NTSTATUS STDMETHODCALLTYPE
T2AudioPowerQueryInterface(
    _In_ PADAPTERPOWERMANAGEMENT This,
    _In_ REFIID InterfaceId,
    _COM_Outptr_ PVOID *Interface)
{
    if (Interface == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Interface = NULL;
    if (IsEqualGUIDAligned(InterfaceId, &IID_IUnknown) ||
        IsEqualGUIDAligned(InterfaceId, &IID_IAdapterPowerManagement)) {
        *Interface = This;
        return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
T2AudioPowerAddRef(_In_ PADAPTERPOWERMANAGEMENT This)
{
    UNREFERENCED_PARAMETER(This);
    return 1;
}

static ULONG STDMETHODCALLTYPE
T2AudioPowerRelease(_In_ PADAPTERPOWERMANAGEMENT This)
{
    UNREFERENCED_PARAMETER(This);
    return 1;
}

// Stop the speaker I/O if a stream started it. Shared with the stream's
// leave-RUN path through Context->HardwareIoActive, so exactly one STOP_IO is
// sent per START_IO. PASSIVE_LEVEL (PortCls power callbacks).
static VOID
T2AudioPowerQuiesce(_In_ PT2AUDIO_DEVICE_CONTEXT Context, _In_z_ PCSTR Why)
{
    NTSTATUS status;

    // Microphones (Parameters\MicInput): stop the T2 writing the host ring.
    if (InterlockedExchange(&Context->MicIoActive, 0) != 0) {
        status = T2AudioStopIoDevice(Context, Context->MicDeviceId);
        KdPrint(("T2Audio: power %s: mic STOP_IO status=0x%08X\n", Why, status));
    }
    // Headphones (Parameters\HeadphoneOutput): the same for their device.
    if (InterlockedExchange(&Context->HeadphoneIoActive, 0) != 0) {
        status = T2AudioStopIoDevice(Context, Context->HeadphoneDeviceId);
        KdPrint(("T2Audio: power %s: headphone STOP_IO status=0x%08X\n", Why, status));
    }
    if (InterlockedExchange(&Context->HardwareIoActive, 0) == 0) {
        return;
    }
    status = T2AudioStopIo(Context);
    KdPrint(("T2Audio: power %s: STOP_IO status=0x%08X\n", Why, status));
}

// Back in D0 (PASSIVE work item): with Parameters\MicInput re-announce the
// microphone ring (msg 18, as AppleAudio does after every wake); with
// Parameters\ResumeAfterSleep restart the I/O of streams that are still in
// RUN. Waits 500 ms first so AppleUSBVHCI has re-established remote access,
// then tries the restart up to 3 times.
static IO_WORKITEM_ROUTINE T2AudioResumeWorkRoutine;

static VOID
T2AudioResumeWorkRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID WorkContext)
{
    PIO_WORKITEM workItem = (PIO_WORKITEM)WorkContext;
    PT2AUDIO_DEVICE_CONTEXT context = T2AudioGetContext(DeviceObject);
    LARGE_INTEGER delay;
    ULONG attempt;
    ULONG i;

    PAGED_CODE();

    delay.QuadPart = -5000000ll; // 500 ms
    for (attempt = 0; attempt < 3; attempt++) {
        BOOLEAN pending = FALSE;

        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        KeWaitForSingleObject(&context->StreamMutex, Executive, KernelMode, FALSE, NULL);
        // Microphones: announce the host ring again after every wake, as
        // AppleAudio does (once, before any stream restarts).
        if (attempt == 0 && !context->PoweredDown && context->MicRegistered) {
            NTSTATUS micStatus = T2AudioMicReRegister(context);

            KdPrint(("T2Audio: D0: mic ranges re-sent: 0x%08X\n", micStatus));
        }
        if (!context->PoweredDown && context->ResumeAfterSleep) {
            if (context->MicRunningStream != NULL) {
                T2AudioMicStreamResumeAfterPower(context->MicRunningStream);
            }
        }
        if (!context->PoweredDown && context->ResumeAfterSleep) {
            for (i = 0; i < 2; i++) {
                if (context->RunningStream[i] != NULL) {
                    T2AudioStreamResumeAfterPower(context->RunningStream[i]);
                }
            }
            pending = (context->RunningStream[0] != NULL && context->HardwareIoActive == 0) ||
                      (context->RunningStream[1] != NULL && context->HeadphoneIoActive == 0) ||
                      (context->MicRunningStream != NULL && context->MicIoActive == 0);
        }
        KeReleaseMutex(&context->StreamMutex, FALSE);
        if (!pending) {
            break;
        }
    }
    InterlockedExchange(&context->ResumeWorkPending, 0);
    if (workItem != NULL) {
        IoFreeWorkItem(workItem);
    }
}

static VOID STDMETHODCALLTYPE
T2AudioPowerChangeState(
    _In_ PADAPTERPOWERMANAGEMENT This,
    _In_ POWER_STATE NewState)
{
    PT2AUDIO_DEVICE_CONTEXT context =
        CONTAINING_RECORD(This, T2AUDIO_POWER_OBJECT, Interface)->Context;
    KIRQL oldIrql;

    KdPrint(("T2Audio: PowerChangeState D%u\n",
             (ULONG)NewState.DeviceState - (ULONG)PowerDeviceD0));
    if (context == NULL) {
        return;
    }

    if (NewState.DeviceState == PowerDeviceD0) {
        KeAcquireSpinLock(&context->HardwareLock, &oldIrql);
        context->PoweredDown = FALSE;
        KeReleaseSpinLock(&context->HardwareLock, oldIrql);
        // Persistent mode: a hardware session runs again (crash guard).
        T2AudioCrashGuardSetArmed(context, TRUE);
        // Like AppleAudio, START_IO is not re-sent here: it comes from the
        // next SetState(RUN). With ResumeAfterSleep a work item restarts the
        // I/O of a stream that stayed in RUN through the sleep.
        if (context->DeviceObject != NULL &&
            (context->MicRegistered ||
             (context->ResumeAfterSleep &&
              (context->RunningStream[0] != NULL || context->RunningStream[1] != NULL ||
               context->MicRunningStream != NULL))) &&
            InterlockedCompareExchange(&context->ResumeWorkPending, 1, 0) == 0) {
            PIO_WORKITEM workItem = IoAllocateWorkItem(context->DeviceObject);

            if (workItem != NULL) {
                IoQueueWorkItem(workItem, T2AudioResumeWorkRoutine, DelayedWorkQueue, workItem);
            } else {
                InterlockedExchange(&context->ResumeWorkPending, 0);
            }
        }
        return;
    }

    if (NewState.DeviceState > PowerDeviceD0 &&
        NewState.DeviceState < PowerDeviceMaximum) {
        T2AudioPowerQuiesce(context, "D1-D3");
        // Persistent mode: the session ended cleanly (idle, sleep, hibernate).
        T2AudioCrashGuardSetArmed(context, FALSE);
        // The copy DPC must not write device memory of a powered-down device.
        KeAcquireSpinLock(&context->HardwareLock, &oldIrql);
        context->PoweredDown = TRUE;
        KeReleaseSpinLock(&context->HardwareLock, oldIrql);
    }
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioPowerQueryPowerChangeState(
    _In_ PADAPTERPOWERMANAGEMENT This,
    _In_ POWER_STATE NewStateQuery)
{
    PT2AUDIO_DEVICE_CONTEXT context =
        CONTAINING_RECORD(This, T2AUDIO_POWER_OBJECT, Interface)->Context;

    KdPrint(("T2Audio: QueryPowerChangeState D%u\n",
             (ULONG)NewStateQuery.DeviceState - (ULONG)PowerDeviceD0));
    // Stop early, while AppleUSBVHCI is certainly still up (AppleAudio does
    // this for D3).
    if (context != NULL && NewStateQuery.DeviceState == PowerDeviceD3) {
        T2AudioPowerQuiesce(context, "query D3");
    }
    return STATUS_SUCCESS;
}

static NTSTATUS STDMETHODCALLTYPE
T2AudioPowerQueryDeviceCapabilities(
    _In_ PADAPTERPOWERMANAGEMENT This,
    _Inout_updates_bytes_(sizeof(DEVICE_CAPABILITIES)) PDEVICE_CAPABILITIES PowerDeviceCaps)
{
    UNREFERENCED_PARAMETER(This);
    UNREFERENCED_PARAMETER(PowerDeviceCaps);
    return STATUS_SUCCESS;
}

static const IAdapterPowerManagementVtbl g_T2AudioPowerVtbl = {
    (void *)T2AudioPowerQueryInterface,
    (void *)T2AudioPowerAddRef,
    (void *)T2AudioPowerRelease,
    (void *)T2AudioPowerChangeState,
    (void *)T2AudioPowerQueryPowerChangeState,
    (void *)T2AudioPowerQueryDeviceCapabilities
};

NTSTATUS
T2AudioRegisterPowerManagement(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PT2AUDIO_DEVICE_CONTEXT Context)
{
    NTSTATUS status;

    g_T2AudioPower.Interface.lpVtbl = &g_T2AudioPowerVtbl;
    g_T2AudioPower.Context = Context;
    status = PcRegisterAdapterPowerManagement((PUNKNOWN)&g_T2AudioPower.Interface,
                                              DeviceObject);
    KdPrint(("T2Audio: PcRegisterAdapterPowerManagement status=0x%08X\n", status));
    return status;
}
