#!/usr/bin/env python3
"""Generate src/dsp/jp8000_params.h from gearmulator's parameterDescriptions_je.json.

The header carries the parameter table (key -> temp-area address, range,
enum options) plus the pre-rendered `chain_params` JSON and `ui_hierarchy`
JSON the plugin answers with. Run from the repo root:

    python3 src/tools/gen_params.py

Everything the JP-8000 keyboard has is exposed (the rack-only tail 0xEF+ and
the VoiceModulator page are skipped). Signed value lists become `int` ranges
with an offset (raw = value + off) rather than 128-option enums.
"""
import json, re, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "libs/gearmulator/source/ronaldo/je8086/jeJucePlugin/parameterDescriptions_je.json")
OUT = os.path.join(ROOT, "src/dsp/jp8000_params.h")

s = open(SRC).read()
s = re.sub(r',(\s*[\]}])', r'\1', s)
doc = json.loads(s)
VL = doc["valuelists"]
DESC = doc["parameterdescriptions"]

# ---- naming -----------------------------------------------------------------
WORDS = {
    "FLT": "Filter", "FIT": "Filter", "ENV": "Env", "OSC": "Osc", "AMP": "Amp", "LFO1": "LFO1",
    "LFO2": "LFO2", "CTRL": "Ctrl", "DPTH": "Depth", "DEPTH": "Depth", "ATK": "Attack",
    "DCY": "Decay", "REL": "Release", "FREQ": "Freq", "RESO": "Reso", "FEEDBK": "Fdbk",
    "FEEDBACK": "Fdbk", "MOD": "Mod", "ASGN": "Assign", "MULTIFX": "Multi-FX",
    "MULTI-FX": "Multi-FX", "SUS": "Sus", "SUSTAIN": "Sustain", "PWM": "PWM", "OSC1": "Osc1",
    "OSC2": "Osc2", "C.CROSS": "C.Cross", "C.OSC": "C.Osc", "C.PITCH": "C.Pitch",
    "C.CUTOFF": "C.Cutoff", "C.OSC1": "C.Osc1", "C.OSC2": "C.Osc2", "C.RESONANCE": "C.Resonance", "C.KEY": "C.Key", "C.FIT": "C.Filter",
    "C.AMP": "C.Amp", "C.TONE": "C.Tone", "C.MULTIFX": "C.Multi-FX", "C.DELAY": "C.Delay",
    "V.LFO1": "V.LFO1", "V.LFO2": "V.LFO2", "V.CROSS": "V.Cross", "V.OSC": "V.Osc",
    "V.OSC1": "V.Osc1", "V.OSC2": "V.Osc2", "DEPTH": "Depth", "LEVEL": "Level", "RATE": "Rate", "TIME": "Time", "V.PITCH": "V.Pitch", "V.PITCHENV": "V.Pitch Env",
    "V.CUTOFF": "V.Cutoff", "V.FLT": "V.Filter", "V.KEY": "V.Key", "V.AMP": "V.Amp",
    "V.TONE": "V.Tone", "V.MULTIFX": "V.Multi-FX", "V.DELAY": "V.Delay", "V.PORTAMENTO": "V.Portamento",
    "V.ACTIVE": "Active", "X-MOD": "X-Mod", "&": "&", "1": "1", "2": "2",
}

def title(display):
    out = []
    for w in display.split():
        out.append(WORDS.get(w, w.capitalize()))
    return " ".join(out)

def snake(name):
    n = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', '_', name).lower()
    n = n.replace("control_", "ctl_", 1) if n.startswith("control_") else n
    n = n.replace("velocity_", "vel_", 1) if n.startswith("velocity_") and n != "velocity_switch" else n
    return n

