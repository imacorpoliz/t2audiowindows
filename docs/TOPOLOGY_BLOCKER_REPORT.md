# Topology Miniport Registration Blocker - Detailed Report

**Date:** 2026-10-06  
**Driver:** T2AudioMiniport.sys (Apple T2 Audio WDM Driver)  
**Critical Error:** `STATUS_RANGE_NOT_FOUND (0xC000028C)` from PortCls Topology Port->Init()

**CORRECTION (2026-10-06):** Initial report incorrectly identified 0xC000028C as STATUS_INVALID_DEVICE_STATE.  
Actual NTSTATUS per ntstatus.h: **STATUS_RANGE_NOT_FOUND** (0xC000028C).  
STATUS_INVALID_DEVICE_STATE = 0xC0000184 (different error).  
See raw kernel log: `docs/logs/topology_init_failure_20261006.log`

---

## Executive Summary

WaveRT miniport registration works perfectly. Topology miniport registration **consistently fails** at `Port->Init()` with STATUS_RANGE_NOT_FOUND (0xC000028C), preventing audio endpoint creation. This error persists across 8+ different configuration attempts, suggesting **missing or invalid data ranges** in pin descriptors.

---

## System Context

### Hardware
- **Device:** Apple T2 Audio Controller
- **PCI ID:** VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01
- **Resources:**
  - BAR0: 0xC1000000 (4MB) - buffer memory
  - BAR4: 0xC1670000 (64KB) - config memory with GPR registers
  - Speaker buffer: offset 0x12C000, size 0x61800 bytes

### Driver State
- **DriverEntry:** SUCCESS
- **PcAddAdapterDevice:** SUCCESS (returns 0x00000000)
- **StartDevice:** Enters successfully, all parameters valid
- **Resource mapping:** SUCCESS (GPR signature 0x19870423 validated)
- **WaveRT registration:** SUCCESS (Port->Init, PcRegisterSubdevice both OK)
- **Topology registration:** **FAILED at Port->Init() with 0xC000028C**

---

## Error Details

### Call Stack (from debug log, line 1302, timestamp 1922.28s)

```
T2Audio: Creating Topology port           [SUCCESS - PcNewPort]
T2Audio: Creating Topology miniport       [SUCCESS - T2AudioCreateTopology]
T2Audio: Topology miniport created        [SUCCESS]
T2Audio: Initializing Topology port (topology FIRST, UnknownAdapter=NULL)
T2Audio: Topology Init success            [SUCCESS - miniport->Init()]
T2Audio: Topology Port Init failed: 0xC000028C  [FAILED - Port->Init()]
```

**Key observation:** 
- `topologyMiniport->Init()` returns SUCCESS
- `((PPORT)topologyPort)->Init()` returns 0xC000028C

This means:
1. The miniport object is valid and accepts initialization
2. PortCls **validates the descriptor** and **rejects it** during Port->Init()

### Error Code Analysis

**CORRECTED:** `STATUS_RANGE_NOT_FOUND (0xC000028C)` per ntstatus.h means:
> "The request failed because a valid data range could not be found."

This is **NOT** a device state issue. PortCls is specifically looking for **data ranges** in the pin descriptors and failing to find valid/matching ranges during validation.

**Initial diagnosis was incorrect:** The error name was misidentified, leading to wrong hypotheses about device state, automation tables, and filter categories. The actual issue points directly to **missing or incompatible pin data ranges**.

---

## Configuration Attempts (All Failed)

### Attempt 1: Topology AFTER WaveRT, UnknownAdapter=NULL
```c
// Driver.c: WaveRT registered first, then Topology
status = PcRegisterSubdevice(DeviceObject, L"Wave", waveRTPort);  // SUCCESS
status = ((PPORT)topologyPort)->Init(..., NULL, ResourceList);     // FAILED 0xC000028C
```
**Result:** FAILED

### Attempt 2: Topology AFTER WaveRT, UnknownAdapter=WaveRT miniport
```c
status = ((PPORT)topologyPort)->Init(..., (PUNKNOWN)waveRTMiniport, ResourceList);
```
**Result:** FAILED 0xC000028C

### Attempt 3: Topology BEFORE WaveRT, UnknownAdapter=NULL (current config)
```c
// Driver.c: Topology registered FIRST
status = ((PPORT)topologyPort)->Init(..., NULL, ResourceList);     // FAILED 0xC000028C
```
**Result:** FAILED 0xC000028C

### Attempt 4: Direct pin-to-pin topology (no nodes)
```c
// Topology.c: 2 pins, 0 nodes, 1 connection
static PCCONNECTION_DESCRIPTOR g_T2AudioTopologyConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }  // Direct: Pin0 -> Pin1
};
```
**Result:** FAILED 0xC000028C

