"""Generate Hardware-C page 12, revision 3.

Revision 2 (four APM key channels sharing one 13-bit ladder) plus:
  - an idle-hold branch per channel: TLP176AM + 2.2 kOhm + AD5292 50 kOhm rheostat, so every channel can sit
    at its learned idle level while another channel is pressed;
  - U808 (second TCA9539 @ 0x75) bit-bangs the AD5292 SPI and drives the idle relays;
  - U817 TPS61040 boosts +5V to WS_VPOT (about 25.9 V) for the AD5292 VDD;
  - the ladder and connector returns move to WS_RET, tied to GND at one point (R844);
  - C802 10 nF -> 1 nF so pulsed APM pull-ups can be sampled.

usage: python3 gen_page12_v3.py out.html [preview.svg] [netlist.json]
"""
import html
import json
import sys
from collections import defaultdict

OUT = sys.argv[1]
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None
NETLIST_OUT = sys.argv[3] if len(sys.argv) > 3 else None

# b0..b11 as revision 1, b12 = 100 ohm (revision 2); b13 = 47 ohm and b14 = 150 kOhm (revision 3, on U808)
# extend the ladder for APM pull-ups other than 2.2 kOhm: b13 for 1 kOhm pull-ups near 200 mV, b14 for 10 kOhm near 4.5 V
BITS = [200, 390, 820, 1600, 3300, 6200, 13000, 24000, 47000, 91000, 180000, 360000, 100, 47, 150000]
BIT_REF = {12: "R831", 13: "R845", 14: "R846"}
BIT_Q = {12: "Q808A", 13: "Q809A", 14: "Q809B"}
NB = len(BITS)
CH = 4

# idle-hold branch
POT_OHM, POT_CODES, POT_WIPER = 50000, 1024, 80          # AD5292BRUZ-50, typical wiper resistance 60-100 ohm
IDLE_RS = 2200                                            # R827-R830
VPOT = 1.233 * (1 + 1_000_000 / 49_900)                   # TPS61040: R841 = 1 MOhm, R842 = 49.9 kOhm


def fmt_ohm(r):
    if r >= 1000:
        return f"{r / 1000:g}k"
    return str(r)


# ---------------------------------------------------------------- netlist
parts = {}


def add(ref, part, pins):
    parts[ref] = {"part": part, "pins": {str(k): {"pin_name": n, "net": net} for k, (n, net) in pins.items()}}


def gate_net(i):
    return f"WS_G{i}"


u801 = {1: ("INT", None), 2: ("A1", "GND"), 3: ("RESET", "WS_RST_N"), 12: ("GND", "GND"),
        21: ("A0", "GND"), 22: ("SCL", "EXP_SCL"), 23: ("SDA", "EXP_SDA"), 24: ("VCC", "+3V3")}
P1_NETS = ["WS_G8", "WS_G9", "WS_G10", "WS_G11", "WS_SHORT_G", "WS_G12", "CH_A0", "CH_A1"]
for i in range(8):
    u801[4 + i] = (f"P0{i}", f"WS_G{i}")
    u801[13 + i] = (f"P1{i}", P1_NETS[i])