# explicit overrides: json name -> (key, name, short)
OVERRIDE = {
    "Lfo1Waveform": ("lfo1_waveform", "LFO1 Wave", "Wave"),
    "Lfo1Rate": ("lfo1_rate", "LFO1 Rate", "Rate"),
    "Lfo1Fade": ("lfo1_fade", "LFO1 Fade", "Fade"),
    "Lfo2Rate": ("lfo2_rate", "LFO2 Rate", "Rate"),
    "Lfo2DepthSelect": ("lfo2_depth_select", "LFO2 Dest", "Dest"),
    "RingModulatorSwitch": ("ring_mod", "Ring Mod", "Ring"),
    "CrossModulationDepth": ("cross_mod_depth", "Cross Mod", "X-Mod"),
    "OscillatorBalance": ("osc_balance", "Osc Balance", "Bal"),
    "Lfo1AndEnvelopeDestination": ("lfo1_env_dest", "LFO1/Env Dest", "Dest"),
    "OscLfo1Depth": ("osc_lfo1_depth", "Pitch LFO1", "LFO1"),
    "PitchLfo2Depth": ("pitch_lfo2_depth", "Pitch LFO2", "LFO2"),
    "PitchEnvelopeDepth": ("pitch_env_depth", "Pitch Env", "Env"),
    "PitchEnvelopeAttackTime": ("pitch_env_attack", "Pitch Attack", "Atk"),
    "PitchEnvelopeDecayTime": ("pitch_env_decay", "Pitch Decay", "Dcy"),
    "Osc1Waveform": ("osc1_waveform", "Osc1 Wave", "Wave"),
    "Osc1Control1": ("osc1_ctrl1", "Osc1 Ctrl 1", "Ctrl1"),
    "Osc1Control2": ("osc1_ctrl2", "Osc1 Ctrl 2", "Ctrl2"),
    "Osc2Waveform": ("osc2_waveform", "Osc2 Wave", "Wave"),
    "Osc2SyncSwitch": ("osc2_sync", "Osc2 Sync", "Sync"),
    "Osc2Range": ("osc2_range", "Osc2 Range", "Range"),
    "Osc2FineWide": ("osc2_fine", "Osc2 Fine", "Fine"),
    "Osc2Control1": ("osc2_ctrl1", "Osc2 Ctrl 1", "Ctrl1"),
    "Osc2Control2": ("osc2_ctrl2", "Osc2 Ctrl 2", "Ctrl2"),
    "FilterType": ("filter_type", "Filter Type", "Type"),
    "CutoffSlope": ("cutoff_slope", "Slope", "Slope"),
    "CutoffFrequency": ("cutoff", "Cutoff", "Cut"),
    "Resonance": ("resonance", "Resonance", "Res"),
    "CutoffFrequencyKeyFollow": ("key_follow", "Key Follow", "KeyF"),
    "FilterLfo1Depth": ("filter_lfo1_depth", "Filter LFO1", "LFO1"),
    "FilterLfo2Depth": ("filter_lfo2_depth", "Filter LFO2", "LFO2"),
    "FilterEnvelopeDepth": ("filter_env_depth", "Filter Env", "Env"),
    "FilterEnvelopeAttackTime": ("filter_attack", "Filter Attack", "Atk"),
    "FilterEnvelopeDecayTime": ("filter_decay", "Filter Decay", "Dcy"),
    "FilterEnvelopeSustainLevel": ("filter_sustain", "Filter Sustain", "Sus"),
    "FilterEnvelopeReleaseTime": ("filter_release", "Filter Release", "Rel"),
    "AmpLevel": ("amp_level", "Amp Level", "Level"),
    "AmpLfo1Depth": ("amp_lfo1_depth", "Amp LFO1/Pan", "LFO1"),
    "AmpLfo2Depth": ("amp_lfo2_depth", "Amp LFO2", "LFO2"),
    "AmpEnvelopeAttackTime": ("amp_attack", "Amp Attack", "Atk"),
    "AmpEnvelopeDecayTime": ("amp_decay", "Amp Decay", "Dcy"),
    "AmpEnvelopeSustainLevel": ("amp_sustain", "Amp Sustain", "Sus"),
    "AmpEnvelopeReleaseTime": ("amp_release", "Amp Release", "Rel"),
    "AutoPanManualPanSwitch": ("amp_lfo1_mode", "Amp LFO1 Mode", "Mode"),
    "ToneControlBass": ("tone_bass", "Tone Bass", "Bass"),
    "ToneControlTreble": ("tone_treble", "Tone Treble", "Treb"),
    "ChorusType": ("chorus_type", "Chorus Type", "Type"),
    "ChorusLevel": ("chorus_level", "Chorus Level", "Level"),
    "DelayType": ("delay_type", "Delay Type", "Type"),
    "DelayTime": ("delay_time", "Delay Time", "Time"),
    "DelayFeedback": ("delay_feedback", "Delay Fdbk", "Fdbk"),
    "DelayLevel": ("delay_level", "Delay Level", "Level"),
    "BendRangeUp": ("bend_up", "Bend Up", "Up"),
    "BendRangeDown": ("bend_down", "Bend Down", "Down"),
    "PortamentoSwitch": ("portamento", "Portamento", "Porta"),
    "PortamentoTime": ("portamento_time", "Porta Time", "Time"),
    "MonoSwitch": ("mono", "Mono", "Mono"),
    "LegatoSwitch": ("legato", "Legato", "Legato"),
    "OscillatorShift": ("osc_shift", "Osc Shift", "Shift"),
    "MorphBendAssign": ("morph_bend", "Morph Bend", "Morph"),
    "ControlPortamentoTime": ("ctl_portamento_time", "C.Porta Time", "Porta"),
    "VelocitySwitch": ("velocity_switch", "Velocity", "Vel"),
    "ActiveIndicatorOfBender": ("active_bender", "Bender Active", "Bend"),
    "ActiveIndicatorOfVelocityAssign": ("active_velocity", "Vel Active", "Vel"),
    "ActiveIndicatorOfControlAssign": ("active_control", "Ctrl Active", "Ctrl"),
}

