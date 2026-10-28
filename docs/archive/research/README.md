# Phase 1: Offline Contract Model

This directory contains user-mode-only layout, parser, ring arithmetic, and QPC
tests. It performs no PCI, BAR, T2, or kernel-driver access.

Build with the Visual Studio x64 Native Tools prompt:

```bat
cl /nologo /W4 /std:c11 /TC DataStructures.h BufferStructParser.c RingBufferMath.c Phase1Test.c /Fe:Phase1Test.exe
Phase1Test.exe > phase1_log.txt
```

The parser uses the verified Windows device-entry stride (`0xBDEC`) explicitly.
That wire-layout assumption is separate from the C compiler's `sizeof` result for
the reconstructed Linux declarations.