add("U801", "TCA9539PWR", u801)
add("C801", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
add("R814", "10kΩ", {1: ("1", "WS_RST_N"), 2: ("2", "GND")})
for i in range(12):
    add(f"R{801 + i}", f"{fmt_ohm(BITS[i])}Ω 1%", {1: ("1", "WS_NODE"), 2: ("2", f"WS_D{i}")})
add("R831", "100Ω 1%", {1: ("1", "WS_NODE"), 2: ("2", "WS_D12")})
add("R845", "47Ω 1%", {1: ("1", "WS_NODE"), 2: ("2", "WS_D13")})
add("R846", "150kΩ 1%", {1: ("1", "WS_NODE"), 2: ("2", "WS_D14")})
add("Q809", "BSS138DW", {1: ("S1", "WS_RET"), 2: ("G1", "WS_G13"), 3: ("D2", "WS_D14"),
                         4: ("S2", "WS_RET"), 5: ("G2", "WS_G14"), 6: ("D1", "WS_D13")})
add("R847", "100kΩ", {1: ("1", "WS_G13"), 2: ("2", "WS_RET")})
add("R848", "100kΩ", {1: ("1", "WS_G14"), 2: ("2", "WS_RET")})
# Ladder switches return to WS_RET (the APM switch return on J801), not to the logic ground.
for q in range(6):
    a, b = 2 * q, 2 * q + 1
    add(f"Q{801 + q}", "BSS138DW", {1: ("S1", "WS_RET"), 2: ("G1", f"WS_G{a}"), 3: ("D2", f"WS_D{b}"),
                                    4: ("S2", "WS_RET"), 5: ("G2", f"WS_G{b}"), 6: ("D1", f"WS_D{a}")})
# Q808: half A switches b12 (to WS_RET), half B inverts WS_RST_N into the channel enable (logic GND)
add("Q808", "BSS138DW", {1: ("S1", "WS_RET"), 2: ("G1", "WS_G12"), 3: ("D2", "CH_EN_N"),
                         4: ("S2", "GND"), 5: ("G2", "WS_RST_N"), 6: ("D1", "WS_D12")})
add("Q807", "AO3400A", {1: ("G", "WS_SHORT_G"), 2: ("S", "WS_RET"), 3: ("D", "WS_NODE")})
for n in range(3):
    pins = {}
    for k in range(4):
        pins[1 + k] = (str(1 + k), f"WS_G{4 * n + k}")
        pins[8 - k] = (str(8 - k), "WS_RET")
    add(f"RN{801 + n}", "100kΩ ×4", pins)
add("R813", "100kΩ", {1: ("1", "WS_SHORT_G"), 2: ("2", "WS_RET")})
add("R832", "100kΩ", {1: ("1", "WS_G12"), 2: ("2", "WS_RET")})

# per channel: J801 -> TVS -> PTC -> photorelay -> WS_NODE; 300k sense tap at the connector side
SENSE_PIN = {1: 12, 2: 14, 3: 15, 4: 11}       # CD74HC4052 A0..A3
SEL_PIN = {1: 4, 2: 5, 3: 6, 4: 7}             # SN74LVC139A 1Y0..1Y3
for k in range(1, CH + 1):
    add(f"D80{k}", "SMF15CA", {1: ("1", f"WS_K{k}"), 2: ("2", "GND")})
    add(f"F80{k}", "PTC 50mA", {1: ("1", f"WS_K{k}"), 2: ("2", f"WS_KF{k}")})
    add(f"U80{k + 1}", "TLP176AM", {1: ("Anode", f"WS_LA{k}"), 3: ("Cathode", f"SEL{k}_N"),
                                     4: ("Drain", "WS_NODE"), 6: ("Drain", f"WS_KF{k}")})
    add(f"R{819 + k}", "330Ω 1%", {1: ("1", "+3V3"), 2: ("2", f"WS_LA{k}")})
    add(f"R{814 + k}", "300kΩ 0.1%", {1: ("1", f"WS_K{k}"), 2: ("2", f"WS_S{k}")})
add("R819", "56kΩ 0.1%", {1: ("1", "WS_ADC"), 2: ("2", "GND")})
add("C802", "1nF", {1: ("1", "WS_ADC"), 2: ("2", "GND")})
add("R824", "10kΩ", {1: ("1", "+3V3"), 2: ("2", "CH_EN_N")})
add("R825", "100kΩ", {1: ("1", "CH_A0"), 2: ("2", "GND")})
add("R826", "100kΩ", {1: ("1", "CH_A1"), 2: ("2", "GND")})
u806 = {1: ("1G", "CH_EN_N"), 2: ("1A", "CH_A0"), 3: ("1B", "CH_A1"), 8: ("GND", "GND"),
        9: ("2Y3", None), 10: ("2Y2", None), 11: ("2Y1", None), 12: ("2Y0", None),
        13: ("2B", "GND"), 14: ("2A", "GND"), 15: ("2G", "+3V3"), 16: ("VCC", "+3V3")}
for k in range(1, CH + 1):
    u806[SEL_PIN[k]] = (f"1Y{k - 1}", f"SEL{k}_N")
add("U806", "SN74LVC139APWR", u806)
u807 = {1: ("B0", "GND"), 2: ("B2", "GND"), 3: ("COM B", "GND"), 4: ("B3", "GND"), 5: ("B1", "GND"),
        6: ("E", "CH_EN_N"), 7: ("VEE", "GND"), 8: ("GND", "GND"), 9: ("S1", "CH_A1"), 10: ("S0", "CH_A0"),
        13: ("COM A", "WS_ADC"), 16: ("VCC", "+3V3")}
for k in range(1, CH + 1):
    u807[SENSE_PIN[k]] = (f"A{k - 1}", f"WS_S{k}")
add("U807", "CD74HC4052PWR", u807)
add("C803", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
add("C804", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
j801 = {}
for k in range(1, CH + 1):
    j801[2 * k - 1] = (str(2 * k - 1), f"WS_K{k}")
    j801[2 * k] = (str(2 * k), "WS_RET")
add("J801", "KF2EDGR-3.81-8P", j801)
add("R844", "0Ω", {1: ("1", "WS_RET"), 2: ("2", "GND")})   # the single WS_RET-to-GND tie

# ---- revision 3: idle hold
# U808: second TCA9539, A0 = +3V3 -> 0x75. Port 0 bit-bangs the AD5292 SPI, port 1 drives the idle relays.
U808_P0 = ["POT_SCLK", "POT_DIN", "POT_SYNC1_N", "POT_SYNC2_N", "POT_SYNC3_N", "POT_SYNC4_N", "VPOT_EN", "WS_G13"]
U808_P1 = ["IDLE1_N", "IDLE2_N", "IDLE3_N", "IDLE4_N", "WS_G14", None, None, None]
u808 = {1: ("INT", None), 2: ("A1", "GND"), 3: ("RESET", "WS_RST_N"), 12: ("GND", "GND"),
        21: ("A0", "+3V3"), 22: ("SCL", "EXP_SCL"), 23: ("SDA", "EXP_SDA"), 24: ("VCC", "+3V3")}
for i in range(8):
    u808[4 + i] = (f"P0{i}", U808_P0[i])
    u808[13 + i] = (f"P1{i}", U808_P1[i])
add("U808", "TCA9539PWR", u808)
add("C808", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
for k in range(1, CH + 1):
    # idle relay: WS_KFk (after the PTC) -> R827-R830 -> AD5292 W; B returns to WS_RET
    add(f"U{812 + k}", "TLP176AM", {1: ("Anode", f"WS_ILA{k}"), 3: ("Cathode", f"IDLE{k}_N"),
                                     4: ("Drain", f"WS_IR{k}"), 6: ("Drain", f"WS_KF{k}")})
    add(f"R{832 + k}", "330Ω 1%", {1: ("1", "+3V3"), 2: ("2", f"WS_ILA{k}")})
    add(f"R{826 + k}", "2.2kΩ 1%", {1: ("1", f"WS_IR{k}"), 2: ("2", f"WS_IW{k}")})
    add(f"U{808 + k}", "AD5292BRUZ-50", {
        1: ("RESET", "WS_RST_N"), 2: ("VSS", "GND"), 3: ("A", None), 4: ("W", f"WS_IW{k}"),
        5: ("B", "WS_RET"), 6: ("VDD", "WS_VPOT"), 7: ("EXT_CAP", f"WS_XC{k}"), 8: ("VLOGIC", "+3V3"),
        9: ("GND", "GND"), 10: ("DIN", "POT_DIN"), 11: ("SCLK", "POT_SCLK"), 12: ("SYNC", f"POT_SYNC{k}_N"),
        13: ("SDO", None), 14: ("RDY", None)})
    add(f"C{808 + k}", "1µF 16V", {1: ("1", f"WS_XC{k}"), 2: ("2", "GND")})
    add(f"C{812 + k}", "100nF 50V", {1: ("1", "WS_VPOT"), 2: ("2", "GND")})
    add(f"C{816 + k}", "100nF", {1: ("1", "+3V3"), 2: ("2", "GND")})
    add(f"R{836 + k}", "10kΩ", {1: ("1", "+3V3"), 2: ("2", f"POT_SYNC{k}_N")})
# U817: +5V -> WS_VPOT boost for the AD5292 VDD; off (VPOT_EN low) whenever U808 is in reset
add("U817", "TPS61040DBVR", {1: ("SW", "WS_BSW"), 2: ("GND", "GND"), 3: ("FB", "WS_BFB"),
                             4: ("EN", "VPOT_EN"), 5: ("VIN", "+5V")})
add("L801", "10µH", {1: ("1", "+5V"), 2: ("2", "WS_BSW")})
add("D805", "B5819W", {1: ("K", "WS_VPOT"), 2: ("A", "WS_BSW")})
add("R841", "1MΩ 1%", {1: ("1", "WS_VPOT"), 2: ("2", "WS_BFB")})
add("C805", "10pF", {1: ("1", "WS_VPOT"), 2: ("2", "WS_BFB")})
add("R842", "49.9kΩ 1%", {1: ("1", "WS_BFB"), 2: ("2", "GND")})
add("R843", "100kΩ", {1: ("1", "VPOT_EN"), 2: ("2", "GND")})
add("C806", "4.7µF 10V", {1: ("1", "+5V"), 2: ("2", "GND")})
add("C807", "4.7µF 50V", {1: ("1", "WS_VPOT"), 2: ("2", "GND")})

mcu_changes = {"U301": {"87": {"pin_name": "PG2", "net": "WS_RST_N"},
                        "28": {"pin_name": "PC2_C", "net": "WS_ADC"}}}

net_pins = defaultdict(list)
for ref, d in parts.items():
    for p, v in d["pins"].items():
        if v["net"]:
            net_pins[v["net"]].append(f"{ref}.{p}")
for p, v in mcu_changes["U301"].items():
    net_pins[v["net"]].append(f"U301.{p}")
existing = {"GND", "+3V3", "+5V", "EXP_SCL", "EXP_SDA"}
single = [n for n, ps in net_pins.items() if len(ps) < 2 and n not in existing]
assert not single, single
n_parts = len(parts)
n_dnp = sum(1 for d in parts.values() if d["part"] == "DNP")
n_pins = sum(len(d["pins"]) for d in parts.values())
new_nets = sorted(n for n in net_pins if n not in existing)
netlist_json = json.dumps({"revision": 3, "new_parts": parts, "changed_pins": mcu_changes},
                          ensure_ascii=False, indent=1)
if NETLIST_OUT:
    with open(NETLIST_OUT, "w", encoding="utf-8") as f:
        f.write(netlist_json + "\n")

# ---- how closely the ladder can hit a voltage for different APM pull-ups (perfect closed loop)
def ladder_levels(rp, vs=5000.0, rs=10.0, rl=356_000.0, rds=5.0):
    g_bit = [1.0 / (b + rds) for b in BITS]
    out = [vs * rl / (rl + rp)]                              # open
    r = rs + 0.05                                            # short
    rr = r * rl / (r + rl)
    out.append(vs * rr / (rr + rp))
    G = [0.0] * (1 << NB)
    for m in range(1, 1 << NB):
        low = m & -m
        G[m] = G[m ^ low] + g_bit[low.bit_length() - 1]
        r = rs + 1.0 / G[m]
        rr = r * rl / (r + rl)
        out.append(vs * rr / (rr + rp))
    return sorted(out)


def worst_error(rp, lo=200, hi=4600, step=10):
    import bisect
    lv = ladder_levels(rp)
    worst = 0.0
    for t in range(lo, hi + 1, step):
        i = bisect.bisect_left(lv, t)
        e = min(abs(lv[j] - t) for j in (i - 1, i) if 0 <= j < len(lv))
        worst = max(worst, e)
    return worst


PULLUPS = [1000, 2200, 4700, 10000]
WORST = {rp: worst_error(rp) for rp in PULLUPS}


def idle_span(rp, vs=5000.0, rl=356_000.0):
    lo = IDLE_RS + POT_WIPER
    hi = IDLE_RS + POT_WIPER + POT_OHM
    def v(r):
        rr = r * rl / (r + rl)
        return vs * rr / (rr + rp)
    return v(lo), v(hi), vs * rl / (rl + rp)


IDLE_SPAN = {rp: idle_span(rp) for rp in PULLUPS}

# ---------------------------------------------------------------- SVG
S = []


def cls_attr(cls):
    return f' class="{cls}"' if cls else ""


def ln(x1, y1, x2, y2, cls=""):
    S.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}"{cls_attr(cls)}/>')


def pl(pts, cls=""):
    p = " ".join(f"{x},{y}" for x, y in pts)
    S.append(f'<polyline points="{p}" fill="none"{cls_attr(cls)}/>')


def rect(x, y, w, h, cls=""):
    S.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="none"{cls_attr(cls)}/>')


def txt(x, y, s, anchor="start", cls="", rot=None):
    t = f' transform="rotate({rot} {x} {y})"' if rot is not None else ""
    S.append(f'<text x="{x}" y="{y}" text-anchor="{anchor}"{cls_attr(cls)}{t}>{html.escape(s)}</text>')


def dot(x, y, cls=""):
    S.append(f'<circle cx="{x}" cy="{y}" r="2.6" fill="currentColor"{cls_attr(cls)}/>')


def gnd(x, y):
    ln(x, y, x, y + 8)
    ln(x - 8, y + 8, x + 8, y + 8)
    ln(x - 5, y + 11, x + 5, y + 11)
    ln(x - 2, y + 14, x + 2, y + 14)


def vcc(x, y, label="+3V3"):
    """Supply bar with the lead going down from (x, y)."""
    ln(x - 8, y, x + 8, y)
    txt(x, y - 5, label, anchor="middle", cls="tiny")


def nc(x, y):
    ln(x - 4, y - 4, x + 4, y + 4)
    ln(x - 4, y + 4, x + 4, y - 4)


def flag(x, y, name, side="right", cls=""):
    """Net label tag. side='right': the wire arrives from the left at (x, y)."""
    w = 8 + 6.6 * len(name)
    if side == "right":
        pts = f"{x},{y} {x + 8},{y - 7} {x + 8 + w},{y - 7} {x + 8 + w},{y + 7} {x + 8},{y + 7}"
        tx = x + 8 + w / 2
    else:
        pts = f"{x},{y} {x - 8},{y - 7} {x - 8 - w},{y - 7} {x - 8 - w},{y + 7} {x - 8},{y + 7}"
        tx = x - 8 - w / 2
    S.append(f'<polygon points="{pts}" fill="none"{cls_attr(cls)}/>')
    txt(tx, y + 4, name, anchor="middle", cls=f"net tiny {cls}".strip())


def res_v(x, y1, y2, ref=None, val=None, side="right", dashed=False):
    """Vertical resistor from y1 to y2 (body in the middle)."""
    mid = (y1 + y2) / 2
    ln(x, y1, x, mid - 15)
    rect(x - 5, mid - 15, 10, 30, "dashed" if dashed else "")
    ln(x, mid + 15, x, y2)
    tx = x + 10 if side == "right" else x - 10
    anchor = "start" if side == "right" else "end"
    if ref:
        txt(tx, mid - 2, ref, anchor=anchor, cls="ref tiny")
    if val:
        txt(tx, mid + 10, val, anchor=anchor, cls="tiny")


def res_h(x1, x2, y, ref=None, val=None):
    mid = (x1 + x2) / 2
    ln(x1, y, mid - 15, y)
    rect(mid - 15, y - 5, 30, 10)
    ln(mid + 15, y, x2, y)
    if ref:
        txt(mid, y - 9, ref, anchor="middle", cls="ref tiny")
    if val:
        txt(mid, y + 17, val, anchor="middle", cls="tiny")


def cap_v(x, y1, y2, ref, val, cls="", side="right"):
    mid = (y1 + y2) / 2
    ln(x, y1, x, mid - 3)
    ln(x - 9, mid - 3, x + 9, mid - 3, cls)
    ln(x - 9, mid + 3, x + 9, mid + 3, cls)
    ln(x, mid + 3, x, y2)
    tx, anchor = (x + 12, "start") if side == "right" else (x - 12, "end")
    txt(tx, mid - 2, ref, anchor=anchor, cls="ref tiny")
    txt(tx, mid + 10, val, anchor=anchor, cls="tiny")


def nmos_at(x, yc, xg, ytop, ybot, pulldown=True):
    """N-MOS with drain lead from (x, ytop), source lead to (x, ybot), gate at (xg, yc)."""
    xc = x - 8
    pl([(x, ytop), (x, yc - 12), (xc, yc - 12)])
    ln(xc, yc - 16, xc, yc + 16)
    ln(xc - 5, yc - 14, xc - 5, yc + 14)
    pl([(xc, yc + 12), (x, yc + 12), (x, ybot)])
    S.append(f'<polygon points="{xc + 1},{yc + 12} {xc + 7},{yc + 8.5} {xc + 7},{yc + 15.5}" fill="currentColor"/>')
    ln(xc - 5, yc, xg, yc)
    if pulldown:
        ln(xg, yc, xg, yc + 18)
        rect(xg - 4, yc + 18, 8, 24)
        ln(xg, yc + 42, xg, ybot)
        dot(xg, yc)


def tvs_v(x, y1, y2):
    mid = (y1 + y2) / 2
    ln(x, y1, x, mid - 16)
    S.append(f'<polygon points="{x - 8},{mid - 16} {x + 8},{mid - 16} {x},{mid}" fill="none"/>')
    S.append(f'<polygon points="{x - 8},{mid + 16} {x + 8},{mid + 16} {x},{mid}" fill="none"/>')
    pl([(x - 11, mid - 3), (x - 8, mid), (x + 8, mid), (x + 11, mid + 3)])
    ln(x, mid + 16, x, y2)


def ptc_h(x1, x2, y, ref):
    mid = (x1 + x2) / 2
    ln(x1, y, mid - 18, y)
    rect(mid - 18, y - 7, 36, 14)
    ln(mid - 14, y + 10, mid + 14, y - 10)
    ln(mid + 18, y, x2, y)
    txt(mid, y - 13, ref, anchor="middle", cls="ref tiny")


# --- U301 (partial)
rect(20, 100, 130, 250, "part")
txt(20, 92, "U301", cls="ref")
txt(85, 122, "STM32H743ZIT6", anchor="middle", cls="small muted")
for y, name, c in [(150, "PD12", ""), (180, "PD13", ""), (230, "PG2 · 87", "accent"), (320, "PC2_C · 28", "accent")]:
    txt(144, y + 4, name, anchor="end", cls=f"pin {c}".strip())
    ln(150, y, 175, y, c)
ln(175, 150, 255, 150)
ln(175, 180, 255, 180)
txt(185, 144, "EXP_SCL", cls="net")
txt(185, 174, "EXP_SDA", cls="net")
ln(175, 230, 255, 230, "accent")
txt(182, 224, "WS_RST_N", cls="net accent")
dot(215, 230, "accent")
ln(215, 230, 215, 246)
rect(209, 246, 12, 38)
ln(215, 284, 215, 296)
gnd(215, 296)
txt(226, 262, "R814", cls="ref small")
txt(226, 275, "10k", cls="small")
ln(175, 320, 188, 320, "accent")
flag(188, 320, "WS_ADC", cls="accent")
txt(20, 380, "I²C4 已在用：", cls="small muted")
txt(20, 396, "U111 TUSB320、J302", cls="small muted")
txt(20, 412, "上拉 R304/R305 4.7 kΩ 已有", cls="small muted")

# --- U801 TCA9539
rect(280, 110, 130, 330, "part")
txt(280, 102, "U801  TCA9539PWR", cls="ref")
txt(345, 432, "I²C 0x74", anchor="middle", cls="small muted")
for y, name, num in [(130, "VCC", 24), (150, "SCL", 22), (180, "SDA", 23), (230, "RESET", 3),
                     (330, "A0", 21), (350, "A1", 2), (380, "INT", 1), (420, "GND", 12)]:
    txt(286, y + 4, name, cls="pin")
    txt(276, y - 3, str(num), anchor="end", cls="pinno")
    ln(255, y, 280, y)
pl([(255, 130), (240, 130), (240, 116)])
ln(232, 116, 248, 116)
txt(240, 110, "+3V3", anchor="middle", cls="small")
txt(160, 128, "C801 100nF", cls="small muted")
pl([(255, 330), (255, 350)])
dot(255, 350)
ln(255, 350, 255, 356)
gnd(255, 356)
nc(255, 380)
gnd(255, 420)
RIGHT_LABELS = [f"G{i}" for i in range(12)] + ["SHORT", "G12", "CH_A0", "CH_A1"]
for i in range(16):
    y = 130 + i * 20
    name = f"P0{i}" if i < 8 else f"P1{i - 8}"
    txt(404, y + 4, name, anchor="end", cls="pin")
    txt(414, y - 3, str(4 + i if i < 8 else 5 + i), cls="pinno")
    new = i >= 13
    ln(410, y, 432, y, "accent2" if new else "")
    txt(436, y + 4, RIGHT_LABELS[i], cls="net accent2" if new else "net")

# --- ladder, 13 bits + short
BUS_Y, RAIL_Y = 150, 340
xs = [526 + i * 46 for i in range(NB)]
XS = 1222
ln(470, BUS_Y, 1300, BUS_Y, "bus")
txt(1238, 140, "WS_NODE", anchor="end", cls="net")
for i, x in enumerate(xs):
    new = i >= 12
    dot(x, BUS_Y)
    txt(x, 134, f"b{i}", anchor="middle", cls="small accent2" if new else "small muted")
    ln(x, BUS_Y, x, 170)
    rect(x - 6, 170, 12, 50, "accent2" if new else "")
    ref = BIT_REF.get(i, f"R{801 + i}")
    txt(x - 10, 195, ref, anchor="middle", cls="ref tiny", rot=-90)
    txt(x + 15, 195, fmt_ohm(BITS[i]), anchor="middle", cls="small", rot=-90)
    ln(x, 220, x, 262)
    xg = x - 24
    nmos_at(x, 280, xg, 262, RAIL_Y)
    txt(xg - 3, 275, f"G{i}", anchor="end", cls="net tiny")
    qref = BIT_Q.get(i, f"Q{801 + i // 2}{'AB'[i % 2]}")
    txt(x - 6, 360, qref, anchor="middle", cls="ref tiny")
dot(XS, BUS_Y)
ln(XS, BUS_Y, XS, 262)
nmos_at(XS, 280, XS - 24, 262, RAIL_Y)
txt(XS - 27, 275, "SHORT", anchor="end", cls="net tiny")
txt(XS - 6, 360, "Q807", anchor="middle", cls="ref tiny")
txt(XS - 6, 373, "AO3400A", anchor="middle", cls="tiny muted")
txt(XS + 8, 214, "0 Ω 直通", cls="tiny muted")
ln(480, RAIL_Y, 1240, RAIL_Y, "accent2")
dot(480, RAIL_Y, "accent2")
ln(480, RAIL_Y, 480, 372, "accent2")
flag(480, 372, "WS_RET", cls="accent2")
txt(500, 398, "Q801–Q806、Q808A、Q809 = BSS138DW，b0–b14 共 15 位：b0–b12 由 U801 控制，b13、b14 由 U808 控制；开关管源极接 WS_RET", cls="small muted")
txt(500, 414, "栅极下拉 100 kΩ 到 WS_RET：RN801–RN803（G0–G11）、R832（G12）、R847（G13）、R848（G14）、R813（SHORT）", cls="small muted")

# --- four channels on the right
NODE_X = 1300
ROWS = [150, 300, 450, 600]
J_X = 1800
ln(NODE_X, BUS_Y, NODE_X, ROWS[-1], "bus")
txt(NODE_X + 6, 236, "WS_NODE", cls="net tiny", rot=90)
rect(J_X, 128, 40, 516, "part")
txt(J_X + 20, 120, "J801", anchor="middle", cls="ref")
txt(J_X + 20, 662, "KF2EDGR-3.81-8P", anchor="middle", cls="tiny muted")
for k, y in enumerate(ROWS, start=1):
    if y != BUS_Y:
        dot(NODE_X, y)
    ln(NODE_X, y, 1340, y)
    # photorelay
    rect(1340, y - 26, 84, 52, "part")
    txt(1382, y - 31, f"U80{k + 1}  TLP176AM", anchor="middle", cls="ref tiny")
    txt(1336, y - 4, "4", anchor="end", cls="pinno")
    txt(1428, y - 4, "6", cls="pinno")
    # switch contacts inside
    ln(1340, y, 1368, y)
    ln(1368, y, 1396, y - 10)
    ln(1398, y, 1424, y)
    # LED inside + arrows
    S.append(f'<polygon points="{1352},{y + 8} {1366},{y + 8} {1359},{y + 18}" fill="none"/>')
    ln(1352, y + 18, 1366, y + 18)
    ln(1370, y + 12, 1378, y + 6)
    ln(1370, y + 17, 1378, y + 11)
    # LED pins: 1 anode, 3 cathode
    pl([(1359, y + 8), (1359, y + 2), (1350, y + 2)])  # cosmetic tie to anode lead
    ln(1350, y + 2, 1350, y + 26)
    ln(1359, y + 18, 1359, y + 22)
    pl([(1359, y + 22), (1396, y + 22), (1396, y + 26)])
    txt(1346, y + 36, "1", anchor="end", cls="pinno")
    txt(1400, y + 36, "3", cls="pinno")
    ln(1350, y + 26, 1350, y + 34)
    res_v(1350, y + 34, y + 80, f"R{819 + k}", "330", side="left")
    ln(1350, y + 80, 1350, y + 86)
    ln(1342, y + 86, 1358, y + 86)
    txt(1350, y + 98, "+3V3", anchor="middle", cls="tiny")
    ln(1396, y + 26, 1396, y + 56)
    flag(1396, y + 56, f"SEL{k}_N", cls="accent")
    # PTC and node; WS_KFk (relay side of the PTC) also feeds the idle-hold branch
    ln(1424, y, 1450, y)
    dot(1437, y, "accent2")
    ln(1437, y, 1437, y + 32, "accent2")
    flag(1437, y + 32, f"WS_KF{k}", cls="accent2")
    ptc_h(1450, 1530, y, f"F80{k}")
    dot(1560, y)
    ln(1530, y, 1560, y)
    tvs_v(1560, y, y + 70)
    gnd(1560, y + 70)
    txt(1572, y + 30, f"D80{k}", cls="ref tiny")
    txt(1572, y + 42, "SMF15CA", cls="tiny")
    # sense tap
    ln(1560, y, J_X, y)
    dot(1630, y)
    res_v(1630, y, y + 62, f"R{814 + k}", "300k 0.1%")
    ln(1630, y + 62, 1630, y + 70)
    flag(1630, y + 78, f"WS_S{k}", cls="accent")
    ln(1630, y + 70, 1630, y + 78)
    txt(1546, y - 10, f"WS_K{k}", anchor="end", cls="net tiny")
    # connector pins: odd = key line, even = WS_RET (the APM's switch return)
    txt(J_X + 6, y + 4, str(2 * k - 1), cls="pinno")
    ln(J_X - 26, y + 24, J_X, y + 24, "accent2")
    flag(J_X - 26, y + 24, "WS_RET", side="left", cls="accent2")
    txt(J_X + 6, y + 28, str(2 * k), cls="pinno")
    txt(J_X + 48, y + 4, f"K{k}", cls="ref small")
    txt(J_X + 48, y + 18, "至 APM 按键", cls="tiny muted")

# --- channel select block (bottom left)
txt(20, 486, "通道选择：同一时刻只接通一路（译码器保证）；PG2 为低时 4 路全部断开", cls="small")

# inverter Q808B: WS_RST_N high -> CH_EN_N low (enabled)
flag(110, 640, "WS_RST_N", side="left", cls="accent")
nmos_at(200, 640, 110, 580, 700, pulldown=False)
gnd(200, 700)
vcc(200, 512)
res_v(200, 512, 580, "R824", "10k")
dot(200, 580)
ln(200, 580, 250, 580)
flag(250, 580, "CH_EN_N")
txt(212, 668, "Q808B", cls="ref tiny")
txt(212, 680, "BSS138DW", cls="tiny muted")
txt(40, 730, "PG2 高 → CH_EN_N 低 → 使能", cls="tiny muted")

# address pull-downs
for y, net, ref in [(540, "CH_A0", "R825"), (660, "CH_A1", "R826")]:
    flag(360, y, net, side="left", cls="accent2")
    ln(360, y, 380, y)
    res_v(380, y, y + 80, ref, "100k")
    gnd(380, y + 80)

# decoder U806
DX0, DX1 = 560, 680
rect(DX0, 525, DX1 - DX0, 210, "part")
txt(DX0, 517, "U806  SN74LVC139APW", cls="ref")
for y, name, num in [(545, "1G", 1), (570, "1A", 2), (595, "1B", 3), (635, "2G", 15),
                     (655, "2A", 14), (675, "2B", 13), (700, "VCC", 16), (720, "GND", 8)]:
    txt(DX0 + 6, y + 4, name, cls="pin")
    txt(DX0 - 4, y - 3, str(num), anchor="end", cls="pinno")
for y, net in [(545, "CH_EN_N"), (570, "CH_A0"), (595, "CH_A1")]:
    ln(520, y, DX0, y)
    flag(520, y, net, side="left", cls="accent2" if net.startswith("CH_A") else "")
ln(540, 635, DX0, 635)
ln(540, 635, 540, 625)
vcc(540, 625)
ln(530, 655, DX0, 655)
ln(530, 675, DX0, 675)
ln(530, 655, 530, 675)
dot(530, 675)
gnd(530, 675)
ln(520, 700, DX0, 700)
txt(516, 704, "+3V3 · C803", anchor="end", cls="tiny muted")
ln(540, 720, DX0, 720)
gnd(540, 720)
for y, name, num, net in [(545, "1Y0", 4, "SEL1_N"), (570, "1Y1", 5, "SEL2_N"),
                          (595, "1Y2", 6, "SEL3_N"), (620, "1Y3", 7, "SEL4_N")]:
    txt(DX1 - 6, y + 4, name, anchor="end", cls="pin")
    txt(DX1 + 4, y - 3, str(num), cls="pinno")
    ln(DX1, y, DX1 + 20, y)
    flag(DX1 + 20, y, net, cls="accent")
for i, (y, num) in enumerate([(655, 12), (675, 11), (695, 10), (715, 9)]):
    txt(DX1 - 6, y + 4, f"2Y{i}", anchor="end", cls="pin")
    txt(DX1 + 4, y - 3, str(num), cls="pinno")
    ln(DX1, y, DX1 + 14, y)
    nc(DX1 + 14, y)

# mux U807
MX0, MX1 = 900, 1030
rect(MX0, 525, MX1 - MX0, 210, "part")
txt(MX0, 517, "U807  CD74HC4052PW", cls="ref")
for y, name, num, net in [(545, "A0", 12, "WS_S1"), (565, "A1", 14, "WS_S2"), (585, "A2", 15, "WS_S3"),
                          (605, "A3", 11, "WS_S4"), (640, "E", 6, "CH_EN_N"), (660, "S0", 10, "CH_A0"),
                          (680, "S1", 9, "CH_A1")]:
    txt(MX0 + 6, y + 4, name, cls="pin")
    txt(MX0 - 4, y - 3, str(num), anchor="end", cls="pinno")
    ln(MX0 - 16, y, MX0, y)
    c = "accent" if net.startswith("WS_S") else ("accent2" if net.startswith("CH_A") else "")
    flag(MX0 - 16, y, net, side="left", cls=c)
for y, name, num in [(545, "COM A", 13), (590, "VCC", 16), (620, "VEE", 7), (640, "GND", 8)]:
    txt(MX1 - 6, y + 4, name, anchor="end", cls="pin")
    txt(MX1 + 4, y - 3, str(num), cls="pinno")
    ln(MX1, y, MX1 + 20, y)
txt(MX1 - 6, 684, "B0–B3、COM B", anchor="end", cls="pin")
txt(MX1 + 4, 677, "1–5", cls="pinno")
ln(MX1, 680, MX1 + 20, 680)
pl([(MX1 + 20, 620), (MX1 + 20, 680)])
dot(MX1 + 20, 640)
dot(MX1 + 20, 680)
gnd(MX1 + 20, 680)
txt(MX1 + 24, 594, "+3V3 · C804", cls="tiny muted")
# WS_ADC: divider bottom + filter
ln(MX1 + 20, 545, 1235, 545)
dot(1145, 545)
res_v(1145, 545, 640, "R819", "56k 0.1%")
gnd(1145, 640)
dot(1215, 545)
cap_v(1215, 545, 610, "C802", "1nF", cls="accent2")
gnd(1215, 610)
flag(1235, 545, "WS_ADC", cls="accent")
txt(900, 762, "WS_Sk 经 R815–R818（300 kΩ）来自各路接口；选中一路时 WS_ADC = 接口电压 ÷ 6.36（第 3 版 R819 改 56 kΩ，量程约 21 V）", cls="tiny muted")
txt(900, 776, "C802 第 3 版改 1 nF：时间常数 47 kΩ × 1 nF ≈ 47 µs，能跟上脉冲供电的 APM 上拉", cls="tiny muted")


# ---------------------------------------------------------------- revision 3: idle hold
def ic_box(x, y, w, ref, part, left, right, pitch=20, note=None):
    """Rectangle with pin stubs; left/right rows are (name, number, net, cls). Nets become labels."""
    n = max(len(left), len(right))
    h = 14 + pitch * n
    rect(x, y, w, h, "part")
    txt(x, y - 8, f"{ref}  {part}", cls="ref")
    if note:
        txt(x + w / 2, y + h + 16, note, anchor="middle", cls="tiny muted")
    pos = {}
    for i, (name, num, net, cls) in enumerate(left):
        if name is None:
            continue
        py = y + 17 + i * pitch
        txt(x + 6, py + 4, name, cls="pin")
        txt(x - 4, py - 3, str(num), anchor="end", cls="pinno")
        ln(x - 20, py, x, py, cls)
        pos[num] = (x - 20, py)
        if net:
            flag(x - 20, py, net, side="left", cls=cls)
        elif net is None:
            nc(x - 20, py)
    for i, (name, num, net, cls) in enumerate(right):
        if name is None:
            continue
        py = y + 17 + i * pitch
        txt(x + w - 6, py + 4, name, anchor="end", cls="pin")
        txt(x + w + 4, py - 3, str(num), cls="pinno")
        ln(x + w, py, x + w + 20, py, cls)
        pos[num] = (x + w + 20, py)
        if net:
            flag(x + w + 20, py, net, cls=cls)
        elif net is None:
            nc(x + w + 20, py)
    return pos


def ind_h(x1, x2, y, ref, val):
    mid = (x1 + x2) / 2
    ln(x1, y, mid - 20, y)
    for i in range(4):
        cx = mid - 15 + i * 10
        S.append(f'<path d="M {cx - 5} {y} A 5 5 0 0 1 {cx + 5} {y}" fill="none"/>')
    ln(mid + 20, y, x2, y)
    txt(mid, y - 12, ref, anchor="middle", cls="ref tiny")
    txt(mid, y + 16, val, anchor="middle", cls="tiny")


def diode_h(x1, x2, y, ref, val):
    """Anode on the left, cathode on the right."""
    mid = (x1 + x2) / 2
    ln(x1, y, mid - 8, y)
    S.append(f'<polygon points="{mid - 8},{y - 8} {mid - 8},{y + 8} {mid + 6},{y}" fill="none"/>')
    ln(mid + 6, y - 8, mid + 6, y + 8)
    ln(mid + 6, y, x2, y)
    txt(mid, y - 12, ref, anchor="middle", cls="ref tiny")
    txt(mid, y + 20, val, anchor="middle", cls="tiny")


def relay_box(x, y, ref):
    """TLP176AM drawn left to right: pin 6 at (x, y), pin 4 at (x + 84, y); LED pins 1/3 at the bottom."""
    rect(x, y - 26, 84, 52, "part")
    txt(x + 42, y - 31, f"{ref}  TLP176AM", anchor="middle", cls="ref tiny")
    txt(x - 4, y - 4, "6", anchor="end", cls="pinno")
    txt(x + 88, y - 4, "4", cls="pinno")
    ln(x, y, x + 28, y)
    ln(x + 28, y, x + 56, y - 10)
    ln(x + 58, y, x + 84, y)
    S.append(f'<polygon points="{x + 12},{y + 8} {x + 26},{y + 8} {x + 19},{y + 18}" fill="none"/>')
    ln(x + 12, y + 18, x + 26, y + 18)
    ln(x + 30, y + 12, x + 38, y + 6)
    ln(x + 30, y + 17, x + 38, y + 11)
    pl([(x + 19, y + 8), (x + 19, y + 2), (x + 10, y + 2), (x + 10, y + 26)])
    pl([(x + 19, y + 18), (x + 19, y + 22), (x + 56, y + 22), (x + 56, y + 26)])
    txt(x + 6, y + 36, "1", anchor="end", cls="pinno")
    txt(x + 60, y + 36, "3", cls="pinno")
    return (x + 10, y + 26), (x + 56, y + 26)


Y3 = 830
ln(20, Y3 - 22, 1920, Y3 - 22, "dashed")
txt(20, Y3, "第 3 版新增 · 空闲保持：每路一个光耦继电器 + 2.2 kΩ + AD5292 50 kΩ 数字电位器（W–B 变阻器接法），"
    "4 路可以同时各自保持学到的空闲电平；按下时电阻阵并在空闲支路上，从空闲直接跳到按下，中间不断开", cls="small")

# U808 expander
U8_L = [("VCC", 24, "+3V3", ""), ("SCL", 22, "EXP_SCL", ""), ("SDA", 23, "EXP_SDA", ""),
        ("RESET", 3, "WS_RST_N", "accent"), ("A0", 21, "+3V3", ""), ("A1", 2, "GND", ""),
        ("INT", 1, None, ""), ("GND", 12, "GND", "")]
U8_R = []
for i in range(16):
    port = f"P0{i}" if i < 8 else f"P1{i - 8}"
    num = 4 + i if i < 8 else 5 + i
    net = U808_P0[i] if i < 8 else U808_P1[i - 8]
    U8_R.append((port, num, net.replace("WS_G", "G") if net else net, "accent2" if net else ""))
ic_box(100, 870, 120, "U808", "TCA9539PWR", U8_L, U8_R, note="I²C 0x75（A0 = +3V3）· C808 100nF")

# boost U817: +5V -> WS_VPOT
BX, BY = 470, 880
flag(BX - 40, BY, "+5V", side="left")
ln(BX - 40, BY, BX + 20, BY)
dot(BX - 20, BY)
cap_v(BX - 20, BY, BY + 60, "C806", "4.7µF", side="left")
gnd(BX - 20, BY + 60)
ind_h(BX + 20, BX + 120, BY, "L801", "10µH")
dot(BX + 46, BY)
dot(BX + 120, BY)
diode_h(BX + 120, BX + 200, BY, "D805", "B5819W")
ln(BX + 200, BY, BX + 380, BY, "bus")
flag(BX + 380, BY, "WS_VPOT", cls="accent2")
txt(BX + 388, BY - 12, f"≈ {VPOT:.1f} V", cls="tiny muted")
dot(BX + 260, BY)
res_v(BX + 260, BY, BY + 100, "R841", "1M", side="left")
pl([(BX + 260, BY + 15), (BX + 290, BY + 15), (BX + 290, BY + 47)])
dot(BX + 260, BY + 15)
ln(BX + 281, BY + 47, BX + 299, BY + 47)
ln(BX + 281, BY + 53, BX + 299, BY + 53)
pl([(BX + 290, BY + 53), (BX + 290, BY + 85), (BX + 260, BY + 85)])
dot(BX + 260, BY + 85)
txt(BX + 302, BY + 48, "C805", cls="ref tiny")
txt(BX + 302, BY + 60, "10pF", cls="tiny")
dot(BX + 260, BY + 100)
res_v(BX + 260, BY + 100, BY + 170, "R842", "49.9k", side="left")
gnd(BX + 260, BY + 170)
dot(BX + 340, BY)
cap_v(BX + 340, BY, BY + 60, "C807", "4.7µF 50V")
gnd(BX + 340, BY + 60)
UX, UY = BX + 60, BY + 40
ln(BX + 120, BY, BX + 120, UY)                                        # SW on the top edge
rect(UX, UY, 90, 90, "part")
txt(UX + 45, UY + 104, "U817  TPS61040DBVR", anchor="middle", cls="tiny")
txt(BX + 120, UY + 14, "SW", anchor="middle", cls="pin")
txt(BX + 124, UY - 3, "1", cls="pinno")
txt(UX + 6, UY + 34, "VIN", cls="pin")
txt(UX - 4, UY + 27, "5", anchor="end", cls="pinno")
pl([(UX, UY + 30), (BX + 46, UY + 30), (BX + 46, BY)])
txt(UX + 6, UY + 58, "EN", cls="pin")
txt(UX - 4, UY + 51, "4", anchor="end", cls="pinno")
ln(UX - 50, UY + 54, UX, UY + 54)
flag(UX - 50, UY + 54, "VPOT_EN", side="left", cls="accent2")
dot(UX - 30, UY + 54)
res_v(UX - 30, UY + 54, UY + 124, "R843", "100k", side="left")
gnd(UX - 30, UY + 124)
txt(UX + 84, UY + 64, "FB", anchor="end", cls="pin")
txt(UX + 94, UY + 57, "3", cls="pinno")
ln(UX + 90, UY + 60, BX + 260, UY + 60)                              # FB -> R841/R842 junction
txt(UX + 45, UY + 84, "GND · 2", anchor="middle", cls="pinno")
ln(UX + 45, UY + 90, UX + 45, UY + 98)
gnd(UX + 45, UY + 98)
for i, line in enumerate([f"WS_VPOT = 1.233 V × (1 + R841 / R842) ≈ {VPOT:.1f} V，",
                          "只供 4 个 AD5292 的 VDD，电流不到 1 mA。",
                          "VPOT_EN 由 U808 P06 控制；U808 复位时 R843 拉低，",
                          "升压关断，输出约 +5 V 减一个二极管压降。"]):
    txt(BX - 60, BY + 214 + i * 14, line, cls="tiny muted")

# WS_RET single tie
TX, TY = 470, 1190
flag(TX, TY, "WS_RET", side="left", cls="accent2")
res_h(TX, TX + 90, TY, "R844", "0Ω")
ln(TX + 90, TY, TX + 110, TY)
gnd(TX + 110, TY)
for i, line in enumerate(["WS_RET 与 GND 只在 R844 一点相连，R844 放在 J801 旁。",
                          "电阻阵、直通管和电位器 B 端都回到 WS_RET，",
                          "线束上把 J801 偶数脚接到原车开关的回线。"]):
    txt(TX - 60, TY + 34 + i * 14, line, cls="tiny muted")

# four idle-hold blocks, 2 x 2
for k in range(1, CH + 1):
    x0 = 980 + ((k - 1) % 2) * 490
    y0 = 880 + ((k - 1) // 2) * 240
    # WS_KFk -> relay -> R827..R830 -> AD5292 W
    ln(x0, y0, x0 + 20, y0, "accent2")
    flag(x0, y0, f"WS_KF{k}", side="left", cls="accent2")
    an, ca = relay_box(x0 + 20, y0, f"U{812 + k}")
    ln(an[0], an[1], an[0], an[1] + 8)
    res_v(an[0], an[1] + 8, an[1] + 58, f"R{832 + k}", "330", side="left")
    ln(an[0] - 8, an[1] + 58, an[0] + 8, an[1] + 58)
    txt(an[0], an[1] + 70, "+3V3", anchor="middle", cls="tiny")
    ln(ca[0], ca[1], ca[0], ca[1] + 22)
    flag(ca[0], ca[1] + 22, f"IDLE{k}_N", cls="accent2")
    res_h(x0 + 104, x0 + 164, y0, f"R{826 + k}", "2.2k")
    PX = x0 + 184
    left = [("W", 4, "", ""), ("A", 3, None, ""), ("B", 5, "WS_RET", "accent2"), (None, 0, None, ""),
            ("EXT_CAP", 7, "", "")]
    right = [("VDD", 6, "WS_VPOT", "accent2"), ("VSS", 2, "GND", ""), ("VLOGIC", 8, "+3V3", ""),
             ("GND", 9, "GND", ""), ("RESET", 1, "WS_RST_N", "accent"),
             ("SYNC", 12, f"POT_SYNC{k}_N", "accent2"), ("SCLK", 11, "POT_SCLK", "accent2"),
             ("DIN", 10, "POT_DIN", "accent2"), ("SDO", 13, None, ""), ("RDY", 14, None, "")]
    pos = ic_box(PX, y0 - 17, 130, f"U{808 + k}", "AD5292BRUZ-50", left, right, pitch=16)
    ln(x0 + 164, y0, pos[4][0], pos[4][1])
    # EXT_CAP capacitor, hanging below the left pin
    ex, ey = pos[7]
    cap_v(ex, ey, ey + 40, f"C{808 + k}", "1µF", side="left")
    gnd(ex, ey + 40)
    txt(PX + 65, y0 + 190, f"VDD：C{812 + k} 100nF 50V · VLOGIC：C{816 + k} 100nF · SYNC 上拉 R{836 + k} 10k",
        anchor="middle", cls="tiny muted")

VB_W, VB_H = 1940, 1350
svg = (f'<svg viewBox="0 0 {VB_W} {VB_H}" role="img" aria-label="第 12 页第 3 版原理图：U801 驱动 13 位电阻阵和 0 Ω 直通，'
       '公共节点 WS_NODE 经 4 个 TLP176AM 光耦继电器分别接到 J801 的 K1–K4，每路有 PTC 和 TVS；'
       'SN74LVC139A 译码保证只接通一路，CD74HC4052 把所选通道的 1:6.36 分压接到 PC2_C。'
       '第 3 版新增：每路一个空闲保持支路（光耦继电器、2.2 kΩ、AD5292 数字电位器），U808 控制，U817 升压供电；'
       '电阻阵和接口回线改为 WS_RET，经 R844 单点接地。">'
       '<g stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">'
       + "".join(S) + "</g></svg>")

if PREVIEW:
    pv = svg.replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" ', 1)
    pv = pv.replace('stroke="currentColor"', 'stroke="#111"').replace('fill="currentColor"', 'fill="#111"')
    style = ('<style>text{font-family:Menlo,monospace;font-size:12px;fill:#111;stroke:none}'
             '.small{font-size:11px}.tiny{font-size:10px}.pinno{font-size:9px;fill:#777}.muted{fill:#666}'
             '.accent{stroke:#0b7a6e}text.accent{fill:#0b7a6e;stroke:none}.accent2{stroke:#b4530a}text.accent2{fill:#b4530a;stroke:none}'
             '.bus{stroke-width:2.2}.dashed{stroke-dasharray:3 3}</style>')
    pv = pv.replace('<g ', f'<rect width="{VB_W}" height="{VB_H}" fill="#fff"/>{style}<g ', 1)
    with open(PREVIEW, "w", encoding="utf-8") as f:
        f.write(pv)

# ---------------------------------------------------------------- tables
ladder_rows = []
for i, r in enumerate(BITS):
    port = f"P0{i}" if i < 8 else (f"P1{i - 8}" if i < 12 else "P15")
    pin = 4 + i if i < 8 else (5 + i if i < 12 else 18)
    ref = BIT_REF.get(i, f"R{801 + i}")
    q = BIT_Q.get(i, f"Q{801 + i // 2}{'AB'[i % 2]}")
    if i == 13:
        port, pin = "U808 P07", 11
    elif i == 14:
        port, pin = "U808 P14", 17
    src = "第 3 版新增" if i >= 13 else ("第 2 版新增" if i == 12 else ("第 1 版新增" if i >= 8 else "apm.md 原值"))
    tr_cls = ' class="added"' if i >= 13 else ''
    ladder_rows.append(
        f"<tr{tr_cls}><td>b{i}</td><td>{ref}</td><td class=num>{fmt_ohm(r)} Ω</td>"
        f"<td class=num>{1e6 / r:,.1f}</td><td>{q}</td><td>{port if port.startswith('U8') else 'U801 ' + port} · {pin}</td><td>{src}</td></tr>")
ladder_rows.append("<tr><td>SHORT</td><td>—</td><td class=num>0 Ω</td><td class=num>—</td><td>Q807</td><td>U801 P14 · 17</td><td>AO3400A 直通</td></tr>")

bom = [
    ("U801", "TCA9539PWR", "TI", "TSSOP-24", 1, "16 位 I²C IO 扩展；P15–P17 本版用上"),
    ("Q801–Q806、Q808、Q809", "BSS138DW", "Diodes", "SOT-363", 8, "双 N-MOS：15 路电阻开关 + 1 个使能反相"),
    ("Q807", "AO3400A", "AOS", "SOT-23", 1, "0 Ω 直通"),
    ("R801–R812、R831、R845、R846", "47 Ω … 360 kΩ，1%", "", "0603", 15, "电阻阵，取值见下表"),
    ("RN801–RN803", "100 kΩ ×4 排阻", "", "4×0603", 3, "G0–G11 栅极下拉"),
    ("R813、R832、R847、R848", "100 kΩ 1%", "", "0603", 4, "SHORT、G12、G13、G14 栅极下拉"),
    ("R814", "10 kΩ 1%", "", "0603", 1, "WS_RST_N 下拉"),
    ("U802–U805", "TLP176AM", "Toshiba", "4-pin SO6", 4, "光耦继电器，60 V，导通 ≤ 2 Ω，每路一个"),
    ("R820–R823", "330 Ω 1%", "", "0603", 4, "继电器 LED 限流，约 5.5–6 mA"),
    ("U806", "SN74LVC139APWR", "TI", "TSSOP-16", 1, "2-4 译码，输出低有效，只用第 1 组"),
    ("U807", "CD74HC4052PWR", "TI", "TSSOP-16", 1, "双 4 选 1 模拟开关，只用 A 段做回读"),
    ("R824", "10 kΩ 1%", "", "0603", 1, "CH_EN_N 上拉：复位时 4 路全断"),
    ("R825、R826", "100 kΩ 1%", "", "0603", 2, "CH_A0、CH_A1 下拉，复位时输入不悬空"),
    ("R815–R818", "300 kΩ 0.1%", "", "0603", 4, "每路回读分压上臂，取在接口侧"),
    ("R819", "56 kΩ 0.1%", "", "0603", 1, "回读分压下臂（4 路共用）；第 3 版由 100 kΩ 改小，量程 13.2 V → 21 V"),
    ("C801、C803、C804", "100 nF X7R", "", "0603", 3, "U801、U806、U807 去耦"),
    ("C802", "1 nF C0G", "", "0603", 1, "回读滤波，靠近 U301.28；第 3 版由 10 nF 改小"),
    ("F801–F804", "PTC 自恢复保险丝", "", "0805/1206", 4, "保持电流 50 mA，耐压 ≥ 15 V，每路一个"),
    ("D801–D804", "SMF15CA", "", "SOD-123FL", 4, "15 V 双向 TVS，每路一个"),
    ("J801", "KF2EDGR-3.81-8P", "", "3.81 mm", 1, "奇数脚 K1–K4，偶数脚 WS_RET"),
    ("R844", "0 Ω", "", "0603", 1, "WS_RET 与 GND 的唯一连接点，放在 J801 旁"),
    ("U808", "TCA9539PWR", "TI", "TSSOP-24", 1, "第二个 IO 扩展 @0x75：电位器 SPI、空闲继电器、升压使能"),
    ("C808", "100 nF X7R", "", "0603", 1, "U808 去耦"),
    ("U809–U812", "AD5292BRUZ-50", "ADI", "TSSOP-14", 4, "50 kΩ、1024 档数字电位器，W–B 变阻器接法，每路一个"),
    ("U813–U816", "TLP176AM", "Toshiba", "4-pin SO6", 4, "空闲支路的光耦继电器，每路一个"),
    ("R827–R830", "2.2 kΩ 1%", "", "0603", 4, "空闲支路串联电阻，限制电位器电流；第 2 版此位号为空闲电阻预留位"),
    ("R833–R836", "330 Ω 1%", "", "0603", 4, "空闲继电器 LED 限流"),
    ("R837–R840", "10 kΩ", "", "0603", 4, "POT_SYNC 上拉：U808 复位时 SYNC 保持高"),
    ("C809–C812", "1 µF 16 V X7R", "", "0603", 4, "AD5292 EXT_CAP（要求 ≥ 7 V）"),
    ("C813–C816", "100 nF 50 V X7R", "", "0603", 4, "AD5292 VDD 去耦"),
    ("C817–C820", "100 nF X7R", "", "0603", 4, "AD5292 VLOGIC 去耦"),
    ("U817", "TPS61040DBVR", "TI", "SOT-23-5", 1, "升压：+5V → WS_VPOT，约 25.9 V，只供 4 个 AD5292"),
    ("L801", "10 µH", "", "3×3 mm", 1, "饱和电流 ≥ 450 mA"),
    ("D805", "B5819W", "", "SOD-123", 1, "40 V 肖特基"),
    ("R841", "1 MΩ 1%", "", "0603", 1, "升压反馈上臂"),
    ("R842", "49.9 kΩ 1%", "", "0603", 1, "升压反馈下臂"),
    ("C805", "10 pF C0G", "", "0603", 1, "前馈电容，跨在 R841 上"),
    ("R843", "100 kΩ", "", "0603", 1, "VPOT_EN 下拉：U808 复位时升压关断"),
    ("C806", "4.7 µF 10 V X5R", "", "0603", 1, "升压输入"),
    ("C807", "4.7 µF 50 V X7R", "", "1206", 1, "WS_VPOT 输出"),
]
bom_rows = "".join(
    f"<tr><td>{a}</td><td>{b}</td><td>{c}</td><td>{d}</td><td class=num>{e}</td><td>{f}</td></tr>" for a, b, c, d, e, f in bom)
assert sum(r[4] for r in bom) == n_parts, (sum(r[4] for r in bom), n_parts)

u808_rows = []
U808_USE = {"POT_SCLK": "电位器 SPI 时钟（位操作）", "POT_DIN": "电位器 SPI 数据", "VPOT_EN": "U817 升压使能，高有效",
            "WS_G13": "电阻阵 b13（47 Ω）开关管 Q809A 栅极", "WS_G14": "电阻阵 b14（150 kΩ）开关管 Q809B 栅极"}
for i in range(16):
    port = f"P0{i}" if i < 8 else f"P1{i - 8}"
    pin = 4 + i if i < 8 else 5 + i
    net = U808_P0[i] if i < 8 else U808_P1[i - 8]
    if net is None:
        use = "空着"
    elif net.startswith("POT_SYNC"):
        use = f"U{808 + int(net[8])} 的 SYNC，低有效"
    elif net.startswith("IDLE"):
        use = f"K{net[4]} 空闲继电器 U{812 + int(net[4])}，低 = 接通"
    else:
        use = U808_USE[net]
    u808_rows.append(f"<tr><td>{port} · {pin}</td><td>{net or '—'}</td><td>{use}</td></tr>")

span_rows = []
for rp in PULLUPS:
    lo, hi, vo = IDLE_SPAN[rp]
    span_rows.append(f"<tr><td>{fmt_ohm(rp)}Ω</td><td class=num>{lo / 1000:.2f}–{hi / 1000:.2f} V</td>"
                     f"<td class=num>{vo / 1000:.2f} V</td><td class=num>{WORST[rp]:.1f} mV</td></tr>")
worst_lo, worst_hi = min(WORST.values()), max(WORST.values())

tca_rows = []
for i in range(16):
    port = f"P0{i}" if i < 8 else f"P1{i - 8}"
    pin = 4 + i if i < 8 else 5 + i
    if i < 12:
        use, note = f"b{i}（{fmt_ohm(BITS[i])} Ω）", "电阻阵"
    elif i == 12:
        use, note = "SHORT", "0 Ω 直通"
    elif i == 13:
        use, note = "b12（100 Ω）", "本版新增"
    elif i == 14:
        use, note = "CH_A0", "通道地址低位，本版新增"
    else:
        use, note = "CH_A1", "通道地址高位，本版新增"
    tr = ' class="added"' if i >= 13 else ''
    tca_rows.append(f"<tr{tr}><td>{port} · {pin}</td><td>{use}</td><td>{note}</td></tr>")

PAGE = f"""<title>Hardware-C 车窗按键模拟</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">
<style>
/* Layout: one reading column; the schematic breaks out full width with its own horizontal scroll */
:root {{
  --bg: #f5f7f8; --panel: #ffffff; --fg: #15202b; --muted: #5b6875; --line: #d5dce2;
  --accent: #0b7a6e; --accent-soft: #e2f3f0; --accent2: #b4530a; --warn: #9a5b00;
  --sans: "IBM Plex Sans", "PingFang SC", "Noto Sans SC", "Microsoft YaHei", system-ui, sans-serif;
  --mono: "IBM Plex Mono", "SF Mono", Menlo, Consolas, monospace;
}}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
  --bg: #0f1418; --panel: #161d23; --fg: #dbe3ea; --muted: #8d9aa6; --line: #2a343d;
  --accent: #3cc9b4; --accent-soft: #12302c; --accent2: #f0a050; --warn: #e0a548; color-scheme: dark; }} }}
:root[data-theme="dark"] {{
  --bg: #0f1418; --panel: #161d23; --fg: #dbe3ea; --muted: #8d9aa6; --line: #2a343d;
  --accent: #3cc9b4; --accent-soft: #12302c; --accent2: #f0a050; --warn: #e0a548; color-scheme: dark; }}
body {{ background: var(--bg); color: var(--fg); font-family: var(--sans); font-size: 15px; line-height: 1.6;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); padding-block: 32px 64px; }}
main {{ max-width: 1120px; margin: 0 auto; display: flex; flex-direction: column; gap: 40px; }}
.prose {{ max-width: 72ch; }}
h1, h2, h3 {{ text-wrap: balance; line-height: 1.25; margin: 0; font-weight: 600; }}
h1 {{ font-size: 1.9rem; }}
h2 {{ font-size: 1.25rem; }}
h3 {{ font-size: 1rem; }}
p {{ margin: 0; }}
section {{ display: flex; flex-direction: column; gap: 14px; min-width: 0; }}
.eyebrow {{ font-family: var(--mono); font-size: .78rem; letter-spacing: .06em; color: var(--muted); text-transform: uppercase; }}
.lede {{ color: var(--muted); }}
.facts {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 12px; }}
.fact {{ border-top: 2px solid var(--line); padding-top: 8px; }}
.fact b {{ display: block; font-family: var(--mono); font-weight: 500; font-size: 1.15rem; font-variant-numeric: tabular-nums; }}
.fact.new {{ border-color: var(--accent); }}
.fact span {{ color: var(--muted); font-size: .88rem; }}
figure {{ margin: 0; background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 12px; }}
.scroll {{ overflow-x: auto; }}
figure svg {{ display: block; width: 100%; min-width: 1400px; height: auto; color: var(--fg); }}
figure svg text {{ font-family: var(--mono); font-size: 12px; fill: currentColor; stroke: none; }}
figure svg .small {{ font-size: 11px; }}
figure svg .tiny {{ font-size: 10px; }}
figure svg .ref {{ font-weight: 500; }}
figure svg .pin {{ font-size: 11px; }}
figure svg .pinno {{ font-size: 9px; fill: var(--muted); }}
figure svg .muted {{ fill: var(--muted); }}
figure svg .net {{ font-size: 11px; }}
figure svg .accent {{ color: var(--accent); stroke: var(--accent); }}
figure svg .accent2 {{ color: var(--accent2); stroke: var(--accent2); }}
figure svg text.accent {{ fill: var(--accent); stroke: none; }}
figure svg text.accent2 {{ fill: var(--accent2); stroke: none; }}
figure svg .bus {{ stroke-width: 2.2; }}
figure svg .dashed {{ stroke-dasharray: 3 3; }}
figcaption {{ color: var(--muted); font-size: .9rem; padding: 10px 4px 2px; max-width: 86ch; }}
.key {{ display: inline-block; width: 18px; height: 3px; vertical-align: middle; margin: 0 4px 0 10px; }}
.table {{ overflow-x: auto; border: 1px solid var(--line); border-radius: 6px; background: var(--panel); }}
table {{ border-collapse: collapse; width: 100%; font-size: .9rem; }}
th, td {{ text-align: left; padding: 7px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }}
th {{ font-weight: 500; color: var(--muted); font-size: .8rem; letter-spacing: .03em; white-space: nowrap; }}
tr:last-child td {{ border-bottom: 0; }}
td:first-child, td.num {{ font-family: var(--mono); white-space: nowrap; }}
td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
tr.added td {{ background: var(--accent-soft); }}
.pill {{ font-family: var(--mono); font-size: .75rem; padding: 1px 7px; border-radius: 999px; border: 1px solid var(--accent); color: var(--accent); white-space: nowrap; }}
.pill.keep {{ border-color: var(--line); color: var(--muted); }}
.grid2 {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 24px; }}
.grid2 > * {{ min-width: 0; }}
ol, ul {{ margin: 0; padding-left: 1.3em; display: flex; flex-direction: column; gap: 8px; }}
code {{ font-family: var(--mono); font-size: .88em; }}
pre {{ font-family: var(--mono); font-size: .8rem; line-height: 1.5; background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 12px 14px; overflow-x: auto; margin: 0; }}
details {{ border: 1px solid var(--line); border-radius: 6px; background: var(--panel); }}
details > summary {{ cursor: pointer; padding: 10px 14px; font-weight: 500; }}
details[open] > summary {{ border-bottom: 1px solid var(--line); }}
details pre {{ border: 0; border-radius: 0; max-height: 420px; }}
.row {{ display: flex; gap: 10px; align-items: center; padding: 10px 14px 0; flex-wrap: wrap; }}
button {{ font: inherit; font-size: .85rem; padding: 4px 12px; border-radius: 5px; border: 1px solid var(--line); background: var(--bg); color: var(--fg); cursor: pointer; }}
button:focus-visible, summary:focus-visible {{ outline: 2px solid var(--accent); outline-offset: 2px; }}
.note {{ color: var(--muted); font-size: .88rem; }}
.warn {{ color: var(--warn); }}
</style>

<main>
<header class="prose" style="display:flex;flex-direction:column;gap:12px">
  <div class="eyebrow">Hardware-C · C/062 之后 · 原理图第 12 页 · 第 3 版</div>
  <h1>APM 车窗按键模拟</h1>
  <p class="lede">思路只有一条：APM 只认它引脚上的电压。学习时记下空闲电压和各档按下时的电压，发出时用电阻把引脚拉到同样的电压。第 3 版在第 2 版（4 路共用一套 13 位电阻阵，同一时刻只按一路）的基础上补了三处，让这条思路不依赖具体车型：每路一个空闲保持支路，4 路可以同时各自保持学到的空闲电平；电阻阵和接口回线改到 WS_RET 单点接地；回读滤波改快，能测脉冲供电的上拉。主控引脚仍只用 PG2 和 PC2_C。</p>
</header>

<div class="facts">
  <div class="fact new"><b>4 路同时保持空闲</b><span>每路 AD5292 50 kΩ 串 2.2 kΩ，空闲电阻 2.3–52 kΩ 可调，也可以开路</span></div>
  <div class="fact new"><b>{worst_lo:.1f}–{worst_hi:.1f} mV</b><span>APM 上拉 1–10 kΩ（5 V）时，200–4600 mV 任一目标电压电阻阵能达到的最小误差；固件闭环微调后的上限</span></div>
  <div class="fact"><b>{n_parts} 个器件</b><span>第 2 版是 63 个（含 4 个不贴的空闲电阻位，本版取消）；{n_pins} 个引脚，{len(new_nets)} 个新网络</span></div>
  <div class="fact"><b>2 个主控引脚</b><span>仍是 PG2、PC2_C；新加的 U808 挂在同一条 I²C4 上，地址 0x75</span></div>
</div>

<figure>
  <div class="scroll">{svg}</div>
  <figcaption>第 12 页第 3 版的全部连接。上半部分和第 2 版相同：左边是电阻阵，右边是 4 路接口，每路依次是 J801、TVS、PTC、光耦继电器，接到公共节点 WS_NODE；中间是通道选择和回读。虚线以下是第 3 版新增的空闲保持：U808、升压 U817 和 4 个空闲支路。<span class="key" style="background:var(--accent)"></span>主控和通道信号 <span class="key" style="background:var(--accent2)"></span>第 2、3 版新增的网络。同名网络标签相连。</figcaption>
</figure>

<section>
  <h2>空闲保持怎么工作</h2>
  <ol class="prose">
    <li><b>学习只记电压。</b>原车开关接着、不按时记空闲电压，按住各档时记各档电压。不需要知道 APM 内部用多大的上拉、上拉到几伏。</li>
    <li><b>发出前先辨识这一路的 APM 侧等效电源。</b>电阻阵高阻时量一次（开路电压 V0），接 0 Ω 直通量一次，再接一个已知电阻量一次。三个读数解出等效内阻 Rth 和这一路的 PTC + 继电器串联电阻。之后所有档位都按 V = V0 × R / (R + Rth) 求电阻，再按回读微调 1～2 次。</li>
    <li><b>要不要保持空闲，由读数决定。</b>学到的空闲电压比 V0 低 30 mV 以上，说明原车开关空闲时本身接着电阻、而现在不在线：算出所需对地电阻 R = Rth × V空闲 / (V0 − V空闲)，扣掉 2.2 kΩ 和滑臂电阻得到电位器码值，接通空闲继电器后按回读微调。原车开关还接着，或者空闲本来就是开路，空闲支路保持断开。</li>
    <li><b>按下时从空闲直接跳到按下。</b>先在电阻阵高阻时选好这一路，再写电阻阵控制字。电阻阵并在空闲支路上，APM 看到的是空闲电平直接变成按下电平；松开时只撤电阻阵，回到空闲。中间不经过开路，不会被当成断线。</li>
    <li><b>电流有上限。</b>AD5292 50 kΩ 款的端子持续电流是 ±2 mA。串联 2.2 kΩ 后，5 V 上拉时最大约 1.6 mA；上拉到 12 V 的车，固件按辨识结果拒绝会超过 1.8 mA 的空闲电阻，并在界面上提示。</li>
  </ol>
  <div class="table"><table>
    <thead><tr><th>APM 上拉（到 5 V）</th><th>空闲支路能保持的电压</th><th>开路电压</th><th>电阻阵最差误差（200–4600 mV）</th></tr></thead>
    <tbody>{''.join(span_rows)}</tbody>
  </table></div>
  <p class="note prose">按上拉 5 V、回读负载 356 kΩ、串联电阻 10 Ω 计算。空闲电压高于表中上限、低于开路电压的极少数情况（空闲电阻大于约 52 kΩ），只能在“接近”和“开路”之间二选一，误差最大几十 mV。</p>
</section>

<section>
  <h2>U808 引脚分配</h2>
  <div class="table"><table>
    <thead><tr><th>端口 · 引脚</th><th>网络</th><th>用途</th></tr></thead>
    <tbody>{''.join(u808_rows)}</tbody>
  </table></div>
  <p class="note prose">U808 的 RESET 接 WS_RST_N：PG2 为低时全部引脚变成输入，SYNC 被 R837–R840 拉高，空闲继电器的 LED 没有电流，VPOT_EN 被 R843 拉低。AD5292 的 SDO、RDY 不用；4 片各用一根 SYNC，不走菊花链。</p>
</section>

<section>
  <h2>4 路怎么切换</h2>
  <div class="grid2">
    <div class="table"><table>
      <thead><tr><th>PG2</th><th>CH_A1</th><th>CH_A0</th><th>接通的通道</th><th>回读</th></tr></thead>
      <tbody>
        <tr><td>低</td><td>×</td><td>×</td><td>全部断开</td><td>0</td></tr>
        <tr><td>高</td><td>0</td><td>0</td><td>K1（U802）</td><td>K1 ÷ 6.36</td></tr>
        <tr><td>高</td><td>0</td><td>1</td><td>K2（U803）</td><td>K2 ÷ 6.36</td></tr>
        <tr><td>高</td><td>1</td><td>0</td><td>K3（U804）</td><td>K3 ÷ 6.36</td></tr>
        <tr><td>高</td><td>1</td><td>1</td><td>K4（U805）</td><td>K4 ÷ 6.36</td></tr>
      </tbody>
    </table></div>
    <ul class="prose">
      <li>PG2 经 Q808B 反相成 CH_EN_N，同时控制 U806 的 1G 和 U807 的 E。主控复位、上电或 HardFault 时 PG2 被 R814 拉低：U801 复位，电阻阵全关，4 个继电器全断。</li>
      <li>运行时总有一路接在公共节点上。电阻阵不下拉时，这一路只多了 356 kΩ 回读负载，对 APM 的影响约 0.6%，辨识时已经算在等效电源里。</li>
      <li>换通道时，固件先把电阻阵切到高阻，再改 CH_A0/CH_A1，然后等 5 ms。TLP176AM 断开最长 0.5 ms，接通最长 3 ms，天然先断后通。</li>
      <li>选中后用 0 Ω 直通档量一次接口电压，算出这一路的 PTC + 继电器串联电阻，之后按这个值求解。没有这一步，最差误差约 38 mV。</li>
    </ul>
  </div>
</section>

<section>
  <h2>U801 引脚分配</h2>
  <div class="table"><table>
    <thead><tr><th>端口 · 引脚</th><th>用途</th><th>说明</th></tr></thead>
    <tbody>{''.join(tca_rows)}</tbody>
  </table></div>
</section>

<section>
  <h2>电阻阵</h2>
  <p class="prose">电阻阵是并联电导式：每一位是一个电阻加一个开关管，接通的位并联对 WS_RET 下拉。第 2 版的 13 位是按这台 APM 的 2.2 kΩ 上拉选的；换成 1 kΩ 上拉，200 mV 附近只能做到约 90 mV 的误差（全开最小约 52 Ω 太大），换成 10 kΩ 上拉，4.5 V 附近 91 k/180 k/360 k 之间的台阶约 57 mV。第 3 版用 U808 的空脚加了两位：b13 = 47 Ω 和 b14 = 150 kΩ，上拉 1–10 kΩ 时最差误差降到 {worst_lo:.0f}–{worst_hi:.0f} mV（上表）。15 位共 32767 种组合，固件不再建 32 KB 查找表，每次直接遍历求解（Cortex-M7 上估计几毫秒），驱动上下文从约 33 KB 降到约 1.7 KB。</p>
  <div class="table"><table>
    <thead><tr><th>位</th><th>电阻</th><th>阻值</th><th>电导 µS</th><th>开关管</th><th>控制引脚</th><th>来源</th></tr></thead>
    <tbody>{''.join(ladder_rows)}</tbody>
  </table></div>
</section>

<section>
  <h2>和第 2 版相比改了什么</h2>
  <ol class="prose">
    <li><b>每路加一个空闲保持支路。</b>WS_KFk（PTC 内侧）经光耦继电器 U813–U816、2.2 kΩ（R827–R830）接到 AD5292 的 W 端，B 端回 WS_RET。第 2 版的空闲电阻预留位 R827–R830（默认不贴）取消，位号改给这 4 个串联电阻。</li>
    <li><b>加 U808（TCA9539 @0x75）。</b>P00/P01 是电位器 SPI 的 SCLK/DIN，P02–P05 是 4 根 SYNC，P06 是升压使能，P10–P13 驱动 4 个空闲继电器。复位脚和 U801 一样接 WS_RST_N。</li>
    <li><b>加升压 U817。</b>AD5292 的端子电压必须在 VSS 和 VDD 之间；VDD 约 25.9 V，接口误接 12 V 甚至 TVS 钳位时，端子仍在范围内。</li>
    <li><b>回线改到 WS_RET。</b>电阻阵开关管、直通管、电位器 B 端和 J801 的偶数脚接 WS_RET，经 R844 一点接到 GND。线束上把 J801 偶数脚接到原车开关在 APM 一侧的回线，车窗电机电流造成的地电位差就不进按键电压。</li>
    <li><b>回读改快、量程加大。</b>C802 由 10 nF 改为 1 nF，R819 由 100 kΩ 改为 56 kΩ：时间常数从约 0.75 ms 降到约 47 µs，固件可以分辨脉冲供电的上拉，只取上拉接通期间的读数；量程从 13.2 V 加到约 21 V，上拉到蓄电池电压（12 V，充电时 14.4 V）的输入也能测。默认过压门限仍是 5.5 V，这类车在配置里把 ovp_threshold_mv 调到 16 V。</li>
    <li>其余与第 2 版相同：4 路共用 13 位电阻阵、同一时刻只接通一路、主控引脚只用 PG2 和 PC2_C。</li>
  </ol>
</section>

<div class="grid2">
<section>
  <h2>固件对接</h2>
<pre>// U801 TCA9539 @ 0x74（不变）：b0–b12、SHORT、CH_A0/A1
// U808 TCA9539 @ 0x75：先写输出（SYNC 高、IDLE_N 高、
//   VPOT_EN 低），再设方向；复位后输出寄存器为 0xFF

// 辨识这一路（电阻阵高阻、空闲支路断开）
v0  = adc();                      // 开路
vsh = adc_with(SHORT);            // 直通：rs
v1  = adc_with(R1);               // 已知电阻
a = vsh / (v0 - vsh); b = v1 / (v0 - v1);
rth = (R1 - rds) / (b - a);  rs = a * rth - rds;

// 空闲保持（只在 v_idle &lt; v0 - 30 mV 时）
VPOT_EN = 1; delay_ms(5);         // 升压建立
spi(SYNCk, 0x1802);               // 命令 6：C1 = 1 允许写 RDAC
r = rth * v_idle / (v0 - v_idle) - 2200 - rw;
spi(SYNCk, 0x0400 | code(r));     // 命令 1：写 RDAC
IDLEk_N = 0;                      // 接通空闲继电器，再按回读微调

// 按下：选通道（电阻阵高阻）→ 写控制字；
// 模型 V = v0 × R / (R + rth)，回读后微调 1～2 次</pre>
</section>
<section>
  <h2>PCB 摆放</h2>
  <ul>
    <li>J801 放在 J401/J402 同一侧板边，D801–D804、F801–F804 紧挨 J801。</li>
    <li>R815–R818 尽量靠近各自的 PTC 前端，WS_S1–WS_S4 到 U807 的走线远离电阻阵和继电器的输出线。</li>
    <li>U802–U805 排成一列，4 个输出端汇到一条短的 WS_NODE。继电器输入、输出两侧按 5 mm 爬电距离留空。</li>
    <li>R819、C802 贴近 U301 第 28 脚；C803、C804 贴近各自芯片的 VCC 脚。</li>
    <li>J801 的 4 个 WS_RET 脚连成一小块铜，R844 放在旁边，作为 WS_RET 与地平面唯一的连接点；电阻阵开关管和 AD5292 的 B 端走线回到这块铜，不就近打地孔。</li>
    <li>U817、L801、D805 的开关回路尽量小，远离 WS_ADC、R815–R819 和 U807。WS_VPOT 只到 4 个 AD5292，电流很小，细线即可。</li>
    <li>U809–U812 靠近各自的空闲继电器 U813–U816；C809–C820 贴近 AD5292 的引脚。</li>
    <li>整块电路远离 U202、U201 的开关节点。</li>
  </ul>
</section>
</div>

<section>
  <h2>主控引脚改动</h2>
  <div class="table"><table>
    <thead><tr><th>U301 引脚</th><th>原状态</th><th>新网络</th><th>用途</th></tr></thead>
    <tbody>
      <tr><td>87 · PG2</td><td>未连接</td><td><span class="pill">WS_RST_N</span></td><td>推挽输出。高电平时 U801 工作、4 路中的一路接通；复位或高阻时 R814 拉低，全部关断。</td></tr>
      <tr><td>28 · PC2_C</td><td>未连接</td><td><span class="pill">WS_ADC</span></td><td>模拟输入 ADC3_INP0。所选通道的接口电压 ÷ 6.36，满量程约 21 V。</td></tr>
      <tr><td>81/82 · PD12/PD13</td><td>EXP_SCL/SDA</td><td><span class="pill keep">不变</span></td><td>I²C4，已有 TUSB320（0x47/0x67）和 J302。U801 地址 0x74、U808 地址 0x75，不冲突（J302 外接设备的地址需要另外确认）。</td></tr>
    </tbody>
  </table></div>
</section>

<section>
  <h2>元件清单</h2>
  <div class="table"><table>
    <thead><tr><th>位号</th><th>型号 / 值</th><th>厂商</th><th>封装</th><th>数量</th><th>说明</th></tr></thead>
    <tbody>{bom_rows}</tbody>
  </table></div>
</section>

<section>
  <h2>逐脚网表</h2>
  <p class="note prose">格式和仓库里的 schematic-pins.json 相同。在嘉立创EDA画完后导出网表，可以和这份逐脚对照。</p>
  <details>
    <summary>new_parts + changed_pins（{n_parts} 个器件，{n_pins} 个引脚）</summary>
    <div class="row"><button type="button" id="copy-netlist">复制 JSON</button><span class="note" id="copy-status" aria-live="polite"></span></div>
    <pre id="netlist">{html.escape(netlist_json)}</pre>
  </details>
</section>

<section class="prose">
  <h2>依据和还没验证的</h2>
  <ul>
    <li>第 2 版已核对：TLP176AM（引脚 1 阳极、3 阴极、4/6 输出；60 V，导通 ≤ 2 Ω，触发电流 ≤ 3 mA，接通 ≤ 3 ms，断开 ≤ 0.5 ms）；SN74LVC139A；CD74HC4052。PC2_C = ADC3_INP0 来自社区引用的数据手册引脚表。</li>
    <li>第 3 版按 AD5291/AD5292 数据手册 Rev. H 核对：引脚 1 RESET、2 VSS、3 A、4 W、5 B、6 VDD、7 EXT_CAP、8 VLOGIC、9 GND、10 DIN、11 SCLK、12 SYNC、13 SDO（开漏）、14 RDY；16 位帧，写 RDAC 前要先用命令 6 把控制寄存器 C1 置 1；RESET 上升沿把 RDAC 恢复成 20-TP 存储值（出厂为中间值）；EXT_CAP 接 1 µF、耐压 ≥ 7 V；单电源规格按 21–33 V 给出；端子持续电流 ±3 mA（20 kΩ）、±2 mA（50 kΩ、100 kΩ）；滑臂电阻 60–100 Ω；端子电压超出 VSS–VDD 时由内部二极管钳位；上电顺序 GND、VSS、VLOGIC、VDD，之后才允许端子上有电压（空闲继电器在固件确认升压建立前保持断开）。</li>
    <li>按 TPS61040 数据手册 SLVS413L 核对：SOT-23-5 引脚 1 SW、2 GND、3 FB、4 EN、5 VIN；VFB 1.233 V；输出最高 28 V，SW 耐压 30 V，输入 1.8–6 V；EN 不能悬空；关断时输入经电感和二极管直通到输出；前馈电容从 10 pF 起步；电感 2.2–47 µH。</li>
    <li>新器件的立创料号还没在嘉立创库里核对（本次查库时客户端不可用），L801 的具体型号待定；B5819W 的引脚 1 按阴极画，导入时要对一下库里的符号。</li>
    <li>辨识和串联电阻标定会短暂接通 0 Ω 直通和一个已知电阻，各约 10 ms。去抖很短的 APM 可能把它当成一次按键，要在实车 APM 上确认；必要时改成只在 APM 休眠前后做。</li>
    <li>PTC 的具体料号和阻值还没定。15 V 的 TVS 只按 12 V 系统设计，24 V 车要换 SMF33CA 并复核耐压。</li>
    <li>表中的误差是计算值。这页还没画进嘉立创EDA，PCB 没更新，没跑 DRC，也没有实板测试。</li>
  </ul>
</section>
</main>

<script>
document.getElementById('copy-netlist').addEventListener('click', function () {{
  var pre = document.getElementById('netlist'), status = document.getElementById('copy-status');
  function selectIt() {{ var r = document.createRange(); r.selectNodeContents(pre); var s = window.getSelection(); s.removeAllRanges(); s.addRange(r); status.textContent = '已选中，按 ⌘C 复制'; }}
  try {{
    navigator.clipboard.writeText(pre.textContent).then(function () {{ status.textContent = '已复制'; }}, selectIt);
  }} catch (e) {{ selectIt(); }}
}});
</script>
"""

with open(OUT, "w", encoding="utf-8") as f:
    f.write(PAGE)
print("parts", n_parts, "dnp", n_dnp, "pins", n_pins, "new nets", len(new_nets))
print(new_nets)