# ---- value lists -> type ----------------------------------------------------
def numeric_list(lst):
    """A list that is a plain integer sequence -> (min, max) of displayed values."""
    try:
        vals = [int(x.replace("+", "")) for x in lst]
    except ValueError:
        return None
    if vals != list(range(vals[0], vals[0] + len(vals))) and vals != list(range(vals[0], vals[0] - len(vals), -1)):
        return None
    return vals

def type_of(p):
    """-> dict(type=..., min, max, off, options)."""
    lo, hi, tt = p.get("min", 0), p.get("max", 127), p.get("toText")
    if tt in (None, "", "unsignedZero", "unsigned201"):
        return dict(type="int", min=lo, max=hi, off=0)
    lst = VL[tt]
    if isinstance(lst, dict):        # oscShift: {"4":"-2 oct",...}
        opts = [lst[str(i)] for i in range(lo, hi + 1)]
        return dict(type="enum", options=opts, off=lo)
    if tt == "bendRangeDown":
        return dict(type="int", min=-hi, max=0, off=0, neg=True)
    nums = numeric_list(lst[lo:hi + 1])
    if nums is not None:
        return dict(type="int", min=nums[0], max=nums[-1], off=lo - nums[0])
    if tt == "splitPoint":
        return dict(type="int", min=lo, max=hi, off=0)   # note number
    if tt == "signed255":
        return dict(type="int", min=lo - 127, max=hi - 127, off=127)
    if tt == "assignOsc2RangeFineWide":
        return dict(type="int", min=lo - 127, max=hi - 127, off=127)
    if tt in ("oscBalance", "LFO1DepthPan"):
        return dict(type="int", min=-64, max=63, off=64)
    if tt == "osc2Range":
        return dict(type="int", min=-25, max=25, off=25)    # -25 = WIDE-, +25 = WIDE+
    if tt == "osc2RangeFine":
        return dict(type="int", min=-50, max=50, off=50)
    if tt == "LFO1DepthPan":
        return dict(type="int", min=-64, max=63, off=64)
    opts = lst[lo:hi + 1]
    if len(opts) <= 0 or len(opts) > 128:
        raise SystemExit("cannot map %s (%s)" % (p["name"], tt))
    return dict(type="enum", options=opts, off=lo)

# ---- collect ----------------------------------------------------------------
AREA_PATCH, AREA_PART_UP, AREA_PART_LO, AREA_COMMON = 0, 1, 2, 3
params = []          # dicts: key,name,short,area,lin,bit14,type,min,max,off,options,default
seen_patch = set()

def add(p, area, key, name, short, lin, bit14):
    t = type_of(p)
    d = dict(key=key, name=name, short=short, area=area, lin=lin, bit14=bit14, default=p.get("default", 0), **t)
    params.append(d)
    return d

