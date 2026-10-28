# Prompt for Gemini (installer, persistent mode, reboot; sound allowed)

Paste the text below into Gemini as its task. Details: the "▶ NEXT RUN" block in `GEMINI.md`, `docs/INSTALL.md` and the first entry in `docs/CURRENT_STATE.md`.

```text
You work in the repository imacorpoliz/t2audiowindows on a Windows machine with the WDK and on the test MacBookPro16,1. You are the builder and tester of the T2AudioPort driver.

OWNER PERMISSIONS FOR THIS SESSION: sound is allowed; installing the driver in persistent mode with the installer tools\T2Audio-Setup.ps1 is allowed; restarting Windows is allowed. Mac sleep (step 6) only if the owner separately says "I allow sleep" («разрешаю сон»).

Read: the "▶ NEXT RUN" block in GEMINI.md (steps 1–8 in order), docs/INSTALL.md, §1 "Hard rules", the first entry in docs/CURRENT_STATE.md.

Goal: verify on hardware what Claude built for daily use:
1) build 1.0.12.0 into packaging\ (signed .sys/.cat/.inf);
2) install: T2Audio-Setup.ps1 -Action Install -Preset macos;
3) ordinary reboot: the driver starts by itself, CrashGuardStrikes stays 0 (the main test);
4) the headphone jack line in the log, with headphones plugged in and unplugged;
5) presets bright / loud / maxvolume / even / macos (even = autogain: is the level more even between a quiet and a loud recording, any "pumping"? autoGainDb10 from run summary);
6) sleep (only with permission);
7) T2Audio-Setup.ps1 -Action Uninstall: back to AppleAudio.

The installer's UI is in Russian; GEMINI.md gives the English meaning of the strings to check.

Capture the kernel log with DbgView from the start of every step. Report: docs/logs/EXCERPT_*.md (installer output; log lines: persistent mode, crash guard, headphone jack, mode parameters, run summary with maxCopyUs; the owner's words verbatim with an English translation), an entry in docs/CURRENT_STATE.md, a PR into diagnostics. All documentation in English. "Proven" means verified only.

Rules:
- A failure or BSOD: stop. After the reboot run T2Audio-Setup.ps1 -Action Status (it shows the crash-guard strike), take the minidump from C:\Windows\Minidump, write the report, ask the owner. Do not fix code.
- If the crash guard shows a strike after an ordinary reboot: stop, this is a bug for Claude.
- At the end, as the owner decides: either keep it installed (preset macos) or -Action Uninstall. Record the choice.
- Never push to diagnostics directly, only a PR.
```
