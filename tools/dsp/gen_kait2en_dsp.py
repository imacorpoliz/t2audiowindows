#!/usr/bin/env python3
# Generates src/T2AudioDspProfile16_1.h from the KAIT2EN MacBookPro16,1
# speaker DSP profile (dsp/profiles/16_1/graph.json + XTC-side-48k.wav).
#
#   python3 tools/dsp/gen_kait2en_dsp.py <kait2en>/dsp/profiles/16_1 src/T2AudioDspProfile16_1.h
#
# Only the LINEAR, 48 kHz part of the graph is exported, in fixed point:
#   Tone1 (12th-order high-pass, per side), XTC (mid delay + side FIR),
#   Tone2 (midrange EQ, per side), and the per-driver "analytical" chains
#   (crossover + EQ) for the woofer and tweeter of each side.
# Not exported (nonlinear): Norm (autogain), VirtBass (bankstown), Comp
# (multiband compressor), Buzz (woofer resonance compressor) and the
# ControlFreak limiters; the driver has its own simple limiter instead.
#
# The profile is Copyright (C) 2026 Andre Eikmeyer <andre.eikmeyer@kait2en.org>,
# GPL-3.0-or-later with the attribution term (section 7b) of KAIT2EN's
# LICENSING.md. The generated header is a derivative work under the same
# terms; this script copies the notice into it.

import json, math, os, struct, sys

Q = 28                    # coefficient fraction bits
RATE = 48000


def load(profile):
    g = json.load(open(os.path.join(profile, "graph.json"), encoding="utf-8"))["filter.graph"]
    return g, {n["name"]: n for n in g["nodes"]}


def coeff(nodes, name):
    for c in nodes[name]["config"]["coefficients"]:
        if c["rate"] == RATE:
            a0 = c["a0"]
            return [c["b0"] / a0, c["b1"] / a0, c["b2"] / a0, c["a1"] / a0, c["a2"] / a0]
    raise SystemExit(f"{name}: no {RATE} Hz coefficients")


def chain(g, first_prefix):
    """Biquads of one analytical chain in link order."""
    names = [n["name"] for n in g["nodes"] if n["name"].startswith(first_prefix)]
    nxt = {}
    for l in g["links"]:
        o, i = l["output"].split(":")[0], l["input"].split(":")[0]
        if o in names and i in names:
            nxt[o] = i
    start = [n for n in names if n not in nxt.values()]
    assert len(start) == 1, (first_prefix, start)
    order = [start[0]]
    while order[-1] in nxt:
        order.append(nxt[order[-1]])
    assert len(order) == len(names)
    return order


