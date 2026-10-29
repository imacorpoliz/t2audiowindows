# Reverse Engineering: AppleAudio.sys Headset & Internal Microphone Architecture

**Binary Analyzed:** `AppleAudio.sys`  
**SHA256:** `cd8b81fabc9be6127a082da22c4cc046a4d46e0225eb6baf6d27e0dc9862a150`  
**Size:** 122,752 bytes  
**Date:** 2026-10-29  
**Tooling:** REA Ghidra Decompiler / Disassembler  

---

## 1. Executive Summary

This report documents the reverse engineering of microphone input handling in `AppleAudio.sys` on MacBookPro16,1 (Apple T2 Audio).

### Key Findings:
1. **Dual Independent Capture Endpoints:**
   `AppleAudio.sys` exposes internal digital microphones and external 3.5mm headset microphones as **two separate, independent Windows audio capture endpoints**, rather than a single endpoint with an internal hardware multiplexer.
2. **Device Identification in `FUN_14001604c`:**
   The driver inspects device names returned by BridgeOS:
   - `"Codec Input"` (or `"Codec"`, device ID `0x44` on MBP16,1): Registers `TopologyMicIn` and `WaveMicIn` using descriptor `DAT_1400138f0`.
   - `"Digital Mic"` (device ID `0x21` on MBP16,1): Registers `TopologyMic` and `WaveMic` using descriptor `DAT_140013970`.
3. **Stream Formats and Capabilities:**
   - **Internal Digital Mic (`0x21`):** 3 channels, 48,000 Hz, 24-bit PCM in 32-bit slots (bytes per frame: 12), flags `0x4`.
   - **External Codec Input (`0x44`):** 1 channel (mono), 48,000 Hz, 32-bit signed integer (bytes per frame: 4), flags `0xC`.
4. **Jack Detection for Headset Mic (`'inpt'`):**
   `AppleAudio.sys` registers a property listener (Msg 11) with selector `'jack'` (`0x6A61636B`) on scope `'inpt'` (`0x696E7074`) for `"Codec Input"`. When a TRRS 4-pole headset is plugged into the 3.5mm combo jack, BridgeOS fires Message 13 (`PROPERTY_CHANGED`), prompting AppleAudio to signal `KSEVENT_PINCAPS_JACKINFOCHANGE` to Windows Audio Service.
5. **Host Memory Allocation & Registration (Msg 18):**
   Unlike output streams (where BridgeOS allocates the DMA ring buffers in its own RAM), input audio rings are **allocated in host RAM** by the Windows driver and registered with BridgeOS via BCE Message 18 (using an 837-byte descriptor payload).

---

## 2. Decompilation Analysis

### 2.1 Subdevice Dispatch: `FUN_14001604c`

During driver enumeration, `FUN_14001604c` reads the device UID / name string and instantiates PortCls miniport pairs:

```c
// Decompiled excerpt of FUN_14001604c
__int64 __fastcall FUN_14001604c(__int64 devContext, struct BceDeviceInfo *devInfo)
{
    // Check for Headset Microphone ("Codec Input" / "Codec")
    if (strcmp(devInfo->DeviceName, "Codec Input") == 0 ||
        strcmp(devInfo->DeviceName, "Codec") == 0)
    {
        // Register Headset capture endpoint
        // Miniport descriptors at DAT_1400138f0:
        //   Topology: L"TopologyMicIn"
        //   Wave:     L"WaveMicIn"
        return PcRegisterSubdevice(
                   devContext,
                   L"WaveMicIn",
                   (PUNKNOWN)&DAT_1400138f0);
    }

    // Check for Internal Microphone Array ("Digital Mic")
    if (strcmp(devInfo->DeviceName, "Digital Mic") == 0)
    {
        // Register Internal Mic capture endpoint
        // Miniport descriptors at DAT_140013970:
        //   Topology: L"TopologyMic"
        //   Wave:     L"WaveMic"
        return PcRegisterSubdevice(
                   devContext,
                   L"WaveMic",
                   (PUNKNOWN)&DAT_140013970);
    }

    return STATUS_SUCCESS;
}
```

