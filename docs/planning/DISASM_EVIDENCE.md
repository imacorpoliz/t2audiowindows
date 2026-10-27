# AppleUSBVHCI.sys IOCTL Analysis - Raw Disassembly Evidence

This file contains the complete raw disassembly traces used in the analysis.

## Script Used

disasm_vhci.py - Custom capstone-based PE disassembler with RVA annotation

Usage:
```
py disasm_vhci.py trace_handler <rva>
py disasm_vhci.py vtable <rva>
py disasm_vhci.py dis <start> <end>
py disasm_vhci.py function <startRVA>
py disasm_vhci.py check_cfg <rva>
```

## Traces Collected

1. trace_0x222004.txt - IOCTL 0x222004 handler 0xDB7C
2. trace_0x222008.txt - IOCTL 0x222008 handler 0xDC5C
3. trace_0x22200C.txt - IOCTL 0x22200C handler 0xDAF0
4. trace_0x222010.txt - IOCTL 0x222010 handler 0xDC24
5. trace_0x222014.txt - IOCTL 0x222014 handler 0xDBEC
6. vtable_0x23F78.txt - Vtable structure verification
7. vtable_func_0xE150.txt - Vtable slot +0x38 (IOCTL 0x222004)
8. vtable_func_0xE310_full.txt - Complete .pdata-bounded [0xE310,0xE4D5) body with bytes and CFG RAX provenance
9. vtable_func_0xDE30.txt - Vtable slot +0x30 (IOCTL 0x22200C)
10. vtable_func_0xE1C0.txt - Vtable slot +0x10 (IOCTL 0x222010)
11. vtable_func_0xE180.txt - Vtable slot +0x18 (IOCTL 0x222014)
12. vtable_func_0xE7C0.txt - Constructor (vtable +0x00)
13. vtable_func_0xE850.txt - Destructor (vtable +0x08)
14. vtable_func_0xE090.txt - List insertion (vtable +0x20)
15. wrapper_0x8A54.txt - Lock wrapper function
16. helper_0xE618.txt - List search helper
17. cfg_check.txt - PE Load Config verification that 0x1E140 is GuardCFDispatchFunctionPointer
18. func_0x7A68.txt - nested descriptor wrapper producer
19. func_0xE8F8.txt - entry allocator and deeper vtable dispatch
20. func_0xDE30_full.txt - 0x22200C consumer path
21. vtable_0x23F78_extended.txt - vtable slots through +0x78

All traces are included in the artifacts directory.