def read_fir(path):
    d = open(path, "rb").read()
    assert d[0:4] == b"RIFF" and d[8:12] == b"WAVE"
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        sz = int.from_bytes(d[pos + 4:pos + 8], "little")
        body = d[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            data = body
        pos += 8 + sz + (sz & 1)
    assert fmt[0] == 3 and fmt[1] == 1 and fmt[2] == RATE and fmt[5] == 32, fmt
    return list(struct.unpack("<%df" % (len(data) // 4), data))


def rbj(kind, fc, fs=RATE, qf=1.0 / math.sqrt(2.0)):
    """Audio EQ Cookbook low/high/all-pass (lp/hp as bankstown builds them)."""
    fc = min(max(fc, 1.0), fs * 0.499)
    w = 2.0 * math.pi * fc / fs
    sw, cw = math.sin(w), math.cos(w)
    alpha = sw / (2.0 * qf)
    if kind == "lp":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
    elif kind == "hp":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
    else:
        b = [1 - alpha, -2 * cw, 1 + alpha]
    a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
    return [b[0] / a0, b[1] / a0, b[2] / a0, a1 / a0, a2 / a0]


def q(x):
    v = int(round(x * (1 << Q)))
    assert -(1 << 31) <= v < (1 << 31), x
    return v


def main(profile, out):
    g, nodes = load(profile)
    # The graph must be symmetric for the per-side tables used here.
    for i in range(6):
        assert coeff(nodes, f"Tone1_c0_f1_{i}") == coeff(nodes, f"Tone1_c1_f1_{i}")
    tone1 = [coeff(nodes, f"Tone1_c0_f1_{i}") for i in range(6)]
    t2names = ["f1_0", "f2_0", "f3_0", "f5_0"]
    for k in t2names:
        assert coeff(nodes, f"Tone2_c0_{k}") == coeff(nodes, f"Tone2_c1_{k}")
    tone2 = [coeff(nodes, f"Tone2_c0_{k}") for k in t2names]
    chains = {c: [coeff(nodes, n) for n in chain(g, f"analytical_c{c}_")] for c in range(6)}
    assert chains[0] == chains[1] == chains[3] == chains[4], "woofer chains differ"
    assert chains[2] == chains[5], "tweeter chains differ"
    woofer, tweeter = chains[0], chains[2]
    delay = nodes["XTC_mid_delay"]["control"]["Delay (s)"]
    delay_frames = int(round(delay * RATE))
    assert abs(delay * RATE - delay_frames) < 1e-6, delay
    fir = read_fir(os.path.join(profile, "XTC-side-48k.wav"))
    mix = {n: nodes[n]["control"] for n in ["XTC_mid", "XTC_side", "XTC_l", "XTC_r"]}
    assert mix["XTC_mid"] == {"Gain 1": 0.5, "Gain 2": 0.5}
    assert mix["XTC_side"] == {"Gain 1": 0.5, "Gain 2": -0.5}
    assert mix["XTC_l"] == {"Gain 1": 1.0, "Gain 2": 1.0}
    assert mix["XTC_r"] == {"Gain 1": 1.0, "Gain 2": -1.0}

    # Virtual bass (bankstown, MIT, Copyright (C) 2023 James Calligeros):
    # HP(floor) -> LP(ceil) -> saturate * amt -> HP(final_hp) -> LP(3*ceil),
    # added to the dry signal. final_hp is not set in the graph; 200 Hz is
    # bankstown's default (as in SpeakerDSP-APO's port).
    vb = nodes["VirtBass"]["control"]
    assert vb.get("bypass", 0) == 0
    final_hp = 200.0
    vbq = [rbj("hp", vb["floor"]), rbj("lp", vb["ceil"]), rbj("hp", final_hp), rbj("lp", vb["ceil"] * 3.0)]
    k = math.pi / (0.5 + math.e)
    # saturate(s) = k * (blend * tanh(s * third) + (s <= 0) * (1 - blend) * tanh(s * second))
    vb_third = k * vb["amt"] * vb["blend"]
    vb_second = k * vb["amt"] * (1.0 - vb["blend"])
    tanh_n, tanh_max = 2048, 8.0     # step 1/256

    # Multiband compressor "Comp" (lsp mb_compressor_stereo in the graph):
    # 4 bands split at sf_1..sf_3, downward compression per band (threshold
    # al_k, ratio cr_k, makeup mk_k, attack at_k / release rt_k in ms), input
    # gain g_in. Reproduced with a Linkwitz-Riley 4 crossover tree (split at
    # the middle frequency first, all-pass compensated so the bands sum flat),
    # a peak envelope linked over L/R and a hard knee.
    cp = nodes["Comp"]["control"]
    assert cp["mode"] == 0 and cp["g_out"] == 1.0
    splits = sorted([cp["sf_1"], cp["sf_2"], cp["sf_3"]])
    assert splits == [cp["sf_1"], cp["sf_2"], cp["sf_3"]]
    for k in range(4):
        assert cp["ce_%d" % k] == 1 and cp["cm_%d" % k] == 0, k
    f1, f2, f3 = splits
    xover = [rbj("lp", f2), rbj("hp", f2), rbj("ap", f3), rbj("ap", f1),
             rbj("lp", f1), rbj("hp", f1), rbj("lp", f3), rbj("hp", f3)]
    comp_bands = []
    for k in range(4):
        al, cr, mk = cp["al_%d" % k], cp["cr_%d" % k], cp["mk_%d" % k]
        att = 1.0 - math.exp(-1.0 / (cp["at_%d" % k] * 1e-3 * RATE))
        rel = 1.0 - math.exp(-1.0 / (cp["rt_%d" % k] * 1e-3 * RATE))
        comp_bands.append((al, cr, mk, att, rel))

    L = []
    L.append("// GENERATED by tools/dsp/gen_kait2en_dsp.py - do not edit.")
    L.append("//")
    L.append("// MacBookPro16,1 speaker DSP, linear 48 kHz part, fixed point (Q%d)." % Q)
    L.append("// Derived from the KAIT2EN profile dsp/profiles/16_1 (graph.json,")
    L.append("// XTC-side-48k.wav):")
    L.append("//   Copyright (C) 2026 Andre Eikmeyer <andre.eikmeyer@kait2en.org>")
    L.append("//   SPDX-License-Identifier: GPL-3.0-or-later")
    L.append("//   with the attribution term (GPL-3.0 section 7b) of KAIT2EN LICENSING.md:")
    L.append("//   keep this notice when redistributing or deriving from this file.")
    L.append("// A driver binary built with this file is subject to GPL-3.0-or-later.")
    L.append("// See THIRD_PARTY_LICENSES.md.")
    L.append("")
    L.append("#ifndef T2AUDIO_DSP_PROFILE_16_1_H")
    L.append("#define T2AUDIO_DSP_PROFILE_16_1_H")
    L.append("")
    L.append("#define T2DSP_Q %d" % Q)
    L.append("#define T2DSP_TONE1_STAGES %d" % len(tone1))
    L.append("#define T2DSP_TONE2_STAGES %d" % len(tone2))
    L.append("#define T2DSP_WOOFER_STAGES %d" % len(woofer))
    L.append("#define T2DSP_TWEETER_STAGES %d" % len(tweeter))
    L.append("#define T2DSP_XTC_DELAY %d" % delay_frames)
    L.append("#define T2DSP_XTC_TAPS %d" % len(fir))
    L.append("")
    L.append("// Biquads {b0, b1, b2, a1, a2}, a0 normalized to 1.")
    for name, tab in [("Tone1", tone1), ("Tone2", tone2), ("Woofer", woofer), ("Tweeter", tweeter)]:
        L.append("static const int g_T2Dsp%s[%d][5] = {" % (name, len(tab)))
        for c in tab:
            L.append("    { %s }," % ", ".join(str(q(x)) for x in c))
        L.append("};")
    L.append("// Virtual bass (bankstown, MIT, Copyright (C) 2023 James Calligeros),")
    L.append("// settings from the graph: amt=%g floor=%g ceil=%g sat_second=%g sat_third=%g blend=%g," % (
        vb["amt"], vb["floor"], vb["ceil"], vb["sat_second"], vb["sat_third"], vb["blend"]))
    L.append("// final_hp=%g (bankstown default). Filters: HP floor, LP ceil, HP final, LP 3*ceil." % final_hp)
    L.append("static const int g_T2DspVirtBass[4][5] = {")
    for c in vbq:
        L.append("    { %s }," % ", ".join(str(q(x)) for x in c))
    L.append("};")
    L.append("// tanh argument scales (Q%d) and output gains (Q%d): k*amt*blend for the" % (Q, Q))
    L.append("// third-harmonic term, k*amt*(1-blend) for the second (negative half only),")
    L.append("// k = pi / (0.5 + e).")
    L.append("#define T2DSP_VB_THIRD_SCALE %d" % q(vb["sat_third"]))
    L.append("#define T2DSP_VB_SECOND_SCALE %d" % q(vb["sat_second"]))
    L.append("#define T2DSP_VB_THIRD_GAIN %d" % q(vb_third))
    L.append("#define T2DSP_VB_SECOND_GAIN %d" % q(vb_second))
    L.append("// tanh(i / %d) for i = 0..%d, Q30; beyond the table tanh = 1." % (tanh_n // int(tanh_max), tanh_n))
    L.append("#define T2DSP_TANH_STEPS_PER_UNIT %d" % (tanh_n // int(tanh_max)))
    L.append("#define T2DSP_TANH_ENTRIES %d" % (tanh_n + 1))
    L.append("static const int g_T2DspTanh[%d] = {" % (tanh_n + 1))
    vals = [int(round(math.tanh(i / (tanh_n / tanh_max)) * (1 << 30))) for i in range(tanh_n + 1)]
    for i in range(0, len(vals), 8):
        L.append("    " + ", ".join(str(v) for v in vals[i:i + 8]) + ",")
    L.append("};")
    L.append("// Multiband compressor (graph node Comp): crossover at %.1f / %.1f / %.1f Hz." % (f1, f2, f3))
    L.append("// Biquads: LP f2, HP f2, AP f3 (low branch), AP f1 (high branch),")
    L.append("//          LP f1, HP f1, LP f3, HP f3; LP/HP are used twice (LR4).")
    L.append("static const int g_T2DspCompXover[8][5] = {")
    for c in xover:
        L.append("    { %s }," % ", ".join(str(q(x)) for x in c))
    L.append("};")
    L.append("#define T2DSP_COMP_INPUT_GAIN %d   // g_in, Q%d" % (q(cp["g_in"]), Q))
    L.append("// Per band: threshold log2 (Q16), slope (1 - 1/ratio, Q16), makeup (Q%d)," % Q)
    L.append("// attack and release one-pole coefficients (Q30).")
    L.append("static const long long g_T2DspCompBand[4][5] = {")
    for (al, cr, mk, att, rel) in comp_bands:
        L.append("    { %d, %d, %d, %d, %d },   // al=%.4f cr=%.4f mk=%.4f" % (
            int(round(math.log2(al) * 65536)), int(round((1.0 - 1.0 / cr) * 65536)), q(mk),
            int(round(att * (1 << 30))), int(round(rel * (1 << 30))), al, cr, mk))
    L.append("};")
    L.append("// log2(1 + i/256) and 2^(i/256) for i = 0..256, Q16 and Q30.")
    L.append("static const int g_T2DspLog2[257] = {")
    v = [int(round(math.log2(1 + i / 256.0) * 65536)) for i in range(257)]
    for i in range(0, 257, 12):
        L.append("    " + ", ".join(str(x) for x in v[i:i + 12]) + ",")
    L.append("};")
    L.append("static const long long g_T2DspExp2[257] = {")
    v = [int(round(2 ** (i / 256.0) * (1 << 30))) for i in range(257)]
    for i in range(0, 257, 8):
        L.append("    " + ", ".join(str(x) for x in v[i:i + 8]) + ",")
    L.append("};")
    L.append("static const int g_T2DspXtcFir[%d] = {" % len(fir))
    for i in range(0, len(fir), 8):
        L.append("    " + ", ".join(str(q(x)) for x in fir[i:i + 8]) + ",")
    L.append("};")
    L.append("")
    L.append("#ifdef T2AUDIO_DSP_REFERENCE")
    L.append("// Double-precision originals, for host tests only.")
    for name, tab in [("Tone1", tone1), ("Tone2", tone2), ("Woofer", woofer), ("Tweeter", tweeter)]:
        L.append("static const double g_T2DspRef%s[%d][5] = {" % (name, len(tab)))
        for c in tab:
            L.append("    { %s }," % ", ".join(repr(x) for x in c))
        L.append("};")
    L.append("static const double g_T2DspRefVirtBass[4][5] = {")
    for c in vbq:
        L.append("    { %s }," % ", ".join(repr(x) for x in c))
    L.append("};")
    L.append("static const double g_T2DspRefVb[4] = { %r, %r, %r, %r }; // third scale, second scale, third gain, second gain" % (
        vb["sat_third"], vb["sat_second"], vb_third, vb_second))
    L.append("static const double g_T2DspRefCompXover[8][5] = {")
    for c in xover:
        L.append("    { %s }," % ", ".join(repr(x) for x in c))
    L.append("};")
    L.append("static const double g_T2DspRefCompInputGain = %r;" % cp["g_in"])
    L.append("// threshold (linear), ratio, makeup, attack coef, release coef")
    L.append("static const double g_T2DspRefCompBand[4][5] = {")
    for b in comp_bands:
        L.append("    { %s }," % ", ".join(repr(x) for x in b))
    L.append("};")
    L.append("static const double g_T2DspRefXtcFir[%d] = {" % len(fir))
    for i in range(0, len(fir), 6):
        L.append("    " + ", ".join(repr(x) for x in fir[i:i + 6]) + ",")
    L.append("};")
    L.append("#endif")
    L.append("")
    L.append("#endif")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("wrote", out, "tone1", len(tone1), "tone2", len(tone2), "woofer", len(woofer),
          "tweeter", len(tweeter), "fir", len(fir), "delay", delay_frames)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__ or "usage: gen_kait2en_dsp.py <profile dir> <out .h>")
    main(sys.argv[1], sys.argv[2])