for p in DESC:
    if p.get("page", 0) != 0: continue
    idx = p["index"]
    if idx < 0x10 or idx >= 0xEF: continue
    if p.get("isPublic") is False: continue
    if idx in seen_patch: continue           # first (generic) variant wins
    seen_patch.add(idx)
    ov = OVERRIDE.get(p["name"])
    if ov: key, name, short = ov
    else:
        key = snake(p["name"]); name = title(p.get("displayName") or p["name"]); short = None
    bit14 = p.get("max", 127) > 127 or p.get("toText") in ("signed255", "assignOsc2RangeFineWide")
    add(p, AREA_PATCH, key, name, short, idx, bit14)

PART = [(0x2, "midi_ch", "MIDI Ch", "Ch"), (0x3, "transpose", "Transpose", "Trans"),
        (0x4, "delay_sync", "Delay Sync", "Delay"), (0x5, "lfo1_sync", "LFO1 Sync", "LFO1"),
        (0x6, "chorus_sync", "Chorus Sync", "Chorus")]
for area, pre, label in ((AREA_PART_UP, "up_", "Upper "), (AREA_PART_LO, "lo_", "Lower ")):
    for idx, key, name, short in PART:
        p = next(x for x in DESC if x.get("page") == 2 and x["index"] == idx)
        add(p, area, pre + key, label + name, short, idx, False)

COMMON = [(0x10, "key_mode", "Key Mode", "Mode"), (0x11, "split_point", "Split Point", "Split"),
          (0x13, "part_detune", "Part Detune", "Detune"), (0x14, "output_assign", "Output", "Out"),
          (0x15, "arp_dest", "Arp Dest", "Dest"), (0x16, "voice_assign", "Voice Assign", "Voices"),
          (0x17, "arp_switch", "Arpeggio", "Arp"), (0x18, "arp_mode", "Arp Mode", "Mode"),
          (0x19, "arp_beat", "Arp Beat", "Beat"), (0x1a, "arp_range", "Arp Range", "Range"),
          (0x1b, "arp_hold", "Arp Hold", "Hold"), (0x1d, "trigger_switch", "Ind. Trigger", "Trig"),
          (0x1e, "trigger_dest", "Trigger Dest", "Dest"), (0x1f, "trigger_ch", "Trigger Ch", "Ch"),
          (0x20, "trigger_note", "Trigger Note", "Note"), (0x22, "tempo", "Tempo", "BPM")]
for idx, key, name, short in COMMON:
    p = next(x for x in DESC if x.get("page") == 3 and x["index"] == idx and x.get("isPublic") is not False)
    bit14 = idx in (0x20, 0x22)
    d = add(p, AREA_COMMON, key, name, short, idx, bit14)
    if key == "trigger_ch": d.update(type="int", min=1, max=16, off=-1)
    if key == "tempo": d.update(type="int", min=20, max=250, off=0)
    if key == "trigger_note": d.update(type="int", min=0, max=127, off=0)

assert len(params) < 230, len(params)
byKey = {p["key"]: p for p in params}
assert len(byKey) == len(params), "duplicate keys"

# ---- hierarchy --------------------------------------------------------------
def K(*keys):
    for k in keys: assert k in byKey, k
    return list(keys)

