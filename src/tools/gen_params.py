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
AREA_PATCH, AREA_PART_UP, AREA_PART_LO, AREA_COMMON, AREA_SYSTEM = 0, 1, 2, 3, 4
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

# 0x12 PanelSelect is the hardware's UPPER/LOWER/UPPER&LOWER button. It is
# addressable so "Edit Part" can write it, but it is kept OUT of chain_params
# (see CP_SKIP): one switch must not have two rows that can disagree.
COMMON = [(0x10, "key_mode", "Key Mode", "Mode"), (0x11, "split_point", "Split Point", "Split"),
          (0x12, "panel_select", "Panel Select", "Panel"),
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
    # Ranges below are what the FIRMWARE accepts, probed on the device by
    # writing a value and reading the area back. The JSON descriptions are the
    # JP-8080's in places and cannot be trusted for the keyboard:
    #   arp_beat     89 lands, 90 and 96 are rejected -> 90 options, not 97
    #   trigger_note 128 lands and MEANS "ALL"; Chariots actually uses it
    #   voice_assign 5 and 6 are rejected -> our 5 options are right, and the
    #                JSON's 7 (8-2, 7-3, ...) sum to 10 voices = the rack
    if key == "trigger_note": d.update(type="int", min=0, max=128, off=0)
    if key == "arp_beat":
        d["options"] = d["options"][:90]
        d.update(min=0, max=len(d["options"]) - 1)


# ---- System area -------------------------------------------------------------
# Reached on the hardware with SHIFT/EXIT ("Performance parameters or System
# parameters", manual p.85) -- a mode you enter, not a page under Performance.
#
# These live at address area 0x00000000, outside the temp performance, so they
# are the one group whose writes do not carry the TEMP base. The image was
# already being filled from DT1 replies (img_system, IMG_SYSTEM); nothing could
# address it until now.
#
# Only the keyboard's parameters, and only the ones that mean something through
# a Schwung slot: the pattern/motion sequencer settings and the rack-only tail
# are skipped. Ranges from jemiditypes.h SystemParameter and verified writable
# by read-back (sysreq/sysparam in jp8000_render).
SYSTEM = [
    # Ten of the keyboard's system parameters, and it is the exclusions that
    # carry the reasoning. Three entries govern hardware that CANNOT EXIST in
    # an emulation hosted in a slot, so no user setting of them is ever
    # observable:
    #
    #   Power-Up Mode   decides what the keyboard loads at power-on. The child
    #                   boots from a snapshot and we apply the slot's own state
    #                   over it, so the host has already answered.
    #   Local           switches the internal keyboard into the sound engine.
    #                   `KeyScanner::read()` returns 0 unconditionally and its
    #                   write() is empty -- the key matrix is a STUB, so that
    #                   keyboard reports no keys pressed, forever.
    #   Keyboard Shift  transposes the same stub. Measured to confirm it:
    #                   a note on the remote channel is identical at -2, 0, +2.
    #
    # The ribbon pair is NOT in that category and stays. `Faders` is fully
    # emulated, `setFader(which, value)` exists, and kFader_Ribbon1/2 sit in
    # its table beside pitch bend and the mod wheel -- nothing calls it yet,
    # which makes the ribbon latent rather than impossible.
    #
    # Gate Time Ratio is the same shape, and it took two attempts to say so
    # honestly. It belongs to the pattern/RPS block, not the arpeggiator:
    # measured against a VERIFIED-RUNNING arpeggiator (mode UP, beat NORMAL
    # 1/16, 11 voice starts at 0.405s), all six of its values render
    # BYTE-IDENTICAL audio. The first attempt at this used a sustained pad and
    # never established the arp was running at all, so it proved nothing and
    # was reported as if it had.
    #
    # What it would act on is RPS, and RPS is selectable but SILENT: arp_mode
    # = RPS gives zero voice starts on every pattern family, across the whole
    # keyboard, with and without a running clock -- while the identical script
    # in mode UP arpeggiates. RPS plays back RECORDED phrases, and PatternSetup
    # (SystemArea 0x1000) holds only 48 loop lengths, not phrase data; the
    # phrases are recorded with the panel REC and the keyboard, and the key
    # matrix is a stub. So it stays for the same reason the ribbon does.
    #
    # Being unable to observe a parameter is not the same as knowing it does
    # nothing; a stub device IS knowing.
    #
    # The seventh field is the DEFAULT, and it is what the panel reads after
    # our boot force, not the factory value. Every max below WAS measured: the
    # firmware silently REJECTS an out-of-range write and keeps what it had,
    # so an accepted write is the only proof of a range. The probe was checked
    # against a positive control first -- Master Tune moves the same note
    # 170 / 174 / 180 Hz -- because a probe that measures the wrong thing
    # reports "no change" just as confidently as a real result.
    (0x02, "sys_perf_ctrl_ch", "Perf Ctrl Ch", "PerfCh", "enum",
     [str(i) for i in range(1, 17)] + ["Off"], 16),
    (0x04, "sys_midi_sync", "MIDI Sync", "Sync", "enum", ["Off", "On"], 1),
    (0x06, "sys_txrx_edit_mode", "TxRx Edit Mode", "EdMode", "enum", ["Mode 1", "Mode 2"], 1),
    (0x07, "sys_txrx_edit", "TxRx Edit", "Edit", "enum", ["Off", "On"], 1),
    (0x08, "sys_txrx_pc", "TxRx Prog Change", "PC", "enum", ["Off", "PC", "Bank Sel + PC"], 2),
    # We force this to 3 at boot -- it is the only note path that feeds the
    # arpeggiator, and the firmware default (Off) is why the arp never ran.
    (0x09, "sys_remote_ch", "Remote Ctrl Ch", "RemCh", "enum",
     [str(i) for i in range(1, 17)] + ["All", "Off"], 2),
    # parameterDescriptions_je.json page 5 index 10: 0..100, 50 = A440.
    (0x0a, "sys_master_tune", "Master Tune", "Tune", "int", (0, 100), 50),
    (0x0e, "sys_gate_ratio", "Gate Time Ratio", "Gate", "enum",
     ["Real", "Staccato", "33%", "50%", "66%", "100%"], 0),
    (0x15, "sys_ribbon_rel", "Ribbon Relative", "RibRel", "enum", ["Off", "On"], 0),
    (0x16, "sys_ribbon_hold", "Ribbon Hold", "RibHld", "enum", ["Off", "On"], 0),
]
for _e in SYSTEM:
    idx, key, name, short, typ, spec = _e[:6]
    dflt = _e[6] if len(_e) > 6 else 0
    d = dict(key=key, name=name, short=short, area=AREA_SYSTEM, lin=idx, bit14=False,
             default=dflt, type=typ, off=0)
    if typ == "enum":
        d["options"] = spec; d["min"] = 0; d["max"] = len(spec) - 1
    else:
        d["min"], d["max"] = spec
    params.append(d)

assert len(params) < 230, len(params)
byKey = {p["key"]: p for p in params}
assert len(byKey) == len(params), "duplicate keys"

# "part" (Edit Part) is a real control but not a sysex parameter -- it writes
# PanelSelect through the plugin, so it has no row in the address table. Give it
# a descriptor here so it can be placed on a page like any other cell. It is NOT
# appended to `params`, so it stays out of chain_params (which declares it
# explicitly above) and out of the page-coverage assertion.
byKey["part"] = dict(key="part", name="Edit Part", short="Part", area=None, lin=None,
                     bit14=False, default=0, type="enum",
                     options=["Upper", "Lower", "Both"], min=0, max=2, off=0)

# ---- short names for the Ribbon / Velocity families -------------------------
# 79 of these had none, and the grid labels a cell with `name` when short_name
# is absent -- names here run to 20 characters against a ~6-character cell, so
# every one of them clipped. Derived rather than hand-written, and asserted
# UNIQUE per page below: two cells reading the same thing is the same defect as
# two bank rows reading the same thing.
CTL_SECT = [("Filter", "F"), ("Amp", "A"), ("Pitch", "P"), ("Osc1", "O1"), ("Osc2", "O2"),
            ("LFO1", "L1"), ("LFO2", "L2"), ("Tone", "T"), ("Delay", "D"),
            ("Multi-FX", "FX"), ("Cross", "X")]
CTL_LEAF = [("Control 1", "C1"), ("Control 2", "C2"), ("Control1", "C1"), ("Control2", "C2"),
            ("LFO1 Depth", "L1"), ("LFO2 Depth", "L2"),
            ("Env Sustain", "Sus"), ("Env Release", "Rel"), ("Env Attack", "Atk"),
            ("Env Decay", "Dcy"), ("Env Depth", "Env"), ("Key Follow", "KFol"),
            ("Ctrl Bass", "Bass"), ("Ctrl Treble", "Treb"), ("Sustain", "Sus"),
            ("Release", "Rel"), ("Attack", "Atk"), ("Decay", "Dcy"), ("Depth", "Dep"),
            ("Level", "Lvl"), ("Balance", "Bal"), ("Resonance", "Res"), ("Cutoff", "Cut"),
            ("Feedback", "Fdbk"), ("Portamento", "Port"), ("Range", "Rng"),
            ("Fine Wide", "Fine"), ("Rate", "Rat"), ("Fade", "Fade"), ("Time", "Tim"),
            ("Mod Depth", "Dep")]

def derive_short(name):
    n = re.sub(r"^(?:[CV]\.|Ctrl |Vel )", "", name).strip()
    sect = ""
    for k, v in CTL_SECT:
        if n.startswith(k + " ") or n == k:
            sect = v; n = n[len(k):].strip(); break
    leaf = None
    for pat, ab in CTL_LEAF:
        if pat in n: leaf = ab; break
    if leaf is None: leaf = n.replace(" ", "")[:4] or "x"
    return ((sect + "." + leaf) if sect else leaf)[:6]

for _p in params:
    if not _p["short"] and _p["key"].startswith(("ctl_", "vel_")):
        _p["short"] = derive_short(_p["name"])
for _fam in ("ctl_", "vel_"):
    _v = [_p["short"] for _p in params if _p["key"].startswith(_fam)]
    assert len(_v) == len(set(_v)), "duplicate short_name on the %s page: %s" % (
        _fam, sorted({x for x in _v if _v.count(x) > 1}))

# ---- hierarchy --------------------------------------------------------------
def K(*keys):
    for k in keys: assert k in byKey, k
    return list(keys)


# ---- Ribbon / Velocity page order -------------------------------------------
# These 40-parameter families were emitted in the source JSON's order, which put
# the FILTER envelope across pages 3 and 4 and the AMP envelope across 4 and 5 --
# a four-stage envelope you cannot see at once. Grouped by section instead, with
# each ADSR quartet whole inside one row of four.
#
# Deliberately NO viz graphics here, unlike the Filter and Amp pages: every one
# of these is a MODULATION DEPTH, not a value. A filter curve drawn from "how
# much the ribbon moves the cutoff" would show a response the synth never has.
MOD_PAGES = [
    ["osc1_control1", "osc1_control2", "osc2_control1", "osc2_control2",
     "osc2_range", "osc2_fine_wide", "oscillator_balance", "cross_modulation_depth"],
    ["lfo1_rate", "lfo1_fade", "lfo2_rate", "pitch_lfo1_depth",
     "pitch_lfo2_depth", "pitch_envelope_depth", "pitch_envelope_attack_time",
     "pitch_envelope_decay_time"],
    ["cutoff_frequency", "resonance", "cutoff_freq_key_follow", "filter_env_depth",
     "filter_env_attack_time", "filter_env_decay_time", "@filter_sustain",
     "filter_env_release_time"],
    ["filter_lfo1_depth", "filter_lfo2_depth", "amp_lfo1_depth", "amp_lfo2_depth",
     "amp_env_attack_time", "amp_env_decay_time", "amp_env_sustain_level",
     "amp_env_release_time"],
    ["amp_level", "tone_control_bass", "tone_control_treble", "multi_effects_level",
     "delay_time", "delay_feedback", "delay_level", "portamento_time"],
]

def mod_order(pre):
    """Resolve the page plan to real keys for one family, and prove it is total.

    The two families are not named identically -- ctl_ has
    filter_env_sustain_level where vel_ has filter_env_sus_level -- so the
    sustain slot is resolved by search and the result asserted complete. A
    silent drop here would lose a parameter off the end of the UI."""
    have = [x["key"] for x in params if x["key"].startswith(pre)]
    out = []
    for page in MOD_PAGES:
        for slot in page:
            if slot == "@filter_sustain":
                cand = [k for k in have if k.startswith(pre + "filter_env_sus")]
                assert len(cand) == 1, (pre, cand)
                out.append(cand[0])
            else:
                k = pre + slot
                assert k in have, "unknown key %s" % k
                out.append(k)
    assert sorted(out) == sorted(have), (
        "%s page plan is not total: missing=%s extra=%s"
        % (pre, sorted(set(have) - set(out)), sorted(set(out) - set(have))))
    return out

PATCH_LEVELS = [
    # One "Oscillators" level of 13 put osc2_ctrl1 as the last cell of page 1 and
    # osc2_ctrl2 as the first of page 2 -- Osc 2's own pair split across a page,
    # the same defect the envelopes had across a row. Three levels instead, one
    # per panel section, and WAVE / CTRL1 / CTRL2 sit in the SAME three
    # positions on both oscillator pages, so the two read alike and the labels
    # need no O1./O2. prefix to stay unambiguous.
    ("osc1", "Osc 1", K("osc1_waveform", "osc1_ctrl1", "osc1_ctrl2", "osc_shift")),
    ("osc2", "Osc 2", K("osc2_waveform", "osc2_ctrl1", "osc2_ctrl2", "osc2_range",
                        "osc2_fine", "osc2_sync")),
    ("oscmix", "Mix & Mod", K("osc_balance", "cross_mod_depth", "ring_mod")),
    # Env depth/attack/decay lead so the pitch-envelope pair stays inside row 1;
    # with lfo1_env_dest gone to the LFO page it had slid onto cells 3-4.
    ("pitch", "Pitch", K("pitch_env_depth", "pitch_env_attack", "pitch_env_decay",
                         "osc_lfo1_depth", "pitch_lfo2_depth")),
    ("filter", "Filter", K("cutoff", "resonance", "filter_env_depth", "key_follow", "filter_attack",
                           "filter_decay", "filter_sustain", "filter_release", "filter_type", "cutoff_slope",
                           "filter_lfo1_depth", "filter_lfo2_depth")),
    # Bass/Treble are the panel's TONE section, not Amp. Tacked on the end here
    # they made Amp ten cells -- one page of eight plus a two-cell orphan. Moved
    # to the output page below, both pages land on exactly eight.
    ("amp", "Amp", K("amp_attack", "amp_decay", "amp_sustain", "amp_release", "amp_level", "amp_lfo1_depth",
                     "amp_lfo1_mode", "amp_lfo2_depth")),
    # Split for the same reason as the oscillators: one "LFO" page put LFO1 and
    # LFO2 side by side, so both rate knobs had to be relabelled L1.RAT/L2.RAT
    # to stay apart. On their own pages each is simply RATE, in the same slot.
    # lfo1_env_dest comes here from Pitch -- DESTINATION is an LFO-section
    # control on the panel, not a pitch one.
    ("lfo1", "LFO 1", K("lfo1_waveform", "lfo1_rate", "lfo1_fade", "lfo1_env_dest")),
    ("lfo2", "LFO 2", K("lfo2_rate", "lfo2_depth_select")),
    ("fx", "FX & Tone", K("chorus_type", "chorus_level", "delay_type", "delay_time", "delay_feedback",
                          "delay_level", "tone_bass", "tone_treble")),
    ("play", "Play", K("portamento", "portamento_time", "mono", "legato", "bend_up", "bend_down",
                       "velocity_switch", "morph_bend", "active_bender", "active_velocity", "active_control")),
    ("ribbon", "Ribbon Ctrl", mod_order("ctl_")),
    ("velocity", "Velocity", mod_order("vel_")),
]
PERF_LEVELS = [
    ("perf_setup", "Setup", K("key_mode", "split_point", "part_detune", "voice_assign", "output_assign", "tempo")),
    ("perf_upper", "Upper Part", K("up_midi_ch", "up_transpose", "up_delay_sync", "up_lfo1_sync", "up_chorus_sync")),
    ("perf_lower", "Lower Part", K("lo_midi_ch", "lo_transpose", "lo_delay_sync", "lo_lfo1_sync", "lo_chorus_sync")),
    ("perf_arp", "Arpeggiator", K("arp_switch", "arp_mode", "arp_beat", "arp_range", "arp_hold", "arp_dest")),
    ("perf_trigger", "Ind. Trigger", K("trigger_switch", "trigger_dest", "trigger_ch", "trigger_note")),
]
# System is a MODE, not a page under Performance: on the hardware you leave the
# performance to reach it (SHIFT/EXIT, manual p.85) and what you set there
# outlives the patch you were editing. Four MIDI entries earn the root because
# they are the ones a Schwung slot actually has to get right -- Remote Ctrl Ch
# is what makes the arpeggiator hear anything at all -- and the rest sit one
# dive down, grouped the way the manual groups them.
# One setup page. Ten parameters do not fit eight cells, so the split is where
# it costs least: the eight that a slot can actually reach today stay together
# on Setup -- channels and tuning on the top row, the TxRx switches and gate on
# the bottom -- and the two ribbon settings, which nothing drives yet, are the
# single dive. Three pages of two, three and four cells were the alternative.
SYS_LEVELS = [
    ("sys_ribbon", "Ribbon", K("sys_ribbon_rel", "sys_ribbon_hold")),
]
SYS_MAIN = K("sys_remote_ch", "sys_perf_ctrl_ch", "sys_midi_sync", "sys_master_tune",
             "sys_txrx_edit", "sys_txrx_edit_mode", "sys_txrx_pc", "sys_gate_ratio")

covered = set(k for _, _, ks in PATCH_LEVELS + PERF_LEVELS + SYS_LEVELS for k in ks) | set(SYS_MAIN)
# panel_select has no page of its own on purpose: "Edit Part" writes it, and a
# second row for the same switch could disagree with the first.
UNPAGED = {"panel_select"}
missing = [p["key"] for p in params if p["key"] not in covered and p["key"] not in UNPAGED]
assert not missing, missing

# A viz graphic must sit inside ONE ROW of four. The amp envelope used to run
# cells 3-6 here -- straddling the boundary -- so it degraded to four separate
# bars and drew no curve at all. delay_level moves up; the ADSR owns row 2.
# Edit Part takes the fourth slot, and delay_level gives it up: delay_level now
# has a home on FX & Tone, while Edit Part had none and was pushed onto a page
# of its own -- a whole screen holding one control. Patch is exactly eight cells
# now, so nothing paginates. Filter keeps cells 0-1 and the amp envelope row 2.
MAIN_KNOBS = K("cutoff", "resonance", "filter_env_depth", "part",
               "amp_attack", "amp_decay", "amp_sustain", "amp_release")

# Two cells on one page reading the same word is not an abbreviation, it is a
# broken page -- and it is invisible in the contract, only in the pixels. These
# were found by rendering all 23 grid pages and diffing the labels.
#
# The override is PER LEVEL because short_name is otherwise global and the right
# answer differs by page: "delay_level" must stay "Level" on Main, where it is
# the only level, and become "DlLvl" on FX, where chorus_level sits beside it.
# levelShortNames() (page_plan.mjs) gives a level entry precedence over the
# chain_params one, which is exactly this case.
LEVEL_SHORT = {
    "perf_main": {"key_mode": "KeyMd", "arp_mode": "ArpMd"},
    "fx": {"chorus_type": "ChTyp", "delay_type": "DlTyp",
           "chorus_level": "ChLvl", "delay_level": "DlLvl"},
    "play": {"velocity_switch": "VelSw", "active_velocity": "VelAc"},
}

def level(label, keys, extra=None, knobs=None, lid=None):
    ov = LEVEL_SHORT.get(lid or "", {})
    items = [{"key": k, "label": byKey[k]["name"],
              **({"short_name": ov.get(k) or byKey[k]["short"]} if (ov.get(k) or byKey[k]["short"]) else {})}
             for k in keys]
    if extra: items += extra
    d = {"label": label, "params": items}
    d["knobs"] = knobs if knobs is not None else keys[:8]
    return d

# The preset browser is a HIERARCHY: folder -> bank -> preset. A real library is
# nested (one collection here is 97 files across 30 folders) and a flat list of
# 36 truncated paths is unreadable on 128 pixels. bank_folder/bank_in_folder are
# served by the plugin and are mode-aware.
BROWSE = dict(patch=("patch_banks", "patch_list", "patch", "Patch"),
              performance=("perf_banks", "perf_list", "performance", "Performance"))
levels = {
    "patch": {"label": "Banks", "list_param": "bank_folder", "count_param": "bank_folder_count",
              "name_param": "bank_folder_name", "children": "patch_banks", "knobs": MAIN_KNOBS, "params": []},
    "patch_banks": {"label": "Bank", "list_param": "bank_in_folder", "count_param": "bank_in_folder_count",
                    "name_param": "bank_in_folder_name", "children": "patch_list", "knobs": MAIN_KNOBS, "params": []},
    "patch_list": {"label": "Patch", "list_param": "patch", "count_param": "patch_count",
                   "name_param": "patch_name", "children": "patch_main", "knobs": MAIN_KNOBS, "params": []},
    # No {"key": "bank"} row: the hierarchy above IS the bank chooser, and the
    # flat enum lists every bank from every folder with only the basename, so
    # two files with the same name in different folders read identically.
    "patch_main": level("Patch", MAIN_KNOBS, lid="patch_main",
                        extra=[{"level": lid, "label": lab} for lid, lab, _ in PATCH_LEVELS]),
    "performance": {"label": "Banks", "list_param": "bank_folder", "count_param": "bank_folder_count",
                    "name_param": "bank_folder_name", "children": "perf_banks", "knobs": MAIN_KNOBS, "params": []},
    "perf_banks": {"label": "Bank", "list_param": "bank_in_folder", "count_param": "bank_in_folder_count",
                   "name_param": "bank_in_folder_name", "children": "perf_list", "knobs": MAIN_KNOBS, "params": []},
    "perf_list": {"label": "Performance", "list_param": "performance", "count_param": "performance_count",
                  "name_param": "performance_name", "children": "perf_main", "knobs": MAIN_KNOBS, "params": []},
    "perf_main": level("Performance", [], lid="perf_main", knobs=K("key_mode", "split_point", "part_detune", "voice_assign",
                                                  "arp_switch", "arp_mode", "arp_beat", "tempo"),
                       extra=[{"level": lid, "label": lab} for lid, lab, _ in PERF_LEVELS]),
}
# perf_main knobs must be present in its params
levels["perf_main"]["params"] = [{"key": k, "label": byKey[k]["name"],
                                  "short_name": LEVEL_SHORT.get("perf_main", {}).get(k) or byKey[k]["short"]}
                                 for k in levels["perf_main"]["knobs"]] + levels["perf_main"]["params"]
levels["system"] = level("Setup", SYS_MAIN, lid="system",
                         extra=[{"level": lid, "label": lab} for lid, lab, _ in SYS_LEVELS])
for lid, lab, keys in PATCH_LEVELS + PERF_LEVELS + SYS_LEVELS:
    levels[lid] = level(lab, keys, lid=lid)

# A page that shows one word twice is ambiguous, and the contract cannot see it
# -- only the rendered pixels can. Assert it here so the next regeneration
# cannot quietly reintroduce what rendering all 23 grid pages just found.
_dupes = {}
for _lid, _lvl in levels.items():
    _seen = {}
    for _e in _lvl.get("params", []):
        if not isinstance(_e, dict) or not _e.get("key"): continue
        _s = (_e.get("short_name") or byKey.get(_e["key"], {}).get("short")
              or _e.get("label") or _e["key"]).upper()
        _seen.setdefault(_s, []).append(_e["key"])
    _d = {k: v for k, v in _seen.items() if len(v) > 1}
    if _d: _dupes[_lid] = _d
assert not _dupes, "duplicate cell labels on a page: %s" % _dupes

hierarchy = {"modes": ["patch", "performance", "system"], "mode_param": "mode", "levels": levels}


# panel_select is addressable but must not get its own chain_params row: the
# user-facing control for it is the existing "Edit Part".
CP_SKIP = {"panel_select"}

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
# A viz graphic must sit inside ONE ROW of four, or it silently degrades to
# separate bars and draws no curve -- which is exactly how the amp envelope
# shipped on Main and Amp. Assert row-containment for every viz group, and for
# the Ribbon/Velocity ADSR quartets too: those carry no graphic on purpose, but
# a four-stage envelope split across a row (or a page) is still unreadable.
_ROWS = 4
_straddle = []
for _lid, _lvl in levels.items():
    _cells = [e["key"] for e in _lvl.get("params", []) if isinstance(e, dict) and e.get("key")]
    for _pi in range(0, (len(_cells) + 7) // 8):
        _page = _cells[_pi * 8:(_pi + 1) * 8]
        _groups = {}
        for _i, _k in enumerate(_page):
            _g = VIZ.get(_k, (None,))[0]
            if _g is None:
                for _fam in ("ctl_", "vel_"):
                    if _k.startswith(_fam) and "_env_" in _k and _k.rsplit("_env_", 1)[1].split("_")[0] in (
                            "attack", "decay", "sustain", "sus", "release"):
                        _g = _k[:_k.index("_env_")] + "_env"
            if _g: _groups.setdefault(_g, []).append(_i)
        for _g, _idx in _groups.items():
            if len({_i // _ROWS for _i in _idx}) != 1:
                _straddle.append((_lid, _pi + 1, _g, _idx))
assert not _straddle, "viz/envelope group straddles a row: %s" % _straddle


# ---- short_options: the enum square is two lines of THREE characters ---------
# 191 option strings across 18 enums were longer than that, so they wrapped or
# were cut on the cell -- "MANUAL" as MAN/UAL, "LOWER & UPPER" as a smear. The
# fix is NOT to shorten the option itself: the held-knob header shows the full
# value and must keep reading "MANUAL". `short_options` is a parallel array that
# only the square consults, which is what it exists for.
SHORT_OPT = {
    "osc1_waveform": {"SUPER SAW": "SUP SAW", "TRIANGLE MOD": "TRI MOD", "FEEDBACK OSC": "FBK OSC",
                      "NOISE": "NSE", "SQUARE": "SQR", "TRIANGLE": "TRI", "SAW": "SAW"},
    "osc2_waveform": {"SQR (PWM)": "PWM", "TRIANGLE": "TRI", "SAW": "SAW", "NOISE": "NSE"},
    "lfo1_waveform": {"TRIANGLE": "TRI", "SAWTOOTH": "SAW", "SQUARE": "SQR", "S&H": "S&H",
                      "RANDOM": "RND", "NOISE": "NSE"},
    "cutoff_slope": {"-12dB/oct": "-12", "-24dB/oct": "-24"},
    "output_assign": {"MIX OUT": "MIX", "PARALLEL OUT": "PAR OUT"},
    # The square is TWO lines of three, so a value that breaks at a space uses
    # both of them -- "2 OCT" reads as 2/OCT, where "2OCT" crams one line and
    # risks a cut. Prefer the split form; each token must fit three characters.
    "arp_dest": {"LOWER & UPPER": "LOW UPP", "LOWER": "LOW", "UPPER": "UPP"},
    "arp_mode": {"UP & DOWN": "UP DWN", "RANDOM": "RND", "DOWN": "DWN", "UP": "UP", "RPS": "RPS"},
    "arp_range": {"1 OCTAVE": "1 OCT", "2 OCTAVES": "2 OCT", "3 OCTAVES": "3 OCT", "4 OCTAVES": "4 OCT"},
    "trigger_dest": {"FILTER & AMP": "FLT AMP", "FILTER ENV": "FLT", "FILTER": "FLT",
                     "AMPLITUDE ENV": "AMP", "AMP": "AMP"},
    "delay_type": {"PANNING L->R": "PAN L>R", "PANNING R->L": "PAN R>L", "PANNING SHORT": "PAN SHT",
                   "MONO SHORT": "MON SHT", "MONO LONG": "MON LNG"},
    "key_mode": {"SINGLE": "SGL", "DUAL": "DUL", "SPLIT": "SPL"},
    "amp_lfo1_mode": {"MANUAL": "MAN", "LFO1": "LFO1", "ENV": "ENV"},
    # System. The generic 6-char truncation turned these into "Per"/"Las" and
    # "Ban" -- three words that name nothing.
    "sys_txrx_pc": {"Off": "OFF", "PC": "PC", "Bank Sel + PC": "BNK PC"},
    "sys_txrx_edit_mode": {"Mode 1": "MOD 1", "Mode 2": "MOD 2"},
    "sys_gate_ratio": {"Staccato": "STAC", "Real": "REAL"},
}

def _short_sync(o):
    """1/8 TRIPLET -> 1/8T, 1/16 DOTTED -> 1/16D, 8 MEASURES -> 8ME."""
    if o in ("OFF", "LFO1"): return o[:4]
    t = o.replace(" TRIPLET", "T").replace(" DOTTED", "D")
    t = t.replace(" MEASURES", "ME").replace(" MEAS", "ME")
    return t[:6] if all(len(x) <= 3 for x in t.split(" ")) else t.replace(" ", "")[:3]

def _short_beat(o):
    """The 90 arpeggio patterns: keep the family initial and its number."""
    if o.startswith("NORMAL "): return o[7:][:6] if len(o[7:]) <= 3 else o[7:][:4]
    for word, ab in (("PORTAMENTO ", "P"), ("SEQUENCE ", "S"), ("STRUMMING ", "ST"),
                     ("PERCUSSION ", "PC"), ("REFRAIN ", "RF"), ("ECHO ", "EC"), ("MUTE ", "MU")):
        if o.startswith(word): return (ab + o[len(word):].replace(" ", ""))[:6]
    return o[:6]

def short_options_for(key, opts):
    m = SHORT_OPT.get(key)
    if m is not None:
        out = [m.get(o, o[:6]) for o in opts]
    elif key.endswith(("_delay_sync", "_lfo1_sync", "_chorus_sync")):
        out = [_short_sync(o) for o in opts]
    elif key == "arp_beat":
        out = [_short_beat(o) for o in opts]
    elif any(len(o) > 6 for o in opts):
        out = [o[:3] for o in opts]
    else:
        return None
    # The square is two lines and breaks at a space, so the budget is PER LINE,
    # not per string: "FLT AMP" fits as FLT/AMP where "FLTAMP" would be cut.
    # Four narrow characters do fit a line -- "2OCT" and "1/12" render whole --
    # so the cap is 4 per token, two tokens at most.
    bad = [x for x in out if any(len(t) > 4 for t in x.split(" "))
           or len(x.split(" ")) > 2]
    assert not bad, (key, bad)
    return out if any(a != b for a, b in zip(out, opts)) else None

cp = []
cp.append({"key": "mode", "name": "Mode", "type": "mode", "options": ["Patch", "Performance", "System"]})
cp.append({"key": "part", "name": "Edit Part", "short_name": "Part", "type": "enum",
           "options": ["Upper", "Lower", "Both"],
           # declared here rather than through the params loop, so it needs its own
           # short_options -- "Lower" was wrapping to LOW/ER in the square
           "short_options": ["UPP", "LOW", "BTH"]})
for p in params:
    if p["key"] in CP_SKIP: continue
    e = {"key": p["key"], "name": p["name"], "type": p["type"]}
    if p["short"]: e["short_name"] = p["short"]
    if p["type"] == "int":
        e["min"], e["max"] = p["min"], p["max"]
        e["default"] = p["default"] - p["off"] if not p.get("neg") else -p["default"]
    else:
        e["options"] = p["options"]
        _so = short_options_for(p["key"], p["options"])
        if _so: e["short_options"] = _so
        e["default"] = p["default"] - p["off"]
    if p["key"] == "tempo": e["unit"] = "BPM"
    if p["key"] in VIZ:
        e["viz"] = {"group": VIZ[p["key"]][0], "role": VIZ[p["key"]][1]}
    elif p["key"].startswith(("ctl_", "vel_")):
        # viz: false -- keep the host's DETECTORS off these.
        #
        # Declaring no viz is not the same as declaring none wanted. viz.mjs
        # runs a detector pool over every key nobody claimed, and it infers a
        # group from the NAMES: "C.Pitch Env Attack" next to "C.Pitch Env
        # Decay" reads as a pitch envelope, so Ribbon Ctrl - 2 drew an envelope
        # curve across two modulation depths. The picture was of a shape the
        # synth never has.
        #
        # Every ctl_/vel_ parameter is an AMOUNT -- how far the ribbon or
        # velocity moves something -- so none of them may become a graphic.
        e["viz"] = False
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
lines.append("enum { JP_AREA_PATCH = 0, JP_AREA_PART_UP = 1, JP_AREA_PART_LO = 2, JP_AREA_COMMON = 3, JP_AREA_SYSTEM = 4 };\n")
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
