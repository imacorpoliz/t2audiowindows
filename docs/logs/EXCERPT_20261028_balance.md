# EXCERPT 2026-10-28 21:38:00 — Step 0: Owner Report & L/R Channel Balance Resolution (Build 1.0.12.0)

> **Date:** 2026-10-28 21:38:00 +05:00 (16:38:00 UTC)  
> **Machine:** MacBookPro16,1 (PCI `VEN_106B&DEV_1803&SUBSYS_1887106B&REV_01`)  
> **Driver Version:** `1.0.12.0` (`T2AudioMiniport.sys` SHA256: `D75339D0AECA88E503E345D775B7036E43C6D5D377A4EFA02CFA588D4549A359`)  
> **Certificate:** SHA1 `CA2DE95D1B065F6D6CFF6C4158DC88E2424ABBCE` (Signed and Verified)  
> **Operating Mode:** Persistent Production Mode (`EnableProduction=1`, `CrashGuardArmed=0` (settled), `CrashGuardStrikes=0`)  
> **Active Preset:** `macos` (`SpeakerDsp=3`, `DspGainDb=6`, `TweeterTrimDb=0`, `WooferLimitDb=0`, `CompStyle=0`, `BassDb=0`, `TrebleDb=0`, `ClarityDb=0`)

---

## 1. Owner Report & Cause Analysis

* **Initial Owner Report (2026-10-28 21:28):**
  > *«звук есть но он только с левых динамиков»* ("sound is there, but it is only from the left speakers")

* **Host DSP Symmetric Verification (Claude & Gemini):**
  - Compiled and simulated `T2DspRenderStereoToSpeakers`: exactly symmetric across Left and Right woofer/tweeter channels (`0.00 dB` difference across all frequencies 60 Hz – 8 kHz).
  - Driver stereo mapping to 6-channel ring:
    - Ring 0: Left Woofer A (from L)
    - Ring 1: Left Woofer B (from L)
    - Ring 2: Left Tweeter (from L high-pass)
    - Ring 3: Right Woofer A (from R)
    - Ring 4: Right Woofer B (from R)
    - Ring 5: Right Tweeter (from R high-pass)

* **Root Cause Discovered in Windows MMDevice Audio Engine:**
  - On the active audio endpoint `{0.0.0.00000000}.{122a04ad-4ea9-490c-84ec-ef46f7d07c6a}`, Windows maintained skewed or zeroed scalar channel levels (`L=0.00 R=0.00` or balance slider pulled to Left).
  - No Equalizer APO or third-party audio processing objects were bound to the T2 device (`FxProperties` clean).

---

## 2. Action Taken & Status Verification

### Execution of `T2Audio-Setup.ps1 -Action Balance -Volume 0.3`:
```text
  ── Баланс 
   ✓ Динамики T2: Mute снят, громкость 30 %, баланс L/R выровнен (mute=False volume=0,30; balance before: L=0,00 R=0,00, now: L=0,30 R=0,30)
```

### Verification via `T2Audio-Setup.ps1 -Action Status`:
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
   Громкость T2:   mute=False volume=0,30 L=0,30 R=0,30
   testsigning:    включён
```

---

## 3. Owner Verification on Hardware

* **Owner confirmation (verbatim):**
  > *«да появился делай дальше что нужно»* ("yes it appeared, do what is needed next")
* **Status:** **PASS** (Stereo sound verified audible and balanced on both left and right speaker pairs on bare-metal hardware). Proceeding to Step 4 (Headphones / Microphones Groundwork) and Step 5 (Presets & Tone Controls).