PATCH_LEVELS = [
    ("osc", "Oscillators", K("osc1_waveform", "osc1_ctrl1", "osc1_ctrl2", "osc_balance", "osc2_waveform",
                             "osc2_range", "osc2_fine", "osc2_ctrl1", "osc2_ctrl2", "osc2_sync",
                             "cross_mod_depth", "ring_mod", "osc_shift")),
    ("pitch", "Pitch", K("lfo1_env_dest", "osc_lfo1_depth", "pitch_lfo2_depth", "pitch_env_depth",
                         "pitch_env_attack", "pitch_env_decay")),
    ("filter", "Filter", K("cutoff", "resonance", "filter_env_depth", "key_follow", "filter_attack",
                           "filter_decay", "filter_sustain", "filter_release", "filter_type", "cutoff_slope",
                           "filter_lfo1_depth", "filter_lfo2_depth")),
    ("amp", "Amp", K("amp_level", "amp_attack", "amp_decay", "amp_sustain", "amp_release", "amp_lfo1_depth",
                     "amp_lfo1_mode", "amp_lfo2_depth", "tone_bass", "tone_treble")),
    ("lfo", "LFO", K("lfo1_waveform", "lfo1_rate", "lfo1_fade", "lfo2_rate", "lfo2_depth_select")),
    ("fx", "Effects", K("chorus_type", "chorus_level", "delay_type", "delay_time", "delay_feedback", "delay_level")),
    ("play", "Play", K("portamento", "portamento_time", "mono", "legato", "bend_up", "bend_down",
                       "velocity_switch", "morph_bend", "active_bender", "active_velocity", "active_control")),
    ("ribbon", "Ribbon Ctrl", [p["key"] for p in params if p["key"].startswith("ctl_")]),
    ("velocity", "Velocity", [p["key"] for p in params if p["key"].startswith("vel_")]),
]
PERF_LEVELS = [
    ("perf_setup", "Setup", K("key_mode", "split_point", "part_detune", "voice_assign", "output_assign", "tempo")),
    ("perf_upper", "Upper Part", K("up_midi_ch", "up_transpose", "up_delay_sync", "up_lfo1_sync", "up_chorus_sync")),
    ("perf_lower", "Lower Part", K("lo_midi_ch", "lo_transpose", "lo_delay_sync", "lo_lfo1_sync", "lo_chorus_sync")),
    ("perf_arp", "Arpeggiator", K("arp_switch", "arp_mode", "arp_beat", "arp_range", "arp_hold", "arp_dest")),
    ("perf_trigger", "Ind. Trigger", K("trigger_switch", "trigger_dest", "trigger_ch", "trigger_note")),
]
covered = set(k for _, _, ks in PATCH_LEVELS + PERF_LEVELS for k in ks)
missing = [p["key"] for p in params if p["key"] not in covered]
assert not missing, missing

MAIN_KNOBS = K("cutoff", "resonance", "filter_env_depth", "amp_attack", "amp_decay", "amp_sustain",
               "amp_release", "delay_level")

def level(label, keys, extra=None, knobs=None):
    items = [{"key": k, "label": byKey[k]["name"], **({"short_name": byKey[k]["short"]} if byKey[k]["short"] else {})}
             for k in keys]
    if extra: items += extra
    d = {"label": label, "params": items}
    d["knobs"] = knobs if knobs is not None else keys[:8]
    return d

levels = {
    "patch": {"list_param": "patch", "count_param": "patch_count", "name_param": "patch_name",
              "children": "patch_main", "knobs": MAIN_KNOBS, "params": []},
    "patch_main": level("Patch", MAIN_KNOBS,
                        extra=[{"key": "part", "label": "Edit Part"}, {"key": "bank", "label": "Bank"}] +
                              [{"level": lid, "label": lab} for lid, lab, _ in PATCH_LEVELS]),
    "performance": {"list_param": "performance", "count_param": "performance_count",
                    "name_param": "performance_name", "children": "perf_main", "knobs": MAIN_KNOBS, "params": []},
    "perf_main": level("Performance", [], knobs=K("key_mode", "split_point", "part_detune", "voice_assign",
                                                  "arp_switch", "arp_mode", "arp_beat", "tempo"),
                       extra=[{"key": "perf_bank", "label": "Bank"}] +
                             [{"level": lid, "label": lab} for lid, lab, _ in PERF_LEVELS]),
}
# perf_main knobs must be present in its params
levels["perf_main"]["params"] = [{"key": k, "label": byKey[k]["name"], "short_name": byKey[k]["short"]}
                                 for k in levels["perf_main"]["knobs"]] + levels["perf_main"]["params"]
for lid, lab, keys in PATCH_LEVELS + PERF_LEVELS:
    levels[lid] = level(lab, keys)

hierarchy = {"modes": ["patch", "performance"], "mode_param": "mode", "levels": levels}

