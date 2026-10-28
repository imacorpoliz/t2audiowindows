# EXCERPT 2026-10-28 21:58:00 — Step 3 (Reboot & Crash Guard) & Step 4 (Headphone / Mic Groundwork)

> **Date:** 2026-10-28 21:58:00 +05:00 (16:58:00 UTC)  
> **Machine:** MacBookPro16,1 (PCI `VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01`)  
> **Driver Version:** `1.0.12.0` (`T2AudioMiniport.sys` SHA256: `D75339D0AECA88E503E345D775B7036E43C6D5D377A4EFA02CFA588D4549A359`)  
> **Certificate:** SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE` (Signed and Verified)  
> **Operating Mode:** Persistent Production Mode (`EnableProduction=1`, `CrashGuardArmed=0` (settled), `CrashGuardStrikes=0`)  
> **Tested Registry Settings:** `ExtendedProbe=1`, `JackEvents=1`

---

## 1. Step 3: Reboot & Crash Guard Verification (PASS)

* **Reboot:** Clean Windows 11 restart executed by owner.
* **Verification via `T2Audio-Setup.ps1 -Action Status`:**
  ```text
  ╔══════════════════════════════════════════════════════════╗
  ║          T2AudioPort  ·  звук Apple T2 для Windows       ║
  ║                  MacBook Pro 16" (2019)                  ║
  ╚══════════════════════════════════════════════════════════╝

  ── Состояние 
   Драйвер:        T2AudioPort  1.0.12.0
   Устройство:     OK
   Постоянный режим: включён
   Пресет:         Как на Mac (рекомендуется)
   Тембр:          бас 0 дБ, высокие 0 дБ, разборчивость 0 дБ, компрессор быстрый
   Защита от падений: в порядке
   Громкость T2:   mute=True volume=0,50 L=0,50 R=0,50
   testsigning:    включён
  ```
* **Result:** `CrashGuardStrikes = 0` («Защита от падений: в порядке»).
* **Kernel log confirmation:**
  - `T2Audio: crash guard armed`
  - `T2Audio: persistent mode active (stage 5, mmioCopy=1, bceIo=1)`
  - Clean shutdown disarms guard: `T2Audio: crash guard disarmed (clean end of session)`.

---

## 2. Step 4b: Extended Probe Output (Verbatim)

```text
T2Audio: mode parameters: ForceSystemBuffer=0 MdlVariant=0 TweeterMode=1 OutputAttenuation=1 (-6 dB) ClockRateTracking=0 SpeakerDsp=3 DspGainDb=6 TweeterTrimDb=0 WooferLimitDb=0 DspAutoGain=0 BassDb=0 TrebleDb=0 ClarityDb=0 CompStyle=0 ExtendedProbe=1 JackEvents=1 HeadphoneAutoMute=0
T2Audio: BCE probe: 5 device(s)
T2Audio: BCE probe: device 0x21 uid="Digital Mic" (11 bytes)
T2Audio: BCE detail: device 0x21 output streams: 0 (status 0x00000000)
T2Audio: BCE detail: device 0x21 input streams: 1 (status 0x00000000)
T2Audio: BCE detail: device 0x21 input stream 0x22 pft: rate=48000 id=0x6C70636D flags=0x4 bpp=12 fpp=1 bpf=12 ch=3 bits=24
T2Audio: BCE detail: device 0x21 input stream 0x22 latency=0 (0x00000000)
T2Audio: BCE detail: device 0x21 outp ltnc=42 (0x00000000)
T2Audio: BCE detail: device 0x21 outp saft=113 (0x00000000)
T2Audio: BCE detail: device 0x21 outp jack=0 (0xC0000010)
T2Audio: BCE detail: device 0x21 inpt ltnc=40 (0x00000000)
T2Audio: BCE detail: device 0x21 inpt saft=114 (0x00000000)
T2Audio: BCE detail: device 0x21 inpt jack=0 (0xC0000010)

T2Audio: BCE probe: device 0x25 uid="Codec Output" (12 bytes)
T2Audio: BCE detail: device 0x25 output streams: 1 (status 0x00000000)
T2Audio: BCE detail: device 0x25 output stream 0x26 pft: rate=48000 id=0x6C70636D flags=0x4 bpp=8 fpp=1 bpf=8 ch=2 bits=24
T2Audio: BCE detail: device 0x25 output stream 0x26 latency=0 (0x00000000)
T2Audio: BCE detail: device 0x25 input streams: 0 (status 0x00000000)
T2Audio: BCE detail: device 0x25 outp ltnc=41 (0x00000000)
T2Audio: BCE detail: device 0x25 outp saft=90 (0x00000000)
T2Audio: BCE detail: device 0x25 outp jack=0 (0x00000000)
T2Audio: BCE detail: device 0x25 inpt ltnc=6 (0x00000000)
T2Audio: BCE detail: device 0x25 inpt saft=64 (0x00000000)
T2Audio: BCE detail: device 0x25 inpt jack=0 (0x00000000)
T2Audio: BCE probe: headphone jack device 0x25: unplugged (status 0x00000000)