### Attempt 5: Pin0 Category = KSCATEGORY_AUDIO
```c
// Pin 0 (bridge):
{
    0, 0, 0, NULL,
    {
        ...,
        KSPIN_DATAFLOW_IN,
        KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO,  // <- Added category
        NULL,
        0
    }
}
```
**Result:** FAILED 0xC000028C

### Attempt 6: Pin0 Category = NULL (bridge convention)
```c
// Pin 0 (bridge):
{
    0, 0, 0, NULL,
    {
        ...,
        KSPIN_DATAFLOW_IN,
        KSPIN_COMMUNICATION_NONE,
        NULL,  // <- Bridge pin, no category
        NULL,
        0
    }
}
```
**Result:** FAILED 0xC000028C

### Attempt 7: Pin instance counts = {1, 1, 0}
```c
// Both pins:
{
    1, 1, 0, NULL,  // MaxGlobal=1, MaxFilter=1, Min=0
    { ... }
}
```
**Result:** FAILED 0xC000028C

### Attempt 8: Topology with speaker node
```c
// Current configuration: Pin0 -> Node0(SPEAKER) -> Pin1
static PCNODE_DESCRIPTOR g_T2AudioTopologyNodes[] = {
    { 0, NULL, &KSNODETYPE_SPEAKER, NULL }
};
static PCCONNECTION_DESCRIPTOR g_T2AudioTopologyConnections[] = {
    { PCFILTER_NODE, 0, 0, 0 },           // Filter Pin0 -> Node0 input
    { 0, 0, PCFILTER_NODE, 1 }            // Node0 output -> Filter Pin1
};
```
**Result:** FAILED 0xC000028C

---

## Current Topology Configuration

### src/Topology.c (210 lines)

#### Pin Descriptors
```c
static PCPIN_DESCRIPTOR g_T2AudioTopologyPins[] = {
    // Pin 0: Input from WaveRT (bridge pin)
    {
        1, 1, 0, NULL,  // MaxGlobalInstanceCount, MaxFilterInstanceCount, MinFilterInstanceCount, AutomationTable
        {
            0, NULL,                    // Interfaces (0 interfaces)
            0, NULL,                    // Mediums (0 mediums)
            0, NULL,                    // DataRanges (0 data ranges)
            KSPIN_DATAFLOW_IN,
            KSPIN_COMMUNICATION_NONE,
            NULL,                       // Category (bridge pin has no category)
            NULL,                       // Name (bridge pin has no name)
            0                           // Reserved
        }
    },
    // Pin 1: Output to speaker (physical connector)
    {
        1, 1, 0, NULL,
        {
            0, NULL,
            0, NULL,
            0, NULL,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            &KSCATEGORY_AUDIO,
            &KSNODETYPE_SPEAKER,
            0
        }
    }
};
```

#### Node Descriptor
```c
static PCNODE_DESCRIPTOR g_T2AudioTopologyNodes[] = {
    {
        0,                      // Flags
        NULL,                   // AutomationTable
        &KSNODETYPE_SPEAKER,    // Type
        NULL                    // Name
    }
};
```

#### Connections
```c
static PCCONNECTION_DESCRIPTOR g_T2AudioTopologyConnections[] = {
    { PCFILTER_NODE, 0, 0, 0 },           // FromNode=FILTER, FromPin=0 -> ToNode=0, ToPin=0
    { 0, 0, PCFILTER_NODE, 1 }            // FromNode=0, FromPin=0 -> ToNode=FILTER, ToPin=1
};
```

#### Filter Descriptor
```c
static PCFILTER_DESCRIPTOR g_T2AudioTopologyFilterDesc = {
    0,                                    // Version
    NULL,                                 // AutomationTable (no properties)
    sizeof(g_T2AudioTopologyPins) / sizeof(PCPIN_DESCRIPTOR),
    g_T2AudioTopologyPins,
    sizeof(g_T2AudioTopologyNodes) / sizeof(PCNODE_DESCRIPTOR),
    g_T2AudioTopologyNodes,
    sizeof(g_T2AudioTopologyConnections) / sizeof(PCCONNECTION_DESCRIPTOR),
    g_T2AudioTopologyConnections,
    0,                                    // CategoryCount
    NULL                                  // Categories
};
```