# ---- chain_params -----------------------------------------------------------
VIZ = {
    "cutoff": ("filter", "cutoff"), "resonance": ("filter", "resonance"), "filter_type": ("filter", "mode"),
    "cutoff_slope": ("filter", "slope"),
    "filter_attack": ("fenv", "attack"), "filter_decay": ("fenv", "decay"), "filter_sustain": ("fenv", "sustain"),
    "filter_release": ("fenv", "release"),
    "amp_attack": ("aenv", "attack"), "amp_decay": ("aenv", "decay"), "amp_sustain": ("aenv", "sustain"),
    "amp_release": ("aenv", "release"),
    "pitch_env_attack": ("penv", "attack"), "pitch_env_decay": ("penv", "decay"),
    "lfo1_waveform": ("lfo1", "shape"), "lfo1_rate": ("lfo1", "rate"),
}
cp = []
cp.append({"key": "mode", "name": "Mode", "type": "mode", "options": ["Patch", "Performance"]})
cp.append({"key": "part", "name": "Edit Part", "short_name": "Part", "type": "enum", "options": ["Upper", "Lower", "Both"]})
for p in params:
    e = {"key": p["key"], "name": p["name"], "type": p["type"]}
    if p["short"]: e["short_name"] = p["short"]
    if p["type"] == "int":
        e["min"], e["max"] = p["min"], p["max"]
        e["default"] = p["default"] - p["off"] if not p.get("neg") else -p["default"]
    else:
        e["options"] = p["options"]
        e["default"] = p["default"] - p["off"]
    if p["key"] == "tempo": e["unit"] = "BPM"
    if p["key"] in VIZ:
        e["viz"] = {"group": VIZ[p["key"]][0], "role": VIZ[p["key"]][1]}
    if p["key"] in ("up_transpose", "lo_transpose"): e["unit"] = "st"
    cp.append(e)
cp_static = json.dumps(cp, separators=(",", ":"))
# the dynamic bank enums are appended by the plugin: strip the closing bracket
assert cp_static.endswith("]")
cp_prefix = cp_static[:-1]

# ---- emit -------------------------------------------------------------------
def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

def chunks(s, n=4000):
    """Split a long string literal so no single literal exceeds MSVC/portable limits."""
    out = []
    for i in range(0, len(s), n):
        out.append("    " + cstr(s[i:i + n]))
    return "\n".join(out)

enum_tables = []
lines = []
lines.append("/* GENERATED by src/tools/gen_params.py from gearmulator's parameterDescriptions_je.json. DO NOT EDIT. */")
lines.append("#pragma once\n#include <stdint.h>\n")
lines.append("enum { JP_AREA_PATCH = 0, JP_AREA_PART_UP = 1, JP_AREA_PART_LO = 2, JP_AREA_COMMON = 3 };\n")
lines.append("typedef struct {\n    const char *key;\n    uint8_t area;      /* JP_AREA_* */\n"
             "    uint8_t lin;       /* linear byte offset inside the area (MSB byte for 14-bit) */\n"
             "    uint8_t bit14;\n    int16_t vmin, vmax; /* displayed range */\n"
             "    int16_t off;       /* raw = value + off */\n    int16_t def;       /* displayed default */\n"
             "    uint8_t nopts;     /* enum option count, 0 for int */\n"
             "    uint8_t neg;       /* raw = -value (bend range down) */\n} jp_param_t;\n")
rows = []
for p in params:
    dmin, dmax = (p["min"], p["max"]) if p["type"] == "int" else (0, len(p["options"]) - 1)
    off = p["off"] if not p.get("neg") else 0
    d = (p["default"] - p["off"]) if not p.get("neg") else -p["default"]
    rows.append('    {%s, %d, 0x%02x, %d, %d, %d, %d, %d, %d, %d},' % (
        cstr(p["key"]), p["area"], p["lin"], 1 if p["bit14"] else 0, dmin, dmax, off, d,
        len(p["options"]) if p["type"] == "enum" else 0, 1 if p.get("neg") else 0))
lines.append("#define JP_PARAM_COUNT %d" % len(params))
lines.append("static const jp_param_t jp_params[JP_PARAM_COUNT] = {\n" + "\n".join(rows) + "\n};\n")
lines.append("/* chain_params JSON without its closing ']' — the plugin appends the bank enums. */")
lines.append("static const char jp_chain_params_prefix[] =\n" + chunks(cp_prefix) + ";\n")
hj = json.dumps(hierarchy, separators=(",", ":"))
lines.append("static const char jp_ui_hierarchy[] =\n" + chunks(hj) + ";\n")
open(OUT, "w").write("\n".join(lines))
print("wrote", OUT, "-", len(params), "params, chain_params", len(cp_prefix), "B, hierarchy", len(hj), "B")
