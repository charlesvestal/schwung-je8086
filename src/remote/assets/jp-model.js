/* JE-8086 Remote UI -- the model.
 *
 * Pure functions over the module's `temp` hex and the generated parameter
 * table (params.js). No DOM in here: tests/remote/model_test.mjs loads this
 * file in node, so anything that touches `document` belongs in app.js.
 *
 * The temp performance is 528 bytes, laid out exactly as state_get writes
 * them: common(36) partU(7) partL(7) patchU(239) patchL(239). A parameter's
 * `area` picks the block, `lin` the byte inside it; 14-bit values are two
 * 7-bit bytes, MSB first. The value the panel shows is `raw - off`, or `-raw`
 * for the one negated parameter (bend down). This mirrors param_read /
 * jp_value_to_raw in jp8000_plugin.cpp byte for byte.
 */
(function (root, factory) {
    var api = factory(root.JP8000);
    if (typeof module === "object" && module.exports) module.exports = api;
    root.JPModel = api;
})(typeof window !== "undefined" ? window : globalThis, function (JP) {
    "use strict";
    if (!JP) throw new Error("params.js must load before jp-model.js");

    var AREA = JP.areas;                       // {patch:0, part_up:1, part_lo:2, common:3, system:4}
    var TEMP_LEN = 528;
    var OFFSET = { common: 0, part_up: 36, part_lo: 43, patch_up: 50, patch_lo: 289 };

    var byKey = {};
    JP.params.forEach(function (p) { byKey[p.key] = p; });

    function hexToBytes(hex) {
        if (typeof hex !== "string") return null;
        var s = hex.trim();
        if (s.length !== TEMP_LEN * 2 && s.length % 2 !== 0) return null;
        var out = new Uint8Array(s.length / 2);
        for (var i = 0; i < out.length; i++) {
            var b = parseInt(s.substr(i * 2, 2), 16);
            if (isNaN(b)) return null;
            out[i] = b;
        }
        return out;
    }

    function bytesToHex(bytes) {
        var s = "";
        for (var i = 0; i < bytes.length; i++) s += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
        return s;
    }

    /* Byte offset of a parameter's block inside the temp image. `part` is the
     * edit part for patch parameters: 0 upper, 1 lower (2 = both reads upper). */
    function blockOffset(p, part) {
        switch (p.area) {
            case AREA.common:  return OFFSET.common;
            case AREA.part_up: return OFFSET.part_up;
            case AREA.part_lo: return OFFSET.part_lo;
            case AREA.patch:   return part === 1 ? OFFSET.patch_lo : OFFSET.patch_up;
            default:           return -1;   // system: not in temp
        }
    }

    function rawToValue(p, raw) { return p.neg ? -raw : raw - p.off; }
    function valueToRaw(p, value) {
        var raw = p.neg ? -value : value + p.off;
        var cap = p.bit14 ? 16383 : 127;
        return raw < 0 ? 0 : raw > cap ? cap : raw;
    }

    function readParam(bytes, p, part) {
        var o = blockOffset(p, part);
        if (o < 0 || !bytes) return null;
        var raw = bytes[o + p.lin];
        if (p.bit14) raw = (raw << 7) | bytes[o + p.lin + 1];
        return rawToValue(p, raw);
    }

    /* Write into a copy of the image the way the plugin writes its own image
     * optimistically: the panel shows the edit at once and the firmware's
     * dump, when it lands, either agrees or corrects it. */
    function writeParam(bytes, p, part, value) {
        var o = blockOffset(p, part);
        if (o < 0) return;
        var raw = valueToRaw(p, clamp(p, value));
        if (p.bit14) { bytes[o + p.lin] = (raw >> 7) & 0x7f; bytes[o + p.lin + 1] = raw & 0x7f; }
        else bytes[o + p.lin] = raw & 0x7f;
    }

    function clamp(p, v) {
        v = Math.round(Number(v));
        if (isNaN(v)) v = p.default;
        return v < p.min ? p.min : v > p.max ? p.max : v;
    }

    /* Every temp-resident value for one edit part, keyed by parameter. */
    function decodeAll(bytes, part) {
        var out = {};
        if (!bytes || bytes.length < TEMP_LEN) return out;
        JP.params.forEach(function (p) {
            if (p.area === AREA.system) return;
            out[p.key] = readParam(bytes, p, part);
        });
        return out;
    }

    /* "sys" is one byte per exposed system parameter, in table order. A blob
     * whose length disagrees with the table is refused, as the plugin does. */
    function decodeSys(hex) {
        var out = {};
        var bytes = hexToBytes(hex || "");
        if (!bytes || bytes.length !== JP.sys_order.length) return out;
        JP.sys_order.forEach(function (k, i) { out[k] = rawToValue(byKey[k], bytes[i]); });
        return out;
    }

    /* The 16-character name at the head of a patch or the performance common
     * block, trimmed, as the firmware's LCD would spell it. */
    function nameAt(bytes, offset) {
        if (!bytes) return "";
        var s = "";
        for (var i = 0; i < 16; i++) {
            var c = bytes[offset + i];
            s += (c >= 0x20 && c < 0x7f) ? String.fromCharCode(c) : " ";
        }
        return s.replace(/\s+$/, "");
    }
    function patchName(bytes, part) { return nameAt(bytes, part === 1 ? OFFSET.patch_lo : OFFSET.patch_up); }
    function performanceName(bytes) { return nameAt(bytes, OFFSET.common); }

    /* A fresh image at the table defaults, for a page that has no device yet. */
    function defaultImage() {
        var bytes = new Uint8Array(TEMP_LEN);
        JP.params.forEach(function (p) {
            if (p.area === AREA.system) return;
            if (p.area === AREA.patch) { writeParam(bytes, p, 0, p.default); writeParam(bytes, p, 1, p.default); }
            else writeParam(bytes, p, 0, p.default);
        });
        return bytes;
    }

    /* ---- display ------------------------------------------------------------- */
    var NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    function noteName(n) { return NOTE[n % 12] + (Math.floor(n / 12) - 1); }

    function valueText(p, v) {
        if (v === null || v === undefined) return "--";
        if (p.options) return p.options[v] !== undefined ? p.options[v] : String(v);
        switch (p.key) {
            case "split_point": return noteName(v);
            case "trigger_note": return v >= 128 ? "ALL" : noteName(v);
            case "sys_master_tune": return (v - 50 === 0 ? "" : (v > 50 ? "+" : "")) + (v - 50) + " (" + tuneHz(v) + " Hz)";
        }
        var s = (p.min < 0 && v > 0 ? "+" : "") + v;
        if (p.unit) s += " " + p.unit;
        return s;
    }
    /* Master Tune 0..100 spans 427.4..452.6 Hz around A440, per the manual. */
    function tuneHz(v) { return (427.4 + (452.6 - 427.4) * v / 100).toFixed(1); }

    /* ---- the panel ------------------------------------------------------------
     *
     * The JP-8000 front panel, section by section, in the order the Owner's
     * Manual numbers them (docs/PANEL_LAYOUT.md). Every temp-resident parameter
     * that is not a ribbon/velocity depth appears exactly once here; the test
     * asserts it. Widgets: knob, slider (the panel's envelope faders), leds
     * (a button per option with an LED), switch (two options), select (long
     * lists the panel reaches through the LCD), readout (display only).
     */
    var K = function (k, w, opts) { var o = { key: k, widget: w }; if (opts) for (var x in opts) o[x] = opts[x]; return o; };

    var PANEL = [
        { id: "row-top", sections: [
            { id: "lfo1", title: "LFO 1", num: 1, viz: "lfo1", items: [
                K("lfo1_waveform", "leds", { label: "Waveform" }),
                K("lfo1_rate", "knob", { label: "Rate" }),
                K("lfo1_fade", "knob", { label: "Fade" }),
                K("lfo1_env_dest", "leds", { label: "LFO1 & Env Dest" }),
            ]},
            { id: "osccommon", title: "OSC Common", num: 2, viz: "penv", items: [
                K("ring_mod", "switch", { label: "Ring" }),
                K("cross_mod_depth", "knob", { label: "X-Mod Depth" }),
                K("osc_balance", "knob", { label: "Osc Balance" }),
                K("osc_lfo1_depth", "knob", { label: "LFO 1 Depth" }),
                K("pitch_env_depth", "knob", { label: "Env Depth" }),
                K("pitch_env_attack", "knob", { label: "Env A" }),
                K("pitch_env_decay", "knob", { label: "Env D" }),
            ]},
            { id: "arp", title: "Arpeggiator / RPS", num: 11, viz: "arp", items: [
                K("arp_switch", "switch", { label: "Arp" }),
                K("arp_hold", "switch", { label: "Hold" }),
                K("tempo", "knob", { label: "Tempo" }),
                K("arp_mode", "leds", { label: "Mode" }),
                K("arp_range", "leds", { label: "Range" }),
                K("arp_beat", "select", { label: "Beat Pattern" }),
                K("arp_dest", "leds", { label: "Destination" }),
            ]},
        ]},
        { id: "row-main", sections: [
            { id: "osc1", title: "OSC 1", num: 3, viz: "osc1", items: [
                K("osc1_waveform", "leds", { label: "Waveform" }),
                K("osc1_ctrl1", "knob", { label: "Control 1" }),
                K("osc1_ctrl2", "knob", { label: "Control 2" }),
            ]},
            { id: "osc2", title: "OSC 2", num: 4, viz: "osc2", items: [
                K("osc2_waveform", "leds", { label: "Waveform" }),
                K("osc2_sync", "switch", { label: "Sync" }),
                K("osc2_range", "knob", { label: "Range" }),
                K("osc2_fine", "knob", { label: "Fine / Wide" }),
                K("osc2_ctrl1", "knob", { label: "Control 1" }),
                K("osc2_ctrl2", "knob", { label: "Control 2" }),
            ]},
            { id: "filter", title: "Filter", num: 5, viz: "filter", items: [
                K("filter_type", "leds", { label: "Type" }),
                K("cutoff_slope", "leds", { label: "Slope" }),
                K("cutoff", "knob", { label: "Cutoff", big: true }),
                K("resonance", "knob", { label: "Resonance" }),
                K("key_follow", "knob", { label: "Key Follow" }),
                K("filter_lfo1_depth", "knob", { label: "LFO 1 Depth" }),
                K("filter_env_depth", "knob", { label: "Env Depth" }),
                K("filter_attack", "slider", { label: "A", group: "env" }),
                K("filter_decay", "slider", { label: "D", group: "env" }),
                K("filter_sustain", "slider", { label: "S", group: "env" }),
                K("filter_release", "slider", { label: "R", group: "env" }),
            ]},
            { id: "amp", title: "Amp", num: 6, viz: "aenv", items: [
                K("amp_lfo1_depth", "knob", { label: "LFO 1 Depth" }),
                K("amp_lfo1_mode", "leds", { label: "Pan" }),
                K("amp_level", "knob", { label: "Level", big: true }),
                K("amp_attack", "slider", { label: "A", group: "env" }),
                K("amp_decay", "slider", { label: "D", group: "env" }),
                K("amp_sustain", "slider", { label: "S", group: "env" }),
                K("amp_release", "slider", { label: "R", group: "env" }),
            ]},
        ]},
        { id: "row-lower", sections: [
            { id: "keypanel", title: "Key & Panel", num: 13, viz: "keys", items: [
                K("key_mode", "leds", { label: "Key Mode" }),
                K("split_point", "knob", { label: "Split Point" }),
                K("part_detune", "knob", { label: "Part Detune" }),
                K("voice_assign", "leds", { label: "Voice Assign" }),
                K("output_assign", "switch", { label: "Output" }),
            ]},
            { id: "tone", title: "Tone Control", num: 7, viz: "tone", items: [
                K("tone_bass", "knob", { label: "Bass" }),
                K("tone_treble", "knob", { label: "Treble" }),
            ]},
            { id: "chorus", title: "Chorus", num: 8, viz: "chorus", items: [
                K("chorus_level", "knob", { label: "Level" }),
                K("chorus_type", "select", { label: "Type" }),
            ]},
            { id: "delay", title: "Delay", num: 9, viz: "delay", items: [
                K("delay_time", "knob", { label: "Time" }),
                K("delay_feedback", "knob", { label: "Feedback" }),
                K("delay_level", "knob", { label: "Level" }),
                K("delay_type", "select", { label: "Type" }),
            ]},
        ]},
        { id: "row-left", sections: [
            { id: "controller", title: "Controller", num: 20, viz: "bend", items: [
                K("bend_up", "knob", { label: "Bend Up" }),
                K("bend_down", "knob", { label: "Bend Down" }),
                K("morph_bend", "switch", { label: "Morph Bend" }),
                K("active_bender", "switch", { label: "Bender" }),
                K("active_control", "switch", { label: "Ribbon" }),
                K("active_velocity", "switch", { label: "Velocity" }),
                { widget: "drawer", id: "ribbon", label: "Ribbon Assign", prefix: "ctl_" },
                { widget: "drawer", id: "velocity", label: "Velocity Assign", prefix: "vel_" },
            ]},
            { id: "lfo2", title: "LFO 2", num: 21, viz: "lfo2", items: [
                K("lfo2_rate", "knob", { label: "Rate" }),
                K("lfo2_depth_select", "leds", { label: "Depth Select" }),
                /* ONE depth knob and a selector on the panel; three bytes in the
                 * sysex. The knob follows the selector; the other two show. */
                { widget: "lfo2depth", keys: ["pitch_lfo2_depth", "filter_lfo2_depth", "amp_lfo2_depth"], label: "Depth" },
            ]},
            { id: "keyboard", title: "Keyboard", num: 22, viz: "porta", items: [
                K("portamento", "switch", { label: "Portamento" }),
                K("portamento_time", "knob", { label: "Porta Time" }),
                K("velocity_switch", "switch", { label: "Velocity" }),
                K("mono", "switch", { label: "Mono" }),
                K("legato", "switch", { label: "Legato" }),
                K("osc_shift", "leds", { label: "Osc Shift" }),
            ]},
        ]},
        { id: "row-parts", sections: [
            { id: "parts", title: "Parts", viz: "parts", parts: true, items: [
                K("midi_ch", "select", { label: "MIDI Ch", perPart: true }),
                K("transpose", "knob", { label: "Transpose", perPart: true }),
                K("delay_sync", "select", { label: "Delay Sync", perPart: true }),
                K("lfo1_sync", "select", { label: "LFO 1 Sync", perPart: true }),
                K("chorus_sync", "select", { label: "Chorus Sync", perPart: true }),
            ]},
            { id: "trigger", title: "Ext Trigger", viz: "trig", items: [
                K("trigger_switch", "switch", { label: "Trigger" }),
                K("trigger_dest", "leds", { label: "Destination" }),
                K("trigger_ch", "knob", { label: "Channel" }),
                K("trigger_note", "knob", { label: "Note" }),
            ]},
        ]},
    ];

    /* The System area: what you reach with SHIFT/EXIT, not a page of the
     * performance. Plus the one plugin-side setting a slot has (audio buffer). */
    var SYSTEM = [
        { title: "MIDI", keys: ["sys_remote_ch", "sys_perf_ctrl_ch", "sys_midi_sync", "sys_txrx_pc", "sys_txrx_edit", "sys_txrx_edit_mode"] },
        { title: "Tuning & Gate", keys: ["sys_master_tune", "sys_gate_ratio"] },
        { title: "Ribbon", keys: ["sys_ribbon_rel", "sys_ribbon_hold"] },
        { title: "This Slot", keys: ["buffer_ms"] },
    ];

    /* The ribbon and velocity families: forty depths each, one per destination,
     * grouped by the panel section the destination lives in. */
    var DRAWER_GROUPS = [
        ["Filter", ["cutoff_frequency", "resonance", "filter_env_depth", "filter_env_attack_time", "filter_env_decay_time", "filter_env_sustain_level", "filter_env_sus_level", "filter_env_release_time", "cutoff_freq_key_follow", "filter_lfo1_depth", "filter_lfo2_depth"]],
        ["Amp", ["amp_level", "amp_env_attack_time", "amp_env_decay_time", "amp_env_sustain_level", "amp_env_release_time", "amp_lfo1_depth", "amp_lfo2_depth"]],
        ["Pitch", ["pitch_lfo1_depth", "pitch_lfo2_depth", "pitch_envelope_depth", "pitch_envelope_attack_time", "pitch_envelope_decay_time", "portamento_time"]],
        ["Oscillators", ["osc1_control1", "osc1_control2", "osc2_range", "osc2_fine_wide", "osc2_control1", "osc2_control2", "oscillator_balance", "cross_modulation_depth"]],
        ["LFO", ["lfo1_rate", "lfo1_fade", "lfo2_rate"]],
        ["Tone & FX", ["tone_control_bass", "tone_control_treble", "multi_effects_level", "delay_time", "delay_feedback", "delay_level"]],
    ];
    function drawerGroups(prefix) {
        var have = JP.params.filter(function (p) { return p.key.indexOf(prefix) === 0; });
        var used = {};
        var groups = DRAWER_GROUPS.map(function (g) {
            var keys = [];
            g[1].forEach(function (suffix) {
                var k = prefix + suffix;
                if (byKey[k]) { keys.push(k); used[k] = true; }
            });
            return { title: g[0], keys: keys };
        }).filter(function (g) { return g.keys.length; });
        var rest = have.filter(function (p) { return !used[p.key]; }).map(function (p) { return p.key; });
        if (rest.length) groups.push({ title: "Other", keys: rest });
        return groups;
    }

    /* The tabs. `panel` shows every section in the hardware's layout; the
     * others are the same sections, fewer at a time and with more room. Every
     * section belongs to exactly one focused tab (the test asserts it). */
    var TABS = [
        { id: "presets", label: "Presets" },
        { id: "panel", label: "Panel", all: true },
        { id: "osc", label: "Osc", sections: ["osc1", "osc2", "osccommon"] },
        { id: "filteramp", label: "Filter & Amp", sections: ["filter", "amp"] },
        { id: "lfo", label: "LFO & Mod", sections: ["lfo1", "lfo2", "controller"] },
        { id: "fx", label: "FX", sections: ["tone", "chorus", "delay"] },
        { id: "play", label: "Play & Arp", sections: ["keypanel", "keyboard", "arp", "parts", "trigger"] },
        { id: "system", label: "System" },
    ];

    /* Resolve a per-part item to its real key. */
    function partKey(item, part) { return (part === 1 ? "lo_" : "up_") + item.key; }

    /* Which real parameter keys the panel covers (for the coverage test). */
    function panelKeys() {
        var keys = [];
        PANEL.forEach(function (row) {
            row.sections.forEach(function (s) {
                s.items.forEach(function (it) {
                    if (it.widget === "drawer") {
                        drawerGroups(it.prefix).forEach(function (g) { keys.push.apply(keys, g.keys); });
                    } else if (it.widget === "lfo2depth") {
                        keys.push.apply(keys, it.keys);
                    } else if (it.perPart) {
                        keys.push(partKey(it, 0), partKey(it, 1));
                    } else keys.push(it.key);
                });
            });
        });
        SYSTEM.forEach(function (g) { keys.push.apply(keys, g.keys); });
        return keys;
    }

    return {
        JP: JP, byKey: byKey, AREA: AREA, OFFSET: OFFSET, TEMP_LEN: TEMP_LEN,
        hexToBytes: hexToBytes, bytesToHex: bytesToHex,
        readParam: readParam, writeParam: writeParam, clamp: clamp,
        decodeAll: decodeAll, decodeSys: decodeSys, defaultImage: defaultImage,
        patchName: patchName, performanceName: performanceName,
        valueText: valueText, noteName: noteName,
        PANEL: PANEL, SYSTEM: SYSTEM, TABS: TABS, drawerGroups: drawerGroups, partKey: partKey, panelKeys: panelKeys,
    };
});
