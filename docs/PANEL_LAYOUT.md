# The JP-8000 front panel, and what our pages get wrong

Source: `jeJucePlugin/skins/jeTrancy/jeTrancy.rml`. That skin is an editor
layout modelled on the hardware panel, not a photograph of one — but it is the
best evidence in the tree, it carries absolute coordinates, and its section
comments are the panel's own section names. Extracted with the positions below;
`x` runs left-to-right across the panel.

| Section | x | y | What sits in it |
|---|---|---|---|
| LFO 1 | 11 | 32 | Waveform, **Sync**, Rate, Fade |
| OSC 1 | 335 | 52 | Waveform, Control 1, Control 2 |
| Key Mode / Voice Assign | 549 | 7 | KeyMode, SplitPoint, VoiceAssign, **BendRange up/down** |
| OSC Common | 524 | 0 | Balance, X-Mod, Osc Shift, LFO1/Env Dest, **all three envelopes**, Mono/Legato/Velocity, Tone, Chorus/Delay |
| OSC 2 | 811 | 32 | Waveform, Control 1, Control 2, Range, Fine, Sync, Ring Mod |
| Filter | 1490 | 32 | Type, Slope, Cutoff, Resonance, Key Follow, **LFO1 Depth** |
| Ensemble / Arp | 1732 | 698 | Arp Switch, Hold, **Tempo**, Beat Pattern, Destination, Range |
| Amp | 2097 | 46 | Level, Pan, LFO1 Depth, **LFO2 Rate + Pitch/Filter/Amp LFO2 depths**, **Portamento**, Delay Type |
| Global Bottom Row | 175 | 1148 | MasterVolume, **MasterTune, RemoteControlChannel, MidiSync, RemoteKeyboardChannel** |

**Signal order across the panel is LFO 1 → OSC 1 → OSC COMMON → OSC 2 → FILTER
→ AMP**, with the arpeggiator lower-centre-right and a global strip along the
bottom.

## Where our pages disagree

Each of these is a deliberate future change, not a bug filed against today's
build. Today's order came from the parameter table; the panel is a better
source, and reordering is cheap because it is all in `gen_params.py`.

1. **LFO 1 should come first, not eighth.** It is the leftmost panel section.
   Our page order runs Main, Osc 1, Osc 2, Mix & Mod, Pitch, Filter, Amp,
   LFO 1 — panel order is LFO 1, Osc 1, Mix & Mod, Osc 2, Filter, Amp.

2. **Mix & Mod belongs BETWEEN Osc 1 and Osc 2**, because "OSC Common" is
   physically the strip between them. Ours sits after Osc 2.

3. **LFO 2 is a modulation strip, not a pair of knobs.** The panel groups
   `Lfo2Rate` with `PitchLfo2Depth`, `FilterLfo2Depth` and `AmpLfo2Depth` in
   the Amp area. We scatter those three across Pitch, Filter and Amp and leave
   LFO 2 as a two-cell page. Collecting them gives a five-control page that
   matches the hardware and removes our thinnest page.

4. **Tempo belongs with the arpeggiator.** The panel has it inside the Arp
   section; we have it on Performance → Setup.

5. **Bend Range belongs with Key Mode / Split / Voice Assign**, in the top
   strip. We have bend on Play and the rest on Performance → Setup.

6. **Portamento is an Amp-section control** on the panel; we have it on Play.

7. **LFO 1 Sync sits with LFO 1.** Ours is per-part (`up_lfo1_sync`,
   `lo_lfo1_sync`) on the Upper/Lower Part pages, which is where the sysex
   address puts it — worth showing in both places rather than moving.

## The system parameters have a panel home

The Global Bottom Row is exactly the system area: MasterTune,
RemoteControlChannel, MidiSync, RemoteKeyboardChannel (and MasterVolume, which
is the host's). So a **System** page is panel-faithful, not an invention.

`ui_hierarchy.modes` can carry a third entry — `page_plan.mjs` treats the mode
names AS level names and the mode selector is itself a pick-list, so a `system`
level of plain params works as a mode root without a preset browser. The
plumbing it needs: widen `mode` past 0..1 (`parse_mode` and its clamp),
teach `bank_view_for` that a mode may have no bank list, and add the system
parameters to the address table under `AREA_SYSTEM`. Writes there are verified
— see the arp note; `sysreq`/`sysparam` in `jp8000_render` read and write them.
