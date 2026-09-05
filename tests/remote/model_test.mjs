// Model tests for the Remote UI: the decoder agrees with the plugin's byte
// layout, every parameter has exactly one home on the panel, and an edit
// round-trips through the image.
//
//   node tests/remote/model_test.mjs
import fs from "node:fs";
import vm from "node:vm";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const assets = path.join(here, "../../src/remote/assets");
const ctx = {};
ctx.window = ctx;
ctx.globalThis = ctx;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(assets, "params.js"), "utf8"), ctx);
vm.runInContext(fs.readFileSync(path.join(assets, "jp-model.js"), "utf8"), ctx);
const M = ctx.JPModel;

let failures = 0;
function check(cond, msg) { if (!cond) { failures++; console.log("FAIL", msg); } }

// 1. Coverage: every temp-resident and system parameter appears exactly once.
const seen = {};
for (const k of M.panelKeys()) seen[k] = (seen[k] || 0) + 1;
for (const p of M.JP.params) {
    if (p.key === "panel_select") continue;        // driven by Edit Part, not a cell
    check(seen[p.key] === 1, `${p.key} appears ${seen[p.key] || 0} times on the panel`);
}
for (const k of Object.keys(seen)) check(M.byKey[k] || k === "buffer_ms", `panel names unknown key ${k}`);

// 1b. Every panel section belongs to exactly one focused tab.
const secTabs = {};
for (const t of M.TABS) for (const sid of (t.sections || [])) secTabs[sid] = (secTabs[sid] || 0) + 1;
for (const row of M.PANEL) for (const sec of row.sections) check(secTabs[sec.id] === 1, `section ${sec.id} is on ${secTabs[sec.id] || 0} tabs`);
for (const sid of Object.keys(secTabs)) check(M.PANEL.some(r => r.sections.some(s => s.id === sid)), `tab names unknown section ${sid}`);

// 2. Layout constants match the plugin (jp8000_plugin.cpp JP_TEMP_* / state_img order).
check(M.OFFSET.common === 0 && M.OFFSET.part_up === 36 && M.OFFSET.part_lo === 43 &&
      M.OFFSET.patch_up === 50 && M.OFFSET.patch_lo === 289 && M.TEMP_LEN === 528, "temp layout offsets");

// 3. Round trip: write every parameter at min, max and default; read it back.
const img = M.defaultImage();
for (const p of M.JP.params) {
    if (p.area === M.AREA.system) continue;
    for (const v of [p.min, p.max, p.default]) {
        for (const part of (p.area === M.AREA.patch ? [0, 1] : [0])) {
            M.writeParam(img, p, part, v);
            const back = M.readParam(img, p, part);
            check(back === v, `${p.key} part ${part}: wrote ${v} read ${back}`);
        }
    }
}
// A patch write to one part leaves the other untouched.
const cut = M.byKey.cutoff;
M.writeParam(img, cut, 0, 10); M.writeParam(img, cut, 1, 100);
check(M.readParam(img, cut, 0) === 10 && M.readParam(img, cut, 1) === 100, "parts are independent");

// 4. Hex round trip and a known byte: the bend-down parameter is negated.
const hex = M.bytesToHex(img);
check(hex.length === 1056, "hex length");
const again = M.hexToBytes(hex);
check(again.every((b, i) => b === img[i]), "hex round trip");
const bd = M.byKey.bend_down;
M.writeParam(img, bd, 0, -12);
check(img[M.OFFSET.patch_up + bd.lin] === 12, "bend_down stores the magnitude");
check(M.readParam(img, bd, 0) === -12, "bend_down reads back negative");

// 5. 14-bit depths straddle two bytes MSB first.
const ctl = M.JP.params.find(p => p.bit14);
M.writeParam(img, ctl, 0, ctl.max);
const raw = ctl.max + ctl.off;
check(img[M.OFFSET.patch_up + ctl.lin] === (raw >> 7) && img[M.OFFSET.patch_up + ctl.lin + 1] === (raw & 0x7f), "14-bit split");

// 6. Names come out of the image trimmed.
const nm = "SuperSaw Ld     ";
for (let i = 0; i < 16; i++) img[M.OFFSET.patch_up + i] = nm.charCodeAt(i);
check(M.patchName(img, 0) === "SuperSaw Ld", `patch name "${M.patchName(img, 0)}"`);

// 7. System decode refuses a blob of the wrong length, accepts the right one.
check(Object.keys(M.decodeSys("00")).length === 0, "short sys blob refused");
const sysHex = M.JP.sys_order.map(k => (M.byKey[k].default + M.byKey[k].off).toString(16).padStart(2, "0")).join("");
const sys = M.decodeSys(sysHex);
check(sys.sys_master_tune === 50 && sys.sys_remote_ch === 2, "sys decode");

// 8. Display text.
check(M.valueText(M.byKey.split_point, 60) === "C4", "split point as note");
check(M.valueText(M.byKey.filter_type, 2) === "LPF", "enum text");
check(M.valueText(M.byKey.osc_balance, 5) === "+5", "bipolar sign");
check(M.valueText(M.byKey.tempo, 120) === "120 BPM", "unit");

console.log(failures ? `${failures} failure(s)` : `ok: ${M.JP.params.length} params, ${M.panelKeys().length} panel cells`);
process.exit(failures ? 1 : 0);
