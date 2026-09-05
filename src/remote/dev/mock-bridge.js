/* A stand-in for Schwung Manager's schwungRemote bridge, for looking at the
 * page with no device. It behaves the way the manager does from the page's
 * side: a full `temp` dump arrives after subscribe, a write is echoed back in
 * a re-read ~250 ms later, and selecting a preset replaces the whole image.
 * Preset names and values here are EXAMPLES, not factory data.
 *
 * Loaded only by the preview build (src/tools/remote_preview.py); never shipped.
 */
(function () {
    "use strict";
    var JP = window.JP8000;
    var byKey = {}; JP.params.forEach(function (p) { byKey[p.key] = p; });
    var OFF = { common: 0, part_up: 36, part_lo: 43, patch_up: 50, patch_lo: 289 };
    function blockOffset(p, part) { return p.area === 3 ? 0 : p.area === 1 ? 36 : p.area === 2 ? 43 : p.area === 0 ? (part ? 289 : 50) : -1; }
    function put(img, key, part, value) {
        var p = byKey[key], o = blockOffset(p, part); if (o < 0) return;
        var raw = p.neg ? -value : value + p.off; raw = Math.max(0, Math.min(p.bit14 ? 16383 : 127, raw));
        if (p.bit14) { img[o + p.lin] = raw >> 7; img[o + p.lin + 1] = raw & 127; } else img[o + p.lin] = raw;
    }
    function name(img, off, s) { for (var i = 0; i < 16; i++) img[off + i] = i < s.length ? s.charCodeAt(i) : 32; }
    function hex(img) { var s = ""; for (var i = 0; i < img.length; i++) s += (img[i] < 16 ? "0" : "") + img[i].toString(16); return s; }

    function fresh() {
        var img = new Uint8Array(528);
        JP.params.forEach(function (p) { if (p.area === 4) return; put(img, p.key, 0, p.default); if (p.area === 0) put(img, p.key, 1, p.default); });
        return img;
    }
    /* Example patches: a few settings each so the panel has something to show. */
    var PATCHES = [
        ["Trance Saw Lead", { osc1_waveform: 0, osc1_ctrl1: 78, osc1_ctrl2: 96, osc2_waveform: 1, osc2_range: -12, osc_balance: -20, cutoff: 92, resonance: 36, filter_env_depth: 24, filter_attack: 4, filter_decay: 70, filter_sustain: 40, filter_release: 56, amp_attack: 2, amp_decay: 60, amp_sustain: 110, amp_release: 48, chorus_type: 0, chorus_level: 64, delay_type: 0, delay_time: 88, delay_feedback: 40, delay_level: 36, lfo1_rate: 58 }],
        ["Warm Pad", { osc1_waveform: 6, osc2_waveform: 2, osc2_fine: 8, osc_balance: 0, cutoff: 60, resonance: 18, filter_type: 2, cutoff_slope: 0, filter_attack: 70, filter_decay: 90, filter_sustain: 90, filter_release: 100, amp_attack: 72, amp_decay: 64, amp_sustain: 127, amp_release: 104, chorus_type: 3, chorus_level: 90, lfo1_waveform: 0, lfo1_rate: 30, filter_lfo1_depth: 12, amp_lfo1_mode: 1 }],
        ["Feedback Bass", { osc1_waveform: 3, osc1_ctrl1: 40, osc1_ctrl2: 20, osc2_waveform: 0, osc2_range: -12, cutoff: 44, resonance: 62, cutoff_slope: 1, filter_env_depth: 40, filter_decay: 34, filter_sustain: 0, amp_decay: 90, amp_sustain: 96, amp_release: 12, mono: 1, legato: 1, portamento: 1, portamento_time: 22 }],
        ["PWM Strings", { osc1_waveform: 4, osc1_ctrl1: 64, osc1_ctrl2: 70, osc2_waveform: 0, osc2_ctrl1: 40, osc2_ctrl2: 60, cutoff: 78, resonance: 8, filter_attack: 40, amp_attack: 50, amp_release: 90, chorus_type: 4, chorus_level: 70, lfo1_rate: 44 }],
        ["Ring Bell", { ring_mod: 1, osc1_waveform: 6, osc2_waveform: 2, osc2_range: 19, cutoff: 110, filter_decay: 50, filter_sustain: 0, amp_attack: 0, amp_decay: 72, amp_sustain: 0, amp_release: 80, delay_type: 2, delay_level: 50 }],
        ["Noise Sweep", { osc1_waveform: 2, osc_balance: -64, filter_type: 1, cutoff: 30, resonance: 90, filter_env_depth: 63, filter_attack: 100, filter_decay: 120, amp_attack: 60, amp_release: 100 }],
        ["Sync Lead", { osc1_waveform: 5, osc2_waveform: 1, osc2_sync: 1, osc2_range: 7, osc2_ctrl1: 30, cutoff: 100, resonance: 20, amp_sustain: 120, bend_up: 12, bend_down: -12 }],
        ["Super Saw Init", {}],
    ];
    /* Fill the factory banks to 64 so the bank/number matrix reads as it does on
     * the hardware. Names are EXAMPLES in the usual tagged style (LD lead, PD pad,
     * BS bass, SQ sequence, FX, SY synth, ST strings, KY keys, AR arp). */
    var TAGS = ["LD", "PD", "BS", "SQ", "FX", "SY", "ST", "KY", "AR", "OR"];
    var WORDS = ["Sandstorm", "Airwave", "Eternal", "Cafe", "Silence", "Uplift", "Anthem", "Voyager", "Prism", "Halo", "Nebula", "Drift", "Pulse", "Rapture", "Sunrise", "Outland", "Orbit", "Delta", "Mirage", "Tide", "Zenith", "Aurora", "Ember", "Glass", "Vapor", "Cirrus", "Marble", "Velvet", "Static", "Comet", "Ion", "Saffron"];
    function fillBank(seed) {
        var out = PATCHES.map(function (p) { return p[0]; });
        for (var i = out.length; i < 64; i++) {
            var t = TAGS[(i * 7 + seed) % TAGS.length], w = WORDS[(i * 5 + seed * 3) % WORDS.length];
            out.push(t + " " + w + (i % 3 === 0 ? " " + ((i % 7) + 1) : ""));
        }
        return out;
    }
    var BANK_A = fillBank(0), BANK_B = fillBank(5);
    var PERFS = [["Sweepers", 0, 1], ["Chariots", 1, 4], ["Split Bass/Lead", 2, 0], ["Dual Pads", 1, 3], ["PD Nightfall", 3, 1], ["LD Anthem Stack", 0, 6], ["BS Sub Layer", 2, 2], ["AR Cascade", 4, 5]];

    var image = fresh(), mode = 0, part = 0, bank = 0, patch = 0, perf_bank = 0, performance = 0, buffer_ms = 10;
    var sys = { sys_perf_ctrl_ch: 16, sys_midi_sync: 1, sys_txrx_edit_mode: 1, sys_txrx_edit: 1, sys_txrx_pc: 2, sys_remote_ch: 2, sys_master_tune: 50, sys_gate_ratio: 0, sys_ribbon_rel: 0, sys_ribbon_hold: 0 };

    function loadPatch(i, toPart) {
        var def = PATCHES[i % PATCHES.length];
        [0, 1].forEach(function (pt) {
            if (toPart !== 2 && toPart !== pt) return;
            var tmp = fresh();
            for (var k in def[1]) put(tmp, k, pt, def[1][k]);
            var o = pt ? OFF.patch_lo : OFF.patch_up;
            for (var b = 0; b < 239; b++) image[o + b] = tmp[o + b];
            name(image, o, def[0]);
        });
    }
    function loadPerf(i) {
        var pf = PERFS[i % PERFS.length];
        image = fresh();
        loadPatch(pf[1], 0); loadPatch(pf[2], 1);
        name(image, OFF.common, pf[0]);
        put(image, "key_mode", 0, i % 3); put(image, "split_point", 0, 60); put(image, "arp_switch", 0, i === 1 ? 1 : 0);
        put(image, "tempo", 0, 138); put(image, "lo_transpose", 0, -12);
    }
    loadPerf(0); loadPatch(0, 0);

    var listeners = [];
    function stateParams() {
        var sysHex = JP.sys_order.map(function (k) { var b = sys[k] + byKey[k].off; return (b < 16 ? "0" : "") + b.toString(16); }).join("");
        var perf = mode === 1;
        var names = perf ? PERFS.map(function (p) { return p[0]; }) : PATCHES.map(function (p) { return p[0]; });
        return {
            "synth:version": "2", "synth:mode": String(mode), "synth:part": String(part), "synth:bank": String(bank), "synth:patch": String(patch),
            "synth:perf_bank": String(perf_bank), "synth:performance": String(performance), "synth:buffer_ms": String(buffer_ms),
            "synth:temp": hex(image), "synth:sys": sysHex,
            "synth:patch_count": String(BANK_A.length), "synth:performance_count": String(PERFS.length),
            "synth:patch_name": BANK_A[patch % BANK_A.length], "synth:performance_name": PERFS[performance % PERFS.length][0],
            "synth:bank_list_name": perf ? "Factory" : (bank ? "Factory B" : "Factory A"),
        };
    }
    function emit(params) { listeners.forEach(function (f) { try { f(params); } catch (e) { console.error(e); } }); }
    var refetch = null;
    function scheduleRefetch() { clearTimeout(refetch); refetch = setTimeout(function () { emit(stateParams()); }, 250); }

    window.schwungRemote = {
        component: "synth",
        onParamChange: function (f) { listeners.push(f); setTimeout(function () { f(stateParams()); }, 350); },
        getParam: function (k) { return Promise.resolve(stateParams()[k]); },
        getHierarchy: function () { return Promise.resolve(JP.hierarchy); },
        getChainParams: function () {
            return Promise.resolve((JP.controls || []).concat([
                { key: "bank", name: "Patch Bank", type: "enum", options: ["Factory A", "Factory B", "The Usual Suspects.syx"] },
                { key: "perf_bank", name: "Perf Bank", type: "enum", options: ["Factory", "Trance Kit.pfm"] }]));
        },
        resubscribe: function () { scheduleRefetch(); },
        setParam: function (full, value) {
            var key = full.replace(/^synth:/, ""); value = String(value);
            switch (key) {
                case "mode": mode = value === "Patch" || value === "0" ? 0 : value === "System" || value === "2" ? 2 : 1; break;
                case "part": part = parseInt(value, 10) || 0; put(image, "panel_select", 0, part); break;
                case "bank": bank = parseInt(value, 10) || 0; patch = 0; loadPatch(0, part); break;
                case "perf_bank": perf_bank = parseInt(value, 10) || 0; performance = 0; loadPerf(0); break;
                case "patch": patch = parseInt(value, 10) || 0; loadPatch(patch, part); break;
                case "performance": performance = parseInt(value, 10) || 0; loadPerf(performance); break;
                case "buffer_ms": buffer_ms = parseInt(value, 10) || 10; break;
                case "temp_refresh": break;   // a re-read request: nothing changes, the dump is re-pushed
                default: {
                    var p = byKey[key]; if (!p) return;
                    var v = parseInt(value, 10);
                    if (p.area === 4) sys[key] = v;
                    else if (p.area === 0) { if (part !== 1) put(image, key, 0, v); if (part !== 0) put(image, key, 1, v); }
                    else put(image, key, 0, v);
                }
            }
            scheduleRefetch();
        }
    };
    /* The catalog file the real module writes; served here from memory. */
    var realFetch = window.fetch;
    window.fetch = function (url, opts) {
        if (String(url).indexOf("banks_index.json") >= 0) {
            var body = JSON.stringify({ version: 1, skipped: 2,
                patch: [{ name: "Factory A", folder: "", presets: BANK_A },
                        { name: "Factory B", folder: "", presets: BANK_B },
                        { name: "The Usual Suspects.syx", folder: "jp8000", presets: ["Eternal", "Adagio Bass", "LD Sandstorm", "Airwave", "Cafe del Mar", "Silence", "PD Adagio", "LD Nine Lives", "BS Sub Zero", "SQ Rolling", "FX Riser", "LD Choir Lead"] },
                        { name: "! 2 files ignored", presets: [] }],
                performance: [{ name: "Factory", folder: "", presets: PERFS.map(function (p) { return p[0]; }) },
                              { name: "AGGRLD", folder: "jp8000-rp", presets: [""] },
                              { name: "BITSTRS", folder: "jp8000-rp", presets: [""] },
                              { name: "DARKCLAV", folder: "jp8000-rp", presets: [""] },
                              { name: "CSBRSS98", folder: "GZJP8", presets: ["CS Brass '98  GZ"] },
                              { name: "JP8POWER", folder: "GZJP8", presets: ["JP Power Lead GZ"] },
                              { name: "Trance Kit.pfm", folder: "jp8000", presets: ["Anthem 1", "Anthem 2", "Uplift", "Hands Up"] },
                              { name: "! 2 files ignored", presets: [] }] });
            return Promise.resolve(new Response(body, { status: 200, headers: { "Content-Type": "application/json" } }));
        }
        return realFetch.apply(this, arguments);
    };
})();