T2Audio: BCE probe: device 0x39 uid="Speaker" (7 bytes)
T2Audio: BCE detail: device 0x39 output streams: 1 (status 0x00000000)
T2Audio: BCE detail: device 0x39 output stream 0x3A pft: rate=48000 id=0x6C70636D flags=0x4 bpp=24 fpp=1 bpf=24 ch=6 bits=24
T2Audio: BCE detail: device 0x39 output stream 0x3A latency=0 (0x00000000)
T2Audio: BCE detail: device 0x39 input streams: 0 (status 0x00000000)
T2Audio: BCE detail: device 0x39 outp ltnc=11 (0x00000000)
T2Audio: BCE detail: device 0x39 outp saft=71 (0x00000000)
T2Audio: BCE detail: device 0x39 outp jack=0 (0xC0000010)
T2Audio: BCE detail: device 0x39 inpt ltnc=8 (0x00000000)
T2Audio: BCE detail: device 0x39 inpt saft=64 (0x00000000)
T2Audio: BCE detail: device 0x39 inpt jack=0 (0xC0000010)

T2Audio: BCE probe: device 0x3F uid="Bridge Loopback" (15 bytes)
T2Audio: BCE detail: device 0x3F output streams: 1 (status 0x00000000)
T2Audio: BCE detail: device 0x3F output stream 0x40 pft: rate=48000 id=0x6C70636D flags=0x4 bpp=8 fpp=1 bpf=8 ch=2 bits=24
T2Audio: BCE detail: device 0x3F output stream 0x40 latency=0 (0x00000000)
T2Audio: BCE detail: device 0x3F input streams: 1 (status 0x00000000)
T2Audio: BCE detail: device 0x3F input stream 0x41 pft: rate=48000 id=0x6C70636D flags=0x4 bpp=8 fpp=1 bpf=8 ch=2 bits=24
T2Audio: BCE detail: device 0x3F input stream 0x41 latency=0 (0x00000000)
T2Audio: BCE detail: device 0x3F outp ltnc=30 (0x00000000)
T2Audio: BCE detail: device 0x3F outp saft=90 (0x00000000)
T2Audio: BCE detail: device 0x3F outp jack=0 (0xC0000010)
T2Audio: BCE detail: device 0x3F inpt ltnc=13 (0x00000000)
T2Audio: BCE detail: device 0x3F inpt saft=77 (0x00000000)
T2Audio: BCE detail: device 0x3F inpt jack=0 (0xC0000010)

T2Audio: BCE probe: device 0x44 uid="Codec Input" (11 bytes)
T2Audio: BCE detail: device 0x44 output streams: 0 (status 0x00000000)
T2Audio: BCE detail: device 0x44 input streams: 1 (status 0x00000000)
T2Audio: BCE detail: device 0x44 input stream 0x45 pft: rate=48000 id=0x6C70636D flags=0xC bpp=4 fpp=1 bpf=4 ch=1 bits=32
T2Audio: BCE detail: device 0x44 input stream 0x45 latency=0 (0x00000000)
T2Audio: BCE detail: device 0x44 outp ltnc=0 (0x00000000)
T2Audio: BCE detail: device 0x44 outp saft=64 (0x00000000)
T2Audio: BCE detail: device 0x44 outp jack=0 (0xC0000010)
T2Audio: BCE detail: device 0x44 inpt ltnc=47 (0x00000000)
T2Audio: BCE detail: device 0x44 inpt saft=111 (0x00000000)
T2Audio: BCE detail: device 0x44 inpt jack=0 (0xC0000010)

T2Audio: BufferStruct device[0] 'Bridge Loopback' in=0 out=1
T2Audio: BufferStruct device[0] output stream[0]: 1 buffer(s)
T2Audio: BufferStruct device[0] output stream[0] buffer[0]: address=0x10C000 size=0x1EF00

T2Audio: BufferStruct device[1] 'Speaker' in=0 out=1
T2Audio: BufferStruct device[1] output stream[0]: 1 buffer(s)
T2Audio: BufferStruct device[1] output stream[0] buffer[0]: address=0x12C000 size=0x61800

T2Audio: BufferStruct device[2] 'Codec Output' in=0 out=1
T2Audio: BufferStruct device[2] output stream[0]: 1 buffer(s)
T2Audio: BufferStruct device[2] output stream[0] buffer[0]: address=0x190000 size=0x20800

T2Audio: BufferStruct device[3] 'Digital Mic' in=1 out=0
T2Audio: BufferStruct device[3] input stream[0]: 3 buffer(s)
T2Audio: BufferStruct device[3] input stream[0] buffer[0]: address=0x47E2DA000 size=0x3000
T2Audio: BufferStruct device[3] input stream[0] buffer[1]: address=0x47E2BA000 size=0x20000
```

---

## 3. Step 4c: Jack Listener & Event Tracking

* **Registration:**
  `T2Audio: jack listener on device 0x25: registered (0x00000000, reply 128 bytes)` (SUCCESS).
* **Hardware Observation:**
  The owner plugged and unplugged 3.5 mm headphones 3 times (~5 s apart) during capture.
  - Result: No `VHCI PROPERTY_CHANGED` messages were forwarded by `AppleUSBVHCI.sys` / bridgeOS for the 0x25 jack selector under Windows.
  - Speaker audio was completely unaffected (`reAims=1`, `skips=0`, `gaps>10ms=0`).
* **Conclusion according to GEMINI.md Step 4c:**
  > *"If the listener line says FAILED, or no PROPERTY_CHANGED arrives: record it, set JackEvents back to 0, go on with step 5."*
* Parameters restored: `HeadphoneAutoMute=0`, `JackEvents=0`, `ExtendedProbe=0`.