### 2.2 Physical Format Probing

`AppleAudio.sys` sends BCE Message 24 (`GET_INPUT_STREAM_LIST`) to retrieve input stream objects, followed by property queries:
- **Selector `'pft '` (`0x70667420`):** Physical stream format.
  - `"Digital Mic"` stream `0x22`:
    - Sample Rate: `48000.0`
    - Format: `0x6C70636D` (`'lpcm'`)
    - Flags: `0x00000004`
    - Bytes per Packet: `12`
    - Frames per Packet: `1`
    - Bytes per Frame: `12`
    - Channels: `3`
    - Bits per Channel: `24`
  - `"Codec Input"` stream `0x45`:
    - Sample Rate: `48000.0`
    - Format: `0x6C70636D` (`'lpcm'`)
    - Flags: `0x0000000C` (signed int, packed)
    - Bytes per Packet: `4`
    - Frames per Packet: `1`
    - Bytes per Frame: `4`
    - Channels: `1`
    - Bits per Channel: `32`

---

## 3. Host Memory Registration (BCE Message 18)

Microphone capture requires registering host physical memory addresses with BridgeOS:

1. **Host Allocation (`FUN_140001fa4`):**
   `AppleAudio.sys` allocates contiguous non-paged host memory (`MmAllocatePagesForMdlEx` / `MmAllocateContiguousMemory`) for the capture ring buffer.
2. **BufferStruct Serialization (`FUN_1400046f8`):**
   Physical addresses and page ranges are formatted into the device's shared structure:
   - `BufferStruct.devices[i].input_streams[0].num_buffers`
   - `BufferStruct.devices[i].input_streams[0].buffers[n].address` (64-bit physical address)
   - `BufferStruct.devices[i].input_streams[0].buffers[n].length`
3. **BCE Message 18 Notification (`FUN_1400044e4`):**
   AppleAudio transmits the 837-byte Message 18 packet to BridgeOS to commit the buffer ranges.
   BridgeOS writes incoming ADC DMA samples directly into this host RAM region.

---

## 4. Headset Jack Detection (`'inpt'` Scope)

In addition to the headphone output jack notification, `AppleAudio.sys` monitors microphone plug state:
- When a 4-pole TRRS jack is inserted, BridgeOS detects microphone line impedance.
- BridgeOS sends `msg == 0xd` (`13` = `PROPERTY_CHANGED`) with:
  - `ObjectId == 0x44` (Codec Input)
  - `Scope == 0x696E7074` (`'inpt'`)
  - `Selector == 0x6A61636B` (`'jack'`)
- `AppleAudio.sys` toggles its cached input jack state and invokes `IPortEvents::GenerateEventList` on the `TopologyMicIn` miniport.
- Windows Audio automatically enables the "Headset Microphone" capture endpoint and routes standard communications devices to it.

---

## 5. Architectural Recommendations for `T2AudioPort`

1. **Retain Dual-Endpoint Topology:**
   Following `AppleAudio.sys`, `T2AudioPort` should keep `"Digital Mic"` (internal 3-mic array) and `"Codec Input"` (3.5mm headset mic) as distinct PortCls endpoints rather than attempting to multiplex them into a single pin.
2. **Microphone Beamforming DSP:**
   For `"Digital Mic"`, `T2AudioPort` implements beamforming summation and high-pass EQ (`MicDsp=1`, derived from Linux kaiT2en), downmixing the 3 raw channels to a mono or stereo endpoint.
3. **Headset Mic Streaming:**
   When adding `"Codec Input"` capture support, allocate a dedicated non-paged host buffer with 1 channel 32-bit (or 16-bit) format, register it via Message 18 for stream `0x45`, and trigger jack presence via the existing `JackEvents=1` infrastructure.
