# Third-Party Code Attributions

## kaiT2en (Linux iBridge/T2 Audio Driver)

**Source:** https://github.com/kaiT2en/KaiT2en-Fedora (`modules/t2bce_audio`), earlier https://github.com/kekrby/linux-t2  
**License:** GPL-2.0  
**Used in:** `src/BceTransport.c`, `src/BceProtocolLogic.h`, `src/WaveRTStream.c`, `src/Device.c`

This project references protocol constants and structures from the Linux kaiT2en driver for Apple T2 audio devices. Specifically:

- BCE (Bridge Co-processor Engine) protocol constants
- Message type definitions (TAG, command/response types)
- Device communication patterns
- Property selectors (`'pft '`, `'ltnc'`, `'saft'`), the stream physical-format
  descriptor layout and its mapping to a sample layout (S24_LE for the speaker)
- Init / quiesce ordering (ring cleared and clock anchored before `START_IO`,
  `SET_REMOTE_ACCESS(OFF)` on teardown, write-combining for a prefetchable BAR)

**Attribution comment in source:**
```c
// T2 BCE Protocol constants (from Linux KAIT2EN)
```

The Windows implementation in this project is original work but relies on protocol knowledge derived from reverse engineering documented in the Linux driver.

**Original Authors:** kekrby and contributors to linux-t2  
**Repository:** https://github.com/kekrby/linux-t2/tree/master/sound/soc/apple

---

## apple-t2-touchid-windows (osaul4ik)

**Source:** https://github.com/osaul4ik/apple-t2-touchid-windows  
**License:** GPL-2.0-only  
**Used in:** `.github/workflows/build.yml`

The CI workflow (installing the WDK on a GitHub `windows-2022` runner and
building the driver with MSBuild) follows the approach of that project's
`BuildT2TouchId.yml`. No driver code was taken from it.

---

## Note on Clean Room Implementation

This Windows driver does NOT contain any copied GPL code. All implementation is original, written for the Windows Driver Model (WDM) and PortCls framework. Protocol constants and structure definitions are derived from publicly documented reverse engineering research.
