# The JP-8000 front panel, and where our pages disagree

Source: the **Roland JP-8000 Owner's Manual, "Front and Rear Panel", pp. 10-11**
— the numbered panel diagram and its key. The manual is a scanned PDF with no
text layer, so this was read off the diagrams themselves.

> An earlier version of this file was derived from
> `jeJucePlugin/skins/jeTrancy/jeTrancy.rml`. **That skin is the JP-8080 rack**,
> not the keyboard: it carries an Ensemble section, the whole Voice Modulator
> block, Unison, an Osc 2 external input and `voiceAssignRack` /
> `RemoteKeyboardChannel`, none of which exist on a JP-8000. Several claims made
> from it were wrong and are corrected below. The skin renders both models
> behind `deviceModel` conditionals, which is what made it look authoritative.

## The panel, by the manual's own numbering

Upper-right block, and the transport strip under it:

| # | Section | Controls |
|---|---|---|
| 1 | **LFO 1** | Waveform, Rate, Fade |
| 2 | **OSC COMMON** | Ring, LFO1 & Env Destination, Env Depth, Osc Balance, X-Mod Depth, LFO 1 Depth, pitch-env A / D |
| 10 | Volume | (host level) |
| 11 | **ARPEGGIATOR / RPS** | **Tempo**, Mode, Range, On/Off, Arp Hold, Rec |
| 12 | Motion Control | 1, 2 |

Main row, left to right:

| # | Section | Controls |
|---|---|---|
| 3 | **OSC 1** | Waveform, Ctrl 1, Ctrl 2 |
| 4 | **OSC 2** | Range, Sync, Waveform, Pulse Width, PWM Depth, Fine/Wide |
| 5 | **FILTER** | Type, Slope, Cutoff, Resonance, Key Follow, LFO 1 Depth, Env Depth, **A D S R** |
| 6 | **AMP** | LFO 1 Depth, Auto/Manual pan, Level, **A D S R** |

Row below it:

| # | Section | Controls |
|---|---|---|
| 13 | **KEY & PANEL** | Key Mode (Single/Dual/Split), Panel Select (Lower/Upper) |
| 7 | **TONE CONTROL** | Bass, Treble |
| 8 | **CHORUS** | Level, Type |
| 9 | **DELAY** | Time, Feedback, Level |

Left-hand block:

| # | Section | Controls |
|---|---|---|
| 20 | **CONTROLLER** | Ribbon Assign, Velocity Assign, **Bend Range** |
| 21 | **LFO 2** | Rate, **Depth**, Depth Select (Pitch / Filter / Amp) |
| 22 | **KEYBOARD** | Portamento Time, Velocity, Legato/Mono, Osc Shift, Keyboard Shift |
| 23-25 | Ribbon controller, bend/mod lever | |

## What this changes for our pages

Confirmed right as shipped:

- **Filter and Amp each own an ADSR on the panel**, in their own sections — so
  the filter envelope belongs on Filter and the amp envelope on Amp, which is
  where they are.
- **Tempo belongs with the arpeggiator** (section 11), not on Setup.
- **Osc 1 and Osc 2 are separate panel sections**, which is what the recent
  split gives us.

To change:

1. **Merge "Mix & Mod" and "Pitch" into one "Osc Common" page.** Section 2 holds
   Ring, LFO1&Env Destination, Env Depth, Osc Balance, X-Mod Depth, LFO 1 Depth
   and the pitch-envelope A/D — our two pages are halves of one panel section,
   and together they are exactly eight cells.
2. **LFO 2 is Rate + Depth + Depth Select.** The panel has ONE depth knob and a
   selector for Pitch / Filter / Amp; the sysex splits that into three
   parameters, which we currently scatter across Pitch, Filter and Amp. Put
   `lfo2_rate`, `lfo2_depth_select` and the three depths on the LFO 2 page —
   five controls, and it stops being our thinnest page.
3. **Tone / Chorus / Delay are three sections, not one.** Our "FX & Tone" page
   merges all three. It fits eight cells and is defensible, but if it is ever
   split, split it the way the panel does.
4. **Osc Shift is a KEYBOARD control** (section 22), not an oscillator one. It
   is on our Osc 1 page; it belongs on Play.
5. **Bend Range sits with Ribbon Assign and Velocity Assign** in CONTROLLER, not
   with Key Mode. Ours is on Play, which is closer than the skin suggested.

Corrections to the JP-8080-derived version of this file: Portamento is a
KEYBOARD control, not an Amp one; Bend Range is in CONTROLLER, not with Key
Mode; and there is no Ensemble section at all.

## The system parameters

The JP-8000 has no global knob strip — the manual reaches System settings
through **SHIFT/EXIT** (section 17: "Press this button to set Performance
parameters or System parameters, p.85"). So System is a *mode you enter*, which
maps cleanly onto a third `ui_hierarchy.modes` entry rather than a page hidden
under Performance.

`page_plan.mjs` supports that: with `modes` present the mode names ARE level
names and the selector is itself a pick-list, so a `system` level of plain
params works as a mode root with no preset browser. The plumbing: widen `mode`
past 0..1 (`parse_mode` and its clamp), teach `bank_view_for` that a mode may
have no bank list, and add the system parameters to the address table under
`AREA_SYSTEM`. Writes there are verified — `sysreq` / `sysparam` in
`jp8000_render` read and write them.
