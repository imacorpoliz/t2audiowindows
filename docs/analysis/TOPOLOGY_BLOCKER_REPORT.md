# Topology Miniport Registration Blocker - Detailed Report

**Date:** 2026-10-06  
**Driver:** T2AudioMiniport.sys (Apple T2 Audio WDM Driver)  
**Critical Error:** `STATUS_INVALID_DEVICE_STATE (0xC000028C)` from PortCls Topology Port->Init()

---

## Executive Summary

WaveRT miniport registration works perfectly. Topology miniport registration **consistently fails** at `Port->Init()` with STATUS_INVALID_DEVICE_STATE, preventing audio endpoint creation. This error persists across 8+ different configuration attempts, suggesting a fundamental missing requirement in the topology descriptor or device state.

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

`STATUS_INVALID_DEVICE_STATE (0xC000028C)` means:
> "The device is not in a valid state to perform this request."

This is **NOT** `STATUS_INVALID_PARAMETER` (which would indicate malformed descriptor).  
This suggests PortCls sees a syntactically correct descriptor, but believes the **device state** prevents topology registration.

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

### Hypothesis 1: Missing Automation Table (LIKELY)
PortCls may **require** topology filters to have at least basic property handlers:
- `KSPROPERTY_TOPOLOGY_*` handlers
- `KSPROPSETID_Topology` support

Current descriptor has `AutomationTable = NULL` for filter, nodes, and pins.

**Evidence:**
- All WDK topology samples include automation tables
- Topology is specifically for **property routing** (volume, mute, etc.)
- Empty automation may trigger "invalid state" since topology has no functionality

**Test needed:**
Add minimal automation table with KSPROPERTY_TOPOLOGY_NAME handler.

### Hypothesis 2: Missing Data Ranges (POSSIBLE)
Even with `KSPIN_COMMUNICATION_NONE`, PortCls may validate that pins have data ranges defined.

Current descriptor has `0, NULL` for DataRanges on both pins.

**Test needed:**
Add dummy KSDATARANGE_AUDIO to both pins.

### Hypothesis 3: PcAddAdapterDevice Size Mismatch (UNLIKELY)
If `PcAddAdapterDevice` was called with wrong `DeviceExtensionSize`, DeviceObject may not have proper PortCls extension.

Current code:
```c
status = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, 
                           T2AudioStartDevice, MAX_MINIPORTS, 0);
```

`DeviceExtensionSize = 0` means PortCls allocates default size. This should be correct.

**Evidence against:** WaveRT Port->Init() works fine with same DeviceObject.

### Hypothesis 4: Multiple Topology Ports Forbidden (UNLIKELY)
PortCls may enforce "only one topology per adapter."

**Evidence against:** Only one topology port created. No duplicates.

### Hypothesis 5: Topology Requires WaveRT Reference (POSSIBLE)
Topology may need `UnknownAdapter` pointing to registered WaveRT miniport to validate bridge connection.

**Already tested:** UnknownAdapter=WaveRT miniport pointer → still FAILED.

### Hypothesis 6: Pin InstantiationCount Issue (RULED OUT)
Initial hypothesis was that `{0, 0, 0}` means "pin cannot be instantiated."

**Already tested:** Changed to `{1, 1, 0}` → still FAILED.

### Hypothesis 7: Missing KSCATEGORY in Filter Categories (POSSIBLE)
Filter descriptor has:
```c
0,        // CategoryCount
NULL      // Categories
```

Topology filters may require `KSCATEGORY_AUDIO` or `KSCATEGORY_TOPOLOGY` in filter categories array.

**Test needed:**
```c
static const GUID* g_T2AudioTopologyCategories[] = {
    &KSCATEGORY_AUDIO
};

// In filter descriptor:
1,
g_T2AudioTopologyCategories
```

---

## Next Steps (Priority Order)

### 1. Add Automation Table (HIGH PRIORITY)
Create minimal automation table with KSPROPERTY_TOPOLOGY handlers.

```c
static PCPROPERTY_ITEM g_TopologyProperties[] = {
    {
        &KSPROPSETID_Topology,
        KSPROPERTY_TOPOLOGY_NAME,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_TopologyName
    }
};

static PCAUTOMATION_TABLE g_TopologyFilterAutomation = {
    DEFINE_PCAUTOMATION_TABLE_PROP(g_TopologyProperties, NULL)
};
```

### 2. Add Filter Categories (MEDIUM PRIORITY)
```c
static const GUID* g_T2AudioTopologyCategories[] = {
    &KSCATEGORY_AUDIO
};

// In PCFILTER_DESCRIPTOR:
1,
g_T2AudioTopologyCategories
```

### 3. Add Pin Data Ranges (MEDIUM PRIORITY)
Even for COMMUNICATION_NONE pins, add dummy data ranges:
```c
static KSDATARANGE_AUDIO g_TopologyPinDataRanges = {
    {
        sizeof(KSDATARANGE_AUDIO),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    2, 48000, 48000, 16, 16, 0
};
```

### 4. Research WDK Samples (HIGH PRIORITY)
Find minimal working topology example in WDK (e.g., MSVAD simple topology) and compare:
- Automation tables
- Pin configurations
- Filter categories
- Property handlers

### 5. Add Debug Logging to PortCls (FALLBACK)
If configuration fixes don't work, may need to use WinDbg to trace PortCls validation:
```
bp portcls!CPortTopology::Init
.reload /f portcls.sys
```

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
