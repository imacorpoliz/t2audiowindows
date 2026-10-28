# Speaker DSP tables

`gen_kait2en_dsp.py` turns the KAIT2EN MacBookPro16,1 speaker profile into the
fixed-point tables of `src/T2AudioDspProfile16_1.h`, used by `src/T2AudioDsp.h`
when `Parameters\SpeakerDsp=1`.

```bash
python3 tools/dsp/gen_kait2en_dsp.py <kait2en>/dsp/profiles/16_1 src/T2AudioDspProfile16_1.h
```

The profile is in the private `imacorpoliz/mbp` workspace under
`upstream-kait2en/dsp/profiles/16_1`. Re-run the script when the profile
changes, then run `tests/T2AudioDspTest.c` (it compares the fixed-point chain
with the double-precision coefficients from the same header).

License: the profile and the generated header are GPL-3.0-or-later with
KAIT2EN's attribution term; see `THIRD_PARTY_LICENSES.md`.
