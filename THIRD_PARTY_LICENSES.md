# Third-Party Code Attributions

## kaiT2en (Linux iBridge/T2 Audio Driver)

**Source:** https://github.com/kekrby/linux-t2  
**License:** GPL-2.0  
**Used in:** `src/BceTransport.c`

This project references protocol constants and structures from the Linux kaiT2en driver for Apple T2 audio devices. Specifically:

- BCE (Bridge Co-processor Engine) protocol constants
- Message type definitions (TAG, command/response types)
- Device communication patterns

**Attribution comment in source:**
```c
// T2 BCE Protocol constants (from Linux KAIT2EN)
```

The Windows implementation in this project is original work but relies on protocol knowledge derived from reverse engineering documented in the Linux driver.

**Original Authors:** kekrby and contributors to linux-t2  
**Repository:** https://github.com/kekrby/linux-t2/tree/master/sound/soc/apple

---

## Note on Clean Room Implementation

This Windows driver does NOT contain any copied GPL code. All implementation is original, written for the Windows Driver Model (WDM) and PortCls framework. Protocol constants and structure definitions are derived from publicly documented reverse engineering research.
