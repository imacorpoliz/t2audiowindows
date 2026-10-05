# Phase 4 In Progress: Topology Port Implementation

**Date**: 2026-10-05 23:35 UTC  
**Status**: ⚠️ **DEBUGGING - Device Code 10**  
**Current Issue**: StartDevice fails with CM_PROB_FAILED_START

---

## Objective

Add topology miniport to create user-visible Windows audio endpoint while keeping BCE transport disabled.

---

## Work Completed

### 1. Topology Miniport Implementation (src/Topology.c)

✅ Created new file with IMiniportTopology interface:
- COM vtable with QueryInterface, AddRef, Release
- GetDescription, DataRangeIntersection, Init methods
- 2 pins: Pin 0 (input from WaveRT), Pin 1 (output to speaker)
- 1 node: KSNODETYPE_SPEAKER
- 2 connections: Pin 0 → Node 0 → Pin 1

✅ Filter descriptor defined with proper structure

### 2. Driver Changes (src/Driver.c:148-205)

✅ Added topology port creation sequence in StartDevice:
- PcNewPort(&CLSID_PortTopology)
- T2AudioCreateTopology()
- Port Init()
- PcRegisterSubdevice(L"Topology")

### 3. Project Configuration

✅ Added Topology.c to vcxproj (line 75)
✅ Removed ksuser.lib (user-mode, caused LNK4257)
✅ Added ksguid.lib for KSCATEGORY_AUDIO and KSNODETYPE_SPEAKER

### 4. Build Results

✅ Compilation successful
✅ No linker errors
✅ Driver size: 37,896 bytes
✅ Signed with SHA256: 77B20DB3EF5DC3818FB3CCFAC3BD6FCA598F967C823B8B4A068352BF6849F5EF

---

## Current Problem

**Device Manager**: Code 10 - CM_PROB_FAILED_START (This device cannot start)

**Symptom**: StartDevice returns error, device fails to initialize

**Diagnosis needed**: Waiting for kernel debug log to determine which step fails:
1. PcNewPort(CLSID_PortTopology)?
2. T2AudioCreateTopology()?
3. Topology Init()?
4. PcRegisterSubdevice(Topology)?

---

## Topology Design

### Pin Descriptors

```c
Pin 0: Input from WaveRT
  - KSPIN_DATAFLOW_IN
  - KSPIN_COMMUNICATION_NONE
  - Category: KSCATEGORY_AUDIO
  
Pin 1: Output to speaker
  - KSPIN_DATAFLOW_OUT
  - KSPIN_COMMUNICATION_NONE
  - Category: KSCATEGORY_AUDIO
  - Name: KSNODETYPE_SPEAKER
```

### Node Descriptor

```c
Node 0: Speaker node
  - Type: KSNODETYPE_SPEAKER
  - No automation table (NULL)
```

### Connections

```c
Connection 0: PCFILTER_NODE:0 → Node 0:1
Connection 1: Node 0:0 → PCFILTER_NODE:1
```

**Note**: Initially used 0xFFFFFFFF for pin indices, corrected to explicit pin numbers (0, 1).

---

## Known Issues Fixed

### Issue 1: ksuser.lib link error
**Error**: `LNK4257: object file was not compiled for kernel mode`  
**Fix**: Removed ksuser.lib from both Debug and Release configurations, added ksguid.lib

### Issue 2: Unresolved GUID symbols
**Error**: `LNK2001: unresolved external symbol KSNODETYPE_SPEAKER`  
**Fix**: Added ksguid.lib to linker dependencies

### Issue 3: Forward declaration
**Error**: `C2065: 'g_T2AudioTopologyFilterDescriptor': undeclared identifier`  
**Fix**: Added forward declaration before first use

### Issue 4: Invalid connection descriptors
**Problem**: Used 0xFFFFFFFF for pin indices (from misunderstanding PCCONNECTION_DESCRIPTOR)  
**Fix**: Changed to explicit pin indices (0, 1)

---

## Testing Status

❌ Device fails to start (Code 10)  
⏳ Waiting for kernel debug log  
❌ Audio endpoint not created (blocked by Code 10)  
✅ WaveRT port still works (tested in Phase 3)  

---

## Debugging Steps Taken

1. ✅ Installed corrected driver (SHA256 77B20DB3...)
2. ✅ Started DebugView (PID 10632)
3. ✅ Enabled kernel capture (Ctrl+K)
4. ✅ Restarted device
5. ⏳ Waiting for log output

---

## Next Steps

1. Analyze kernel debug log to find exact failure point
2. Fix the failing step (likely Init or PcRegisterSubdevice)
3. Verify topology port registers successfully
4. Check if AudioEndpointBuilder creates endpoint
5. Test endpoint visibility in Sound settings

---

## Files Modified

- src/Topology.c (new file, 210 lines)
- src/T2AudioMiniport.h (added T2AudioCreateTopology prototype)
- src/Driver.c (added topology port creation, lines 148-205)
- src/T2AudioMiniport.vcxproj (added Topology.c, fixed linker libs)

---

## Reference

Topology implementation based on:
- Windows Driver Kit (WDK) PortCls documentation
- IMiniportTopology interface specification
- PCFILTER_DESCRIPTOR structure requirements
- Microsoft sysvad sample driver topology miniport

**Important**: Topology has NO data ranges (DataRangeIntersection returns STATUS_NOT_IMPLEMENTED) because topology filters do not process audio data — they only describe routing and volume nodes.
