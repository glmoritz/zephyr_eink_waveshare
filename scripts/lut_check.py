#!/usr/bin/env python3
"""Decode and sanity-check an SSD1677 105-byte waveform LUT.

Usage: lut_check.py <c_file> <array_name>

Layout: 5 rows x 10 VS bytes (4 phases of 2 bits each, MSB first),
then 10 groups x {tpA,tpB,tpC,tpD,repeat}, then 5 FR bytes (one nibble
per group). VS codes: 00=VSS, 01=VSH1 (drive black), 10=VSL (drive
white), 11=VSH2.

Reports, per LUT row: the phase-by-phase drive schedule, total frames,
and the net black-minus-white drive (DC imbalance). A large per-update
imbalance accumulates charge in the film and shows up as permanent
stains — keep it small and symmetric between the black and white rows.
"""

import re
import sys

VS_NAME = {0: "VSS ", 1: "BLK ", 2: "WHT ", 3: "VSH2"}


def extract_array(path: str, name: str) -> list[int]:
    text = open(path).read()
    m = re.search(rf"{name}\s*\[\s*\d*\s*\]\s*=\s*\{{(.*?)\}}\s*;", text, re.S)
    if not m:
        sys.exit(f"array '{name}' not found in {path}")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", m.group(1), flags=re.S)
    vals = [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body)]
    if len(vals) != 105:
        sys.exit(f"expected 105 bytes, got {len(vals)}")
    return vals


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    lut = extract_array(sys.argv[1], sys.argv[2])

    vs = [lut[r * 10:(r + 1) * 10] for r in range(5)]
    tp = [lut[50 + g * 5:50 + (g + 1) * 5] for g in range(10)]
    fr = []
    for b in lut[100:105]:
        fr += [b >> 4, b & 0xF]

    rows = ["L0 (black)", "L1", "L2", "L3 (white)", "L4 (VCOM) "]
    for r in range(5):
        net = 0       # +frames driving black, -frames driving white
        total = 0
        sched = []
        for g in range(10):
            tpa, tpb, tpc, tpd, rp = tp[g]
            durs = (tpa, tpb, tpc, tpd)
            if not any(durs):
                continue
            byte = vs[r][g]
            phases = [(byte >> (6 - 2 * i)) & 0x3 for i in range(4)]
            for _ in range(rp + 1):
                for code, dur in zip(phases, durs):
                    if dur == 0:
                        continue
                    total += dur
                    if code == 1:
                        net += dur
                    elif code == 2:
                        net -= dur
                    sched.append(f"{VS_NAME[code]}x{dur}")
        drive = f"net {'black' if net >= 0 else 'white'} {abs(net)}f"
        print(f"{rows[r]}: {total:3d} frames, {drive:>15}  FRgrp0={fr[0]}")
        if sched:
            print(f"    {' '.join(sched)}")

    l0 = sum(tp[g][i] * (tp[g][4] + 1) for g in range(10) for i in range(4))
    print(f"\nGroups total (per row upper bound): {l0} frames")
    imb = []
    for r in (0, 3):
        net = 0
        for g in range(10):
            tpa, tpb, tpc, tpd, rp = tp[g]
            byte = vs[r][g]
            phases = [(byte >> (6 - 2 * i)) & 0x3 for i in range(4)]
            for code, dur in zip(phases, (tpa, tpb, tpc, tpd)):
                net += dur * (rp + 1) * (1 if code == 1 else -1 if code == 2 else 0)
        imb.append(net)
    if abs(imb[0] + imb[1]) > 4:
        print(f"WARNING: black/white rows not cross-balanced: {imb[0]} vs {imb[1]}")
    else:
        print(f"Cross-balance OK: black row {imb[0]:+d}f, white row {imb[1]:+d}f")


if __name__ == "__main__":
    main()