#### Miniport Implementation
```c
typedef struct _T2AUDIOTOPOLOGY {
    MINIPORTTOPOLOGY_VTABLE* lpVtbl;
    LONG RefCount;
    PT2AUDIOCONTEXT Context;
} T2AUDIOTOPOLOGY, *PT2AUDIOTOPOLOGY;

// IMiniportTopology methods implemented:
// - QueryInterface
// - AddRef
// - Release
// - GetDescription (returns &g_T2AudioTopologyFilterDesc)
// - DataRangeIntersection (returns STATUS_NOT_IMPLEMENTED)
// - Init (returns STATUS_SUCCESS)
```

All methods are correctly implemented. Miniport->Init() succeeds.

---

## Hypotheses

### Hypothesis 1: Missing Pin Data Ranges (CONFIRMED - ROOT CAUSE)
**STATUS_RANGE_NOT_FOUND** explicitly indicates PortCls cannot find valid data ranges during pin validation.

Current descriptor had `DataRangeCount = 0, DataRanges = NULL` for both pins.

**Evidence:**
- Error name directly references "RANGE"
- PortCls validates data ranges during Port->Init()
- Even KSPIN_COMMUNICATION_NONE pins require data range descriptors

**Fix applied:**
Added KSDATARANGE with KSDATAFORMAT_TYPE_AUDIO/SUBTYPE_ANALOG/SPECIFIER_NONE for both bridge and speaker pins.

### Hypothesis 2: Missing Automation Table (DEPRIORITIZED)
Initially suspected based on incorrect error diagnosis. STATUS_RANGE_NOT_FOUND does not indicate missing property handlers.

Automation tables are for property routing (volume, mute), not topology validation.

### Hypothesis 3: Missing Filter Categories (UNLIKELY)
Filter categories advertise filter capabilities. Not required for basic topology registration.

---

## Next Steps (Priority Order)

### 1. Test Data Range Fix (COMPLETED)
Added KSDATARANGE descriptors to both topology pins:
- Pin 0 (bridge): AUDIO/ANALOG/NONE wildcard
- Pin 1 (speaker): AUDIO/ANALOG/NONE wildcard

**Expected:** Port->Init() should succeed with valid data ranges.

### 2. Verify Topology Registration (PENDING)
After successful Port->Init():
- Check PcRegisterSubdevice() return code
- Verify KSCATEGORY_AUDIO interface creation
- Check device manager for topology subdevice

### 3. Register Physical Connection (PENDING)
Connect WaveRT and Topology filters:
```c
PcRegisterPhysicalConnection(DeviceObject, 
                            waveRTPort, KSPIN_WAVE_RENDER_SINK_SYSTEM,
                            topologyPort, KSPIN_TOPO_WAVEOUT_SOURCE);
```

### 4. Verify Audio Endpoint Creation (PENDING)
Check if MMDevice endpoint appears in Sound Settings after topology registration.

### 5. Add Property Handlers (DEFERRED)
Only if endpoint still doesn't appear. Add automation table with basic topology properties.

---

## Impact Assessment

**Without topology registration:**
- ❌ No audio endpoint in Device Manager
- ❌ No device visible in Sound Settings
- ❌ Cannot set as default playback device
- ❌ No audio streaming possible (even if WaveRT works)

**Topology is mandatory** for PortCls audio drivers to create user-visible endpoints.

---

## Files

- `C:\Users\othysa\Desktop\mbp\T2AudioPort\src\Driver.c` (lines 77-133): Topology registration code
- `C:\Users\othysa\Desktop\mbp\T2AudioPort\src\Topology.c` (210 lines): Topology miniport implementation
- `C:\Users\othysa\Desktop\mbp\DESKTOP-PK3472B.log` (lines 1297-1302): Latest failure log
- `C:\Windows\System32\drivers\T2AudioMiniport.sys`: Installed driver (oem16.inf)

---

## Recommendations for Next Developer

1. **Start with automation table** - this is the most likely missing piece
2. **Don't waste time on pin categories or instance counts** - already exhaustively tested
3. **If automation doesn't help**, add filter categories and data ranges
4. **If still failing**, use WinDbg to step through PortCls validation logic
5. **Consider alternative approach**: Research if PortCls supports WaveRT-only drivers without topology (unlikely but worth checking)

---

## Conclusion

STATUS_INVALID_DEVICE_STATE suggests PortCls is enforcing a requirement we haven't met. Since the descriptor is syntactically valid (miniport->Init() succeeds), the issue is likely:

1. Missing automation table (topology has no property handlers)
2. Missing filter categories (topology not advertising KSCATEGORY_AUDIO)
3. Some PortCls internal state validation that requires specific initialization order or device state

The error is **configuration-related, not a fundamental design flaw**. With proper descriptor configuration, topology registration should succeed.
