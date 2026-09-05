/* JE-8086 Remote UI -- the page.
 *
 * Talks to the slot through the `schwungRemote` bridge Schwung Manager injects
 * into the iframe (see docs/REMOTE_UI.md for the contract). Every value on
 * this panel is decoded from the module's `temp` hex -- the whole 528-byte
 * temp performance, as state_get emits it -- so the panel can never show a
 * value the firmware does not hold. Edits are written into a local copy of
 * that image at once (as the plugin does with its own image) and confirmed
 * when the manager pushes the next dump.
 */
(function () {
    "use strict";
    var M = window.JPModel, JP = M.JP, byKey = M.byKey;
    var remote = window.schwungRemote;
    var comp = (remote && remote.component) || "synth";
    var PFX = comp + ":";
    var query = new URLSearchParams(location.search);
    var MODULE_ID = query.get("module") || "jp8000";

    /* ---- state --------------------------------------------------------------- */
    var S = {
        temp: null,           // Uint8Array(528) or null before the first dump
        sys: {},              // system values from the "sys" field
        mode: 1,              // 0 patch, 1 performance, 2 system (the UI fact the module keeps)
        part: 0,              // Edit Part: 0 upper, 1 lower, 2 both
        bank: 0, patch: 0, perf_bank: 0, performance: 0,
        buffer_ms: null,
        names: {},            // hierarchy params the manager pushes: patch_name, patch_count, ...
        catalog: null,        // banks_index.json, written by the module's child at boot
        chain: null,          // chain_params (bank enums as a fallback for bank names)
        connected: false,
        pending: {},          // key -> timestamp of an unconfirmed edit
        lcdUntil: 0, lcdText: null,
    };
    var T = { cur: (function () { try { return localStorage.getItem("je8086.tab") || "panel"; } catch (e) { return "panel"; } })() };
    var W = {};                // key -> widget {set(value, ghost), el}
    var vizByGroup = {};       // viz id -> draw()

    function editPart() { return S.part === 1 ? 1 : 0; }   // which patch the panel reads
    function otherPart() { return S.part === 1 ? 0 : 1; }

    /* ---- outbound ------------------------------------------------------------ */
    var throttles = {};
    function setParam(key, value) {
        if (!remote) return;
        var now = Date.now();
        var t = throttles[key];
        if (t && now - t.last < 33) {           // knob drags: ~30 Hz, trailing edge kept
            t.value = value;
            if (!t.timer) t.timer = setTimeout(function () { t.timer = null; t.last = Date.now(); remote.setParam(PFX + key, t.value); }, 33 - (now - t.last));
            return;
        }
        throttles[key] = { last: now, timer: null, value: value };
        remote.setParam(PFX + key, value);
    }

    /* A parameter edit from the panel: local image first, then the device. */
    function edit(key, value) {
        var p = byKey[key];
        if (!p) return;
        value = M.clamp(p, value);
        if (p.area === M.AREA.system) {
            S.sys[key] = value;
        } else if (S.temp) {
            if (p.area === M.AREA.patch) {
                if (S.part === 0 || S.part === 2) M.writeParam(S.temp, p, 0, value);
                if (S.part === 1 || S.part === 2) M.writeParam(S.temp, p, 1, value);
            } else M.writeParam(S.temp, p, 0, value);
        }
        S.pending[key] = Date.now();
        setParam(key, value);
        refreshKey(key);
        lcdShow(p, value);
        redrawViz([key]);
    }

    /* ---- inbound ------------------------------------------------------------- */
    /* ASKING FOR A RE-READ MUST NOT RELOAD THE PAGE.
     *
     * schwungRemote.resubscribe() makes the manager re-send the slot's
     * hierarchy, and the manager's page answers a hierarchy message with a full
     * re-render of the slot -- which tears the iframe down and reloads this page.
     * That was "the whole app refreshes when I change a preset". So the page
     * never resubscribes. Instead it writes `temp_refresh=1`, a plugin key that
     * asks the firmware to dump the temp performance back into the image; the
     * manager re-reads state 250 ms after any write to a component and pushes
     * it, and nothing else moves. The same nudge serves the Re-read button. */
    var resyncTimer = null;
    function nudge() { if (remote) remote.setParam(PFX + "temp_refresh", "1"); }
    function scheduleResync(ms) {
        if (!remote) return;
        clearTimeout(resyncTimer);
        resyncTimer = setTimeout(nudge, ms || 400);
    }
    /* A preset load is confirmed by NAME, not by the next dump. The manager
     * re-reads the module 250 ms after a write, which is before the firmware has
     * clocked in a performance (~230 ms of sysex plus the load itself), so the
     * first dump after a selection can still be the OLD sound. Until the temp
     * image's name matches the row we asked for, keep re-reading on a widening
     * schedule; give up quietly after the last step (a preset with a blank name
     * can never match, and it should not re-read forever). */
    /* ...and NOT by name alone. Measured on the device: a performance's NAME
     * (the first bytes of the common block) lands in the image ~300 ms after the
     * selection, the two patches, the arpeggiator and the effects up to ~700 ms
     * after that. A page that stopped at the matching name kept the previous
     * sound's arp and FX on screen -- the first device bug report. So a load is
     * confirmed when the name matches AND two consecutive dumps are identical. */
    var LOAD_RETRY_MS = [600, 1200, 2000, 3200, 5000];
    function expectLoad(perf, name) {
        B.loadPending = true;
        B.expect = { perf: perf, name: (name || "").trim().toUpperCase(), step: 0, lastHex: null, stable: 0 };
        scheduleResync(LOAD_RETRY_MS[0]);
    }
    function nameMatches() {
        var e = B.expect;
        if (!e || !S.temp) return true;
        if (!e.name) return true;                              // nothing to compare against
        var sounding = (e.perf ? M.performanceName(S.temp) : M.patchName(S.temp, editPart())).trim().toUpperCase();
        return sounding === e.name;
    }
    function afterDump() {
        if (!B.loadPending || !B.expect) return;
        var e = B.expect, hex = S.temp ? M.bytesToHex(S.temp) : "";
        e.stable = hex === e.lastHex ? e.stable + 1 : 0;
        e.lastHex = hex;
        if (nameMatches() && e.stable >= 1) { B.loadPending = false; B.expect = null; return; }
        e.step++;
        if (e.step < LOAD_RETRY_MS.length) scheduleResync(LOAD_RETRY_MS[e.step]);
        else { B.loadPending = false; B.expect = null; }       // stop asking; the header's "sounding:" says what is
    }

    function onParams(params) {
        var gotTemp = false, presetMoved = false, touched = {};
        for (var full in params) {
            if (!Object.prototype.hasOwnProperty.call(params, full)) continue;
            if (full.indexOf(PFX) !== 0) continue;
            var key = full.substring(PFX.length), val = params[full];
            switch (key) {
                case "temp": {
                    var bytes = M.hexToBytes(val);
                    if (bytes && bytes.length === M.TEMP_LEN) {
                        S.temp = bytes; gotTemp = true;
                        if (!S.connected) { S.connected = true; setBusy(false); }
                    }
                    break;
                }
                case "sys": S.sys = M.decodeSys(val); touched.__sys = true; break;
                case "mode": {
                    var m = parseMode(val);
                    if (m >= 0 && m !== S.mode) { S.mode = m; presetMoved = true; }
                    break;
                }
                case "part": {
                    var pt = parseInt(val, 10);
                    if (!isNaN(pt) && pt !== S.part) { S.part = pt; presetMoved = true; }
                    break;
                }
                case "bank": case "patch": case "perf_bank": case "performance": {
                    var n = parseInt(val, 10);
                    if (!isNaN(n) && n !== S[key]) { S[key] = n; presetMoved = true; }
                    break;
                }
                case "buffer_ms": S.buffer_ms = parseInt(val, 10); touched.buffer_ms = true; break;
                case "version": case "state": break;
                default:
                    if (byKey[key]) {
                        /* A single value, e.g. a knob turned on the Move: it
                         * addresses the module's current edit part. */
                        var p = byKey[key], v = parseInt(val, 10);
                        if (isNaN(v)) break;
                        if (p.area === M.AREA.system) S.sys[key] = v;
                        else if (S.temp) {
                            if (p.area === M.AREA.patch) {
                                if (S.part === 0 || S.part === 2) M.writeParam(S.temp, p, 0, v);
                                if (S.part === 1 || S.part === 2) M.writeParam(S.temp, p, 1, v);
                            } else M.writeParam(S.temp, p, 0, v);
                        }
                        delete S.pending[key];
                        touched[key] = true;
                    } else {
                        S.names[key] = val;       // patch_name, patch_count, bank_list_name, ...
                        touched.__names = true;
                    }
            }
        }
        if (gotTemp) { S.pending = {}; afterDump(); refreshAll(); }
        else {
            for (var k in touched) if (byKey[k]) refreshKey(k);
            if (touched.__sys || touched.buffer_ms) refreshSystem();
            if (touched.__names || presetMoved) { refreshHeader(); refreshBrowser(); }
            if (Object.keys(touched).length) redrawViz(Object.keys(touched));
        }
        /* A preset, bank, mode or part moved on the device without a new dump
         * in the same message: ask the manager for a full re-read. */
        if (presetMoved) {
            refreshModes();
            var perfNow = S.mode === 1;
            expectLoad(perfNow, expectedName(perfNow, perfNow ? S.perf_bank : S.bank, perfNow ? S.performance : S.patch));
        }
    }

    function parseMode(v) {
        if (typeof v === "number") return v;
        var s = String(v).toLowerCase();
        if (s === "0" || s === "patch") return 0;
        if (s === "1" || s === "performance") return 1;
        if (s === "2" || s === "system") return 2;
        return -1;
    }

    /* ---- refresh -------------------------------------------------------------- */
    function valueOf(key) {
        var p = byKey[key];
        if (!p) return null;
        if (p.area === M.AREA.system) return S.sys[key] === undefined ? null : S.sys[key];
        if (!S.temp) return null;
        return M.readParam(S.temp, p, editPart());
    }
    function ghostOf(key) {
        var p = byKey[key];
        if (!p || p.area !== M.AREA.patch || !S.temp) return null;
        var g = M.readParam(S.temp, p, otherPart());
        return g === valueOf(key) ? null : g;
    }
    function refreshKey(key) {
        var w = W[key];
        if (!w) return;
        w.set(valueOf(key), ghostOf(key), !!S.pending[key]);
    }
    function refreshAll() {
        for (var key in W) refreshKey(key);
        refreshSystem(); refreshHeader(); refreshModes(); refreshBrowser(); redrawViz();
    }
    function refreshSystem() {
        M.SYSTEM.forEach(function (g) { g.keys.forEach(function (k) {
            if (k === "buffer_ms") { if (W.buffer_ms) W.buffer_ms.set(S.buffer_ms, null, false); }
            else refreshKey(k);
        }); });
    }

    /* ---- header: LCD, modes, part ---------------------------------------------- */
    var el = {};
    function pad(s, n) { s = String(s); return s.length >= n ? s.substring(0, n) : s + new Array(n - s.length + 1).join(" "); }
    function rpad(s, n) { s = String(s); return s.length >= n ? s.substring(s.length - n) : new Array(n - s.length + 1).join(" ") + s; }

    function lcdShow(p, value) {
        S.lcdText = pad(p.name.toUpperCase(), 13) + rpad(M.valueText(p, value), 7);
        S.lcdUntil = Date.now() + 1800;
        refreshHeader();
        clearTimeout(lcdShow.t);
        lcdShow.t = setTimeout(refreshHeader, 1850);
    }

    function refreshHeader() {
        var l1, l2;
        if (!S.connected) {
            l1 = "JE-8086";
            l2 = remote ? "waiting for slot" : "no host";
        } else if (S.mode === 2) {
            l1 = "SYSTEM";
            l2 = pad("REMOTE CH", 12) + rpad(M.valueText(byKey.sys_remote_ch, S.sys.sys_remote_ch), 8);
        } else {
            var perf = S.mode === 1;
            var name = perf ? M.performanceName(S.temp) : M.patchName(S.temp, editPart());
            var row = perf ? S.performance : S.patch;
            var bankName = bankLabel(perf ? S.perf_bank : S.bank, perf) || "";
            l1 = pad(name || "(no name)", 16) + rpad(row + 1, 4);
            var partTxt = perf ? "UPR+LWR" : ["UPPER", "LOWER", "BOTH"][S.part] || "";
            l2 = pad(bankName.toUpperCase(), 12) + rpad(partTxt, 8);
        }
        if (S.lcdText && Date.now() < S.lcdUntil) l2 = S.lcdText;
        el.lcd1.textContent = l1;
        el.lcd2.textContent = l2;
        el.dot.className = "dot " + (!S.connected ? "" : Object.keys(S.pending).length ? "busy" : "on");
        el.linkText.textContent = !S.connected ? "waiting for the slot" : Object.keys(S.pending).length ? "sending" : "in sync";
    }

    function refreshModes() {
        /* Two mode buttons, not three: System is a TAB here and never touches
         * the module's mode, so the remote cannot flip the device's screen into
         * its system pages. A device already in System lights neither. */
        el.modeBtns.forEach(function (b, i) { b.setAttribute("aria-pressed", String(S.mode === i)); b.querySelector(".led").classList.toggle("on", S.mode === i); });
        el.partBtns.forEach(function (b, i) { b.setAttribute("aria-checked", String(S.part === i)); b.querySelector(".led").classList.toggle("on", S.part === i); });
    }

    /* ---- tabs -------------------------------------------------------------------- */
    function tabById(id) { return M.TABS.filter(function (t) { return t.id === id; })[0]; }
    function setTab(id) {
        if (!tabById(id)) return;
        T.cur = id;
        try { localStorage.setItem("je8086.tab", id); } catch (e) { /* session only */ }
        applyTab();
    }
    function applyTab() {
        var t = tabById(T.cur) || M.TABS[1];
        el.tabBtns.forEach(function (b) {
            var on = b.dataset.tab === t.id;
            b.setAttribute("aria-selected", String(on)); b.tabIndex = on ? 0 : -1;
        });
        el.browser.hidden = t.id !== "presets";
        el.system.hidden = t.id !== "system";
        el.rows.hidden = !(t.all || t.sections);
        el.rows.classList.toggle("focus", !!t.sections);
        var show = null;
        if (t.sections) { show = {}; t.sections.forEach(function (id) { show[id] = true; }); }
        Array.prototype.forEach.call(el.rows.querySelectorAll(".section"), function (sec) {
            sec.hidden = show ? !show[sec.id.replace(/^sec-/, "")] : false;
        });
        Array.prototype.forEach.call(el.rows.querySelectorAll(".row"), function (r) {
            r.hidden = show ? !Array.prototype.some.call(r.children, function (c) { return !c.hidden; }) : false;
        });
        el.panel.setAttribute("data-tab", t.id);
        if (el.browseBtn) { el.browseBtn.setAttribute("aria-pressed", String(t.id === "presets")); el.browseBtn.querySelector(".led").classList.toggle("on", t.id === "presets"); }
        if (t.id === "presets") refreshBrowser();
        redrawViz();
    }
    function tabKeys(e) {
        var i = el.tabBtns.indexOf(document.activeElement);
        if (i < 0) return;
        var n = i;
        if (e.key === "ArrowRight") n = (i + 1) % el.tabBtns.length;
        else if (e.key === "ArrowLeft") n = (i - 1 + el.tabBtns.length) % el.tabBtns.length;
        else if (e.key === "Home") n = 0;
        else if (e.key === "End") n = el.tabBtns.length - 1;
        else return;
        e.preventDefault();
        el.tabBtns[n].focus(); setTab(el.tabBtns[n].dataset.tab);
    }

    /* ---- preset browser ----------------------------------------------------------
     *
     * The JP-8000 picks a sound with BANK 1-8 and NUMBER 1-8 buttons -- 64 to
     * a bank, spelled "A11".."A88" on its display -- so the browser keeps that
     * matrix: eight to a row, a code on every cell. Around it, what a screen can
     * add and a panel cannot: every bank as a pane, one search across all of
     * them, filters made from the names' own prefixes (LD, PD, BS...),
     * favourites, and the keyboard.
     *
     * The row the browser highlights is the module's ROW; what SOUNDS is read
     * from the temp image, and when the two names differ the header says so.
     */
    var B = { q: "", chip: null, favsOnly: false, favs: loadFavs(), loadPending: false, sig: "", all: false, chips: [] };

    function loadFavs() { try { return JSON.parse(localStorage.getItem("je8086.favs") || "{}") || {}; } catch (e) { return {}; } }
    function saveFavs() { try { localStorage.setItem("je8086.favs", JSON.stringify(B.favs)); } catch (e) { /* private window: favourites live for the session */ } }
    function favKey(perf, bankName, name) { return (perf ? "P|" : "A|") + bankName + "|" + name; }

    function bankList(perf) {
        if (S.catalog) return (perf ? S.catalog.performance : S.catalog.patch) || [];
        if (S.chain) {
            var e = S.chain.filter(function (c) { return c.key === (perf ? "perf_bank" : "bank"); })[0];
            if (e && e.options) return e.options.map(function (n) { return { name: n, presets: null }; });
        }
        return [];
    }
    function bankLabel(idx, perf) {
        var b = bankList(perf)[idx];
        return b ? b.name : (S.names.bank_list_name || "");
    }
    function isIgnoredRow(b) { return !!b && b.name.charAt(0) === "!"; }
    function bankLetter(b) { var m = /^Factory ([AB])$/.exec(b.name); return m ? m[1] : ""; }
    /* "A23": bank letter, bank 1-8, number 1-8; a page digit past the 64th. */
    function code(b, i) {
        /* a one-file bank of three performances has rows 1, 2, 3, not A11..A13 */
        if (!b.presets || b.presets.length <= 8) return String(i + 1);
        return bankLetter(b) + (i >= 64 ? String(Math.floor(i / 64) + 1) + "." : "") + (Math.floor(i / 8) % 8 + 1) + (i % 8 + 1);
    }
    /* A preset with no name in the file: the file's own name when it is the
     * only one (a single-performance dump), otherwise its row. Never blank. */
    function rowName(b, i, name) { return name || (b.presets && b.presets.length === 1 ? b.name : "Preset " + (i + 1)); }
    function tagOf(name) { var m = /^([A-Z]{2,3})[\s:._\-\/]/.exec(name || ""); return m ? m[1] : null; }

    /* Tags that occur more than once across this kind's banks become chips. */
    function prefixChips(perf) {
        /* Chips only when the library really uses tags: UPPERCASE two- or
         * three-letter prefixes, each seen at least three times, together on at
         * least a sixth of the presets. A library of "Ham Sandwich" and "The
         * Void" produced HAM and THE chips before this rule. */
        var freq = {}, total = 0;
        bankList(perf).forEach(function (b) { (b.presets || []).forEach(function (n) { total++; var t = tagOf(n); if (t) freq[t] = (freq[t] || 0) + 1; }); });
        var tags = Object.keys(freq).filter(function (t) { return freq[t] >= 3; });
        var covered = tags.reduce(function (a, t) { return a + freq[t]; }, 0);
        if (!total || covered < total / 6) return [];
        return tags.sort(function (a, b) { return freq[b] - freq[a] || a.localeCompare(b); }).slice(0, 10);
    }
    function rowMatches(b, i, name, perf) {
        name = rowName(b, i, name);
        if (B.q) {
            var n = name.toLowerCase();
            if (n.indexOf(B.q) < 0 && code(b, i).toLowerCase() !== B.q && String(i + 1) !== B.q) return false;
        }
        if (B.chip && tagOf(name) !== B.chip) return false;
        if (B.favsOnly && !B.favs[favKey(perf, b.name, name)]) return false;
        return true;
    }
    function filtering() { return !!(B.q || B.chip || B.favsOnly || B.all); }
    /* A stable, muted hue per tag so LD, PD, BS read apart at a glance. */
    function tagHue(t) { var hsh = 0; for (var i = 0; i < t.length; i++) hsh = (hsh * 31 + t.charCodeAt(i)) % 360; return "hsl(" + hsh + " 34% 54%)"; }

    function refreshBrowser() {
        if (!el.browser) return;
        var perf = S.mode === 1, banks = bankList(perf);
        var curBank = perf ? S.perf_bank : S.bank, cur = perf ? S.performance : S.patch;
        el.browserTitle.textContent = perf ? "Performances" : "Patches";
        el.partHint.hidden = perf;
        el.partHint.textContent = "a patch loads into " + ["Upper", "Lower", "both parts"][S.part] + " (Panel Select)";

        /* banks pane and its narrow-screen twin, rebuilt only when the list changes */
        var sig = (perf ? "p" : "a") + banks.map(function (b) { return b.name + ":" + (b.presets ? b.presets.length : "?"); }).join("|");
        if (B.sig !== sig) {
            B.sig = sig;
            el.banks.innerHTML = ""; el.bankSel.innerHTML = "";
            var total = banks.reduce(function (a, b) { return a + (b.presets && !isIgnoredRow(b) ? b.presets.length : 0); }, 0);
            var allLi = document.createElement("li"), allBt = document.createElement("button"); allBt.type = "button"; allBt.className = "all";
            allBt.innerHTML = '<span class="bn">All banks</span><span class="n"></span>'; allBt.querySelector(".n").textContent = total;
            allBt.addEventListener("click", function () { B.all = true; refreshBrowser(); });
            allLi.appendChild(allBt); el.banks.appendChild(allLi);
            var lastFolder = null, anyFolder = banks.some(function (b) { return b.folder; });
            banks.forEach(function (b, i) {
                if (anyFolder && !isIgnoredRow(b)) {
                    var fold = b.folder || "";
                    if (fold !== lastFolder) {
                        lastFolder = fold;
                        var fh = document.createElement("li"); fh.className = "folder"; fh.textContent = fold || "Factory & top level"; el.banks.appendChild(fh);
                    }
                }
                var li = document.createElement("li");
                var bt = document.createElement("button"); bt.type = "button"; bt.dataset.idx = i;
                bt.className = isIgnoredRow(b) ? "ignored" : "";
                bt.innerHTML = '<span class="bn"></span><span class="n"></span>';
                bt.querySelector(".bn").textContent = isIgnoredRow(b) ? b.name.substring(2) : b.name;
                bt.querySelector(".n").textContent = b.presets ? (isIgnoredRow(b) ? "" : b.presets.length) : "";
                bt.addEventListener("click", function () { if (!isIgnoredRow(b)) { B.all = false; selectBank(i); } });
                li.appendChild(bt); el.banks.appendChild(li);
                var o = document.createElement("option"); o.value = i; o.textContent = b.name + (b.presets && !isIgnoredRow(b) ? " (" + b.presets.length + ")" : ""); el.bankSel.appendChild(o);
            });
            el.chips.innerHTML = "";
            B.chips = prefixChips(perf);
            if (B.chip && B.chips.indexOf(B.chip) < 0) B.chip = null;
            B.chips.forEach(function (t) {
                var c = document.createElement("button"); c.type = "button"; c.className = "chip tagchip"; c.textContent = t;
                c.style.setProperty("--tag", tagHue(t));
                c.setAttribute("aria-pressed", String(B.chip === t));
                c.addEventListener("click", function () { B.chip = B.chip === t ? null : t; refreshBrowser(); });
                el.chips.appendChild(c);
            });
            el.chips.hidden = !el.chips.children.length;
        }
        Array.prototype.forEach.call(el.banks.querySelectorAll("button"), function (bt) {
            var isAll = bt.classList.contains("all");
            bt.setAttribute("aria-current", String(isAll ? B.all : (!B.all && parseInt(bt.dataset.idx, 10) === curBank)));
        });
        Array.prototype.forEach.call(el.chips.children, function (c) { c.setAttribute("aria-pressed", String(B.chip === c.textContent)); });
        el.bankSel.value = String(curBank);
        el.favBtn.setAttribute("aria-pressed", String(B.favsOnly));

        var bank = banks[curBank], rows = bank && bank.presets;
        var count = rows ? rows.length : parseInt(S.names[perf ? "performance_count" : "patch_count"], 10);
        el.count.textContent = isNaN(count) ? "" : (rows && bank ? code(bank, cur) + "  " : "") + (cur + 1) + " / " + count;
        el.bankName.textContent = B.all ? "All banks" : (bank ? bank.name : "");

        /* the browser names a ROW; the image says what SOUNDS */
        var curName = rows ? rowName(bank, cur, rows[cur]) : S.names[perf ? "performance_name" : "patch_name"];
        var sounding = S.temp ? (perf ? M.performanceName(S.temp) : M.patchName(S.temp, editPart())) : "";
        var differs = sounding && curName && rows && rows[cur] && sounding.trim().toUpperCase() !== String(rows[cur]).trim().toUpperCase();
        el.sounding.textContent = differs ? "sounding: " + sounding : "";
        el.sounding.hidden = !differs;

        /* Rebuild the grid only when what it LISTS changed. A dump from the
         * manager arrives after every write and every preset load, and tearing
         * the cells down and back up on each one is a visible flicker; when only
         * the selection moved, the cells stay and their state is updated. */
        var grid = el.grid;
        var gridSig = [perf ? "p" : "a", curBank, B.q, B.chip || "", B.favsOnly ? 1 : 0, B.all ? 1 : 0, sig, rows ? rows.length : -1].join("|");
        if (grid.dataset.sig === gridSig && grid.children.length) {
            Array.prototype.forEach.call(grid.querySelectorAll(".pcell"), function (d) {
                var isCur = parseInt(d.dataset.bi, 10) === curBank && parseInt(d.dataset.i, 10) === cur;
                if (d.getAttribute("aria-current") !== String(isCur)) { d.setAttribute("aria-current", String(isCur)); d.querySelector(".led").classList.toggle("on", isCur); }
                d.classList.toggle("pending", isCur && B.loadPending);
            });
            return;
        }
        grid.dataset.sig = gridSig;
        grid.innerHTML = "";
        if (filtering()) {
            grid.className = "pgrid list"; grid.setAttribute("aria-label", B.all && !B.q && !B.chip && !B.favsOnly ? "Every preset in every bank" : "Search results across all banks");
            var shown = 0, lastGroup = null, plainAll = B.all && !B.q && !B.chip && !B.favsOnly;
            banks.forEach(function (b, bi) {
                if (!b.presets) return;
                b.presets.forEach(function (n, i) {
                    if (!rowMatches(b, i, n, perf)) return;
                    shown++;
                    /* In the All view, headings group the cells: a bank of many
                     * presets is its own group; single-preset FILES are grouped by
                     * the folder they sit in, each cell named after its file. */
                    if (plainAll) {
                        var single = b.presets.length === 1;
                        var group = single ? "folder:" + (b.folder || "") : "bank:" + bi;
                        if (group !== lastGroup) {
                            lastGroup = group;
                            var bh = document.createElement("div"); bh.className = "bankhead";
                            var count = single ? banks.filter(function (x) { return x.presets && x.presets.length === 1 && (x.folder || "") === (b.folder || ""); }).length : b.presets.length;
                            bh.textContent = (single ? (b.folder || "Top level") + "  ·  single-preset files" : (b.folder ? b.folder + " / " : "") + b.name) + "  ·  " + count;
                            grid.appendChild(bh);
                        }
                    }
                    grid.appendChild(cellFor(b, bi, i, n, perf, bi === curBank && i === cur, !plainAll));
                });
            });
            if (!shown) grid.appendChild(emptyMsg(B.favsOnly && !B.q && !B.chip ? "No favourites yet. Star a preset to keep it here." : "Nothing matches" + (B.q ? " “" + el.search.value + "”" : "") + (B.chip ? " in " + B.chip : "") + "."));
            return;
        }
        grid.className = "pgrid"; grid.setAttribute("aria-label", (bank ? bank.name : "") + " presets");
        if (!rows) {
            grid.appendChild(emptyMsg(isIgnoredRow(bank) ? "These files matched a bank extension but held no preset data, so there is nothing to list."
                : "This build of the module publishes no preset list. Step with Prev and Next; the display names each row as it loads."));
            return;
        }
        rows.forEach(function (n, i) { grid.appendChild(cellFor(bank, curBank, i, n, perf, i === cur, false)); });
    }
    function emptyMsg(t) { var d = document.createElement("div"); d.className = "empty"; d.textContent = t; return d; }

    function cellFor(b, bi, i, name, perf, current, showBank) {
        var d = document.createElement("div"); d.className = "pcell" + (current && B.loadPending ? " pending" : "");
        d.dataset.bi = bi; d.dataset.i = i;
        var shown = rowName(b, i, name);
        var tag = tagOf(name);
        if (tag && B.chips.indexOf(tag) >= 0) { d.classList.add("tagged"); d.style.setProperty("--tag", tagHue(tag)); }
        d.setAttribute("aria-current", String(current)); d.setAttribute("role", "gridcell");
        var load = document.createElement("button"); load.type = "button"; load.className = "load";
        load.innerHTML = '<span class="led"></span><span class="code"></span><span class="nm"></span>' + (showBank ? '<span class="bk"></span>' : "");
        load.querySelector(".led").classList.toggle("on", current);
        load.querySelector(".code").textContent = code(b, i);
        load.querySelector(".nm").textContent = shown;
        if (showBank) load.querySelector(".bk").textContent = (b.folder ? b.folder + " / " : "") + b.name;
        load.setAttribute("aria-label", code(b, i) + " " + shown + (showBank ? ", " + b.name : "") + (current ? ", selected" : ""));
        load.addEventListener("click", function () { selectBankRow(bi, i); });
        var fk = favKey(perf, b.name, shown);
        var star = document.createElement("button"); star.type = "button"; star.className = "star";
        star.setAttribute("aria-pressed", String(!!B.favs[fk])); star.setAttribute("aria-label", (B.favs[fk] ? "Remove " : "Add ") + shown + (B.favs[fk] ? " from" : " to") + " favourites");
        star.textContent = B.favs[fk] ? "★" : "☆";
        star.addEventListener("click", function (e) { e.stopPropagation(); if (B.favs[fk]) delete B.favs[fk]; else B.favs[fk] = 1; saveFavs(); refreshBrowser(); });
        d.appendChild(load); d.appendChild(star);
        return d;
    }

    /* Arrow keys walk the matrix the way the panel's buttons sit. */
    function gridKeys(e) {
        var t = e.target;
        if (!t.classList || !t.classList.contains("load")) return;
        var loads = Array.prototype.slice.call(el.grid.querySelectorAll(".load"));
        var idx = loads.indexOf(t); if (idx < 0) return;
        var cols = getComputedStyle(el.grid).gridTemplateColumns.split(" ").length || 1;
        var next = idx;
        switch (e.key) {
            case "ArrowRight": next = idx + 1; break;
            case "ArrowLeft": next = idx - 1; break;
            case "ArrowDown": next = idx + cols; break;
            case "ArrowUp": next = idx - cols; break;
            case "Home": next = 0; break;
            case "End": next = loads.length - 1; break;
            default: return;
        }
        e.preventDefault();
        if (next >= 0 && next < loads.length) loads[next].focus();
    }

    function selectBankRow(bi, i) {
        var perf = S.mode === 1;
        var curBank = perf ? S.perf_bank : S.bank;
        if (bi !== curBank) {
            /* Two writes, in order: the bank (which loads its first row) then
             * the row. request_load only bumps a sequence, so the child serves
             * the second and the first costs nothing audible. */
            if (perf) S.perf_bank = bi; else S.bank = bi;
            setParam(perf ? "perf_bank" : "bank", bi);
        }
        selectRow(i);
    }
    function expectedName(perf, bi, i) {
        var b = bankList(perf)[bi];
        return b && b.presets ? b.presets[i] : null;
    }
    function selectRow(i) {
        var perf = S.mode === 1;
        if (perf) S.performance = i; else S.patch = i;
        setParam(perf ? "performance" : "patch", i);
        expectLoad(perf, expectedName(perf, perf ? S.perf_bank : S.bank, i));
        refreshBrowser(); refreshHeader();
    }
    function selectBank(i) {
        var perf = S.mode === 1;
        if (perf) { S.perf_bank = i; S.performance = 0; } else { S.bank = i; S.patch = 0; }
        setParam(perf ? "perf_bank" : "bank", i);   // selects the bank AND loads its first preset
        expectLoad(perf, expectedName(perf, i, 0));
        refreshBrowser(); refreshHeader();
    }
    function visibleRows() {
        return Array.prototype.map.call(el.grid.querySelectorAll(".pcell .load"), function (l) { return l; });
    }
    function stepRow(d) {
        var perf = S.mode === 1;
        var rows = (bankList(perf)[perf ? S.perf_bank : S.bank] || {}).presets;
        var count = rows ? rows.length : parseInt(S.names[perf ? "performance_count" : "patch_count"], 10);
        var cur = perf ? S.performance : S.patch;
        var next = cur + d;
        if (!isNaN(count) && count > 0) next = (next + count) % count;
        if (next < 0) next = 0;
        selectRow(next);
    }
    function randomRow() {
        var perf = S.mode === 1, banks = bankList(perf);
        var pool = [];
        if (filtering()) {
            banks.forEach(function (b, bi) { (b.presets || []).forEach(function (n, i) { if (rowMatches(b, i, n, perf)) pool.push([bi, i]); }); });
        } else {
            var bi0 = perf ? S.perf_bank : S.bank, rows = (banks[bi0] || {}).presets;
            var count = rows ? rows.length : parseInt(S.names[perf ? "performance_count" : "patch_count"], 10);
            for (var i = 0; i < (count || 0); i++) pool.push([bi0, i]);
        }
        if (!pool.length) return;
        var pick = pool[Math.floor(Math.random() * pool.length)];
        selectBankRow(pick[0], pick[1]);
    }
    function setMode(m) {
        if (m === S.mode) return;
        S.mode = m;
        setParam("mode", ["Patch", "Performance"][m]);
        refreshModes(); refreshHeader(); refreshBrowser();
    }
    function setPart(pt) {
        S.part = pt;
        setParam("part", pt);      // the plugin writes Panel Select through this
        refreshModes(); refreshAll();
    }

    function loadCatalog() {
        var url = "/api/remote-ui/module-assets/" + encodeURIComponent(MODULE_ID) + "/banks_index.json";
        if (window.fetch) fetch(url, { cache: "no-store" }).then(function (r) { return r.ok ? r.json() : null; })
            .then(function (j) { if (j && j.patch) { S.catalog = j; refreshBrowser(); refreshHeader(); } })
            .catch(function () { /* older dsp.so: no catalog, the fallback UI stands */ });
    }

    /* ---- widgets ----------------------------------------------------------------- */
    function cell(label, cls) {
        var c = document.createElement("div");
        c.className = "cell" + (cls ? " " + cls : "");
        var l = document.createElement("div"); l.className = "label"; l.textContent = label;
        var v = document.createElement("div"); v.className = "value"; v.textContent = "--";
        c.appendChild(l);
        return { el: c, label: l, value: v };
    }

    /* Drag/wheel/keyboard handling shared by knobs and sliders. `axis` is the
     * pixel travel for the full range. */
    /* How many fingers are on a control right now. While it is not zero the
     * displays draw without shadow blur and the scopes stand still: a drag
     * frame has to be cheap, and the glow comes back on the frame after the
     * finger lifts. */
    var DRAG = { n: 0 };
    function draggingNow() { return DRAG.n > 0; }
    function bindRange(node, p, get, commit, pixels) {
        var startY = 0, startV = 0, dragging = false, acc = 0;
        var span = p.max - p.min;
        node.addEventListener("pointerdown", function (e) {
            if (e.button !== 0 && e.pointerType === "mouse") return;
            dragging = true; startY = e.clientY; startV = get(); acc = 0; DRAG.n++;
            node.classList.add("dragging");
            node.setPointerCapture(e.pointerId);
            node.focus({ preventScroll: true });
            e.preventDefault();
        });
        node.addEventListener("pointermove", function (e) {
            if (!dragging) return;
            var fine = e.shiftKey ? 4 : 1;
            var dv = (startY - e.clientY) / (pixels * fine) * span;
            var nv = Math.round(startV + dv);
            if (nv !== get()) commit(nv);
        });
        function up(e) { if (dragging) { dragging = false; DRAG.n = Math.max(0, DRAG.n - 1); node.classList.remove("dragging"); try { node.releasePointerCapture(e.pointerId); } catch (x) {} redrawViz([p.key]); } }
        node.addEventListener("pointerup", up);
        node.addEventListener("pointercancel", up);
        node.addEventListener("wheel", function (e) {
            e.preventDefault();
            acc += e.deltaY;
            var step = e.deltaMode === 1 ? 1 : 24;     // lines vs pixels
            if (Math.abs(acc) < step) return;
            var n = Math.trunc(acc / step); acc -= n * step;
            commit(get() - n * (e.shiftKey ? 5 : 1));
        }, { passive: false });
        node.addEventListener("dblclick", function () { commit(p.default); });
        node.addEventListener("keydown", function (e) {
            var v = get(), d = 0;
            switch (e.key) {
                case "ArrowUp": case "ArrowRight": d = 1; break;
                case "ArrowDown": case "ArrowLeft": d = -1; break;
                case "PageUp": d = 10; break;
                case "PageDown": d = -10; break;
                case "Home": commit(p.min); e.preventDefault(); return;
                case "End": commit(p.max); e.preventDefault(); return;
                case "Backspace": case "Delete": commit(p.default); e.preventDefault(); return;
                default: return;
            }
            e.preventDefault();
            commit(v + d * (e.shiftKey ? 5 : 1));
        });
    }

    var SVGNS = "http://www.w3.org/2000/svg";
    function svgEl(tag, attrs) { var n = document.createElementNS(SVGNS, tag); for (var k in attrs) n.setAttribute(k, attrs[k]); return n; }
    function polar(cx, cy, r, deg) { var a = (deg - 90) * Math.PI / 180; return [cx + r * Math.cos(a), cy + r * Math.sin(a)]; }
    function arcPath(cx, cy, r, a0, a1) {
        if (a1 < a0) { var t = a0; a0 = a1; a1 = t; }
        var p0 = polar(cx, cy, r, a0), p1 = polar(cx, cy, r, a1);
        return "M" + p0[0].toFixed(2) + " " + p0[1].toFixed(2) + " A" + r + " " + r + " 0 " + (a1 - a0 > 180 ? 1 : 0) + " 1 " + p1[0].toFixed(2) + " " + p1[1].toFixed(2);
    }

    /* Shared SVG defs: the knob's metal, its well, the arc's glow. One copy in
     * the document; every knob references them by id. */
    function ensureDefs() {
        if (document.getElementById("jp-defs")) return;
        var svg = svgEl("svg", { id: "jp-defs", width: 0, height: 0, "aria-hidden": "true", style: "position:absolute" });
        svg.innerHTML =
            '<defs>' +
            '<radialGradient id="kcap" cx="38%" cy="32%" r="75%"><stop offset="0" stop-color="#2f353d"/><stop offset="0.55" stop-color="#171b20"/><stop offset="1" stop-color="#0b0d10"/></radialGradient>' +
            '<linearGradient id="krim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5a636e"/><stop offset="1" stop-color="#1c2127"/></linearGradient>' +
            '<radialGradient id="kwell" cx="50%" cy="50%" r="50%"><stop offset="0.82" stop-color="#1a1e24"/><stop offset="1" stop-color="#11141a"/></radialGradient>' +
            '<filter id="kglow" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="1.6" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>' +
            '</defs>';
        document.body.insertBefore(svg, document.body.firstChild);
    }
    /* The tick scale round the travel: eleven marks, the middle one longer. */
    var TICKS = (function () {
        var g = "";
        for (var i = 0; i <= 10; i++) {
            var a = -135 + 270 * i / 10, r0 = i === 5 ? 29.5 : 30.5, p0 = polar(32, 32, r0, a), p1 = polar(32, 32, 32.5, a);
            g += '<line x1="' + p0[0].toFixed(1) + '" y1="' + p0[1].toFixed(1) + '" x2="' + p1[0].toFixed(1) + '" y2="' + p1[1].toFixed(1) + '"/>';
        }
        return g;
    })();

    function knobWidget(key, opts) {
        var p = byKey[key];
        var c = cell(opts.label || p.name, opts.big ? "big" : "");
        var k = document.createElement("div");
        k.className = "knob"; k.tabIndex = 0; k.setAttribute("role", "slider");
        k.setAttribute("aria-label", opts.label || p.name);
        k.setAttribute("aria-valuemin", p.min); k.setAttribute("aria-valuemax", p.max);
        var bipolar = p.min < 0 && p.max > 0;
        var svg = svgEl("svg", { viewBox: "-2 -2 68 68" });
        svg.innerHTML =
            '<circle class="well" cx="32" cy="32" r="30"/>' +
            '<g class="ticks">' + TICKS + '</g>' +
            '<path class="track" d="' + arcPath(32, 32, 27, -135, 135) + '"/>' +
            '<path class="arcglow" d=""/>' +
            '<path class="arc" d=""/>' +
            '<line class="ghost" x1="0" y1="0" x2="0" y2="0" visibility="hidden"/>' +
            '<circle class="rim" cx="32" cy="32" r="22"/>' +
            '<circle class="cap" cx="32" cy="32" r="20.5"/>' +
            '<line class="ptr" x1="32" y1="30" x2="32" y2="15"/>' +
            (bipolar ? '<circle class="detent" cx="32" cy="2.5" r="1.1"/>' : "");
        var arc = svg.querySelector(".arc"), arcGlow = svg.querySelector(".arcglow"), ghost = svg.querySelector(".ghost"), ptr = svg.querySelector(".ptr");
        k.appendChild(svg);
        c.el.appendChild(k); c.el.appendChild(c.value);
        var cur = p.default;
        function angle(v) { return -135 + 270 * (v - p.min) / (p.max - p.min); }
        function set(v, g, pending) {
            var has = v !== null && v !== undefined;
            cur = has ? v : p.default;
            var a = angle(cur);
            ptr.style.transform = "rotate(" + a + "deg)";
            var ad = bipolar ? arcPath(32, 32, 27, angle(0), a) : arcPath(32, 32, 27, -135, a);
            arc.setAttribute("d", ad); arcGlow.setAttribute("d", ad);
            arc.style.visibility = arcGlow.style.visibility = has && (bipolar ? cur !== 0 : cur > p.min) ? "" : "hidden";
            if (g !== null && g !== undefined) {
                var ga = angle(g), q0 = polar(32, 32, 30.5, ga), q1 = polar(32, 32, 24.5, ga);
                ghost.setAttribute("x1", q0[0]); ghost.setAttribute("y1", q0[1]); ghost.setAttribute("x2", q1[0]); ghost.setAttribute("y2", q1[1]);
                ghost.setAttribute("visibility", "visible");
            } else ghost.setAttribute("visibility", "hidden");
            c.value.textContent = has ? M.valueText(p, cur) : "--";
            k.setAttribute("aria-valuenow", cur); k.setAttribute("aria-valuetext", M.valueText(p, cur));
            c.el.classList.toggle("pending", !!pending);
        }
        bindRange(k, p, function () { return cur; }, function (v) { (opts.commit || edit)(key, M.clamp(p, v)); }, 140);
        set(null, null, false);
        return { el: c.el, set: set, cell: c };
    }

    function sliderWidget(key, opts) {
        var p = byKey[key];
        var c = cell(opts.label || p.name, "");
        var s = document.createElement("div");
        s.className = "slider"; s.tabIndex = 0; s.setAttribute("role", "slider");
        s.setAttribute("aria-label", p.name); s.setAttribute("aria-valuemin", p.min); s.setAttribute("aria-valuemax", p.max);
        var rail = document.createElement("div"); rail.className = "rail";
        var fill = document.createElement("div"); fill.className = "fill";
        var gm = document.createElement("div"); gm.className = "ghostm"; gm.hidden = true;
        var capel = document.createElement("div"); capel.className = "capel";
        s.appendChild(rail); s.appendChild(fill); s.appendChild(gm); s.appendChild(capel);
        c.el.appendChild(s); c.el.appendChild(c.value);
        var cur = p.default;
        function pct(v) { return (v - p.min) / (p.max - p.min); }
        function set(v, g, pending) {
            var has = v !== null && v !== undefined;
            cur = has ? v : p.default;
            var top = 6 + (1 - pct(cur)) * 104;         // rail runs 6..110 of 116
            capel.style.top = top + "px";
            fill.style.height = (110 - top) + "px";
            fill.style.visibility = has ? "" : "hidden";
            if (g !== null && g !== undefined) { gm.hidden = false; gm.style.top = (6 + (1 - pct(g)) * 104) + "px"; } else gm.hidden = true;
            c.value.textContent = has ? M.valueText(p, cur) : "--";
            s.setAttribute("aria-valuenow", cur); s.setAttribute("aria-valuetext", M.valueText(p, cur));
            c.el.classList.toggle("pending", !!pending);
        }
        bindRange(s, p, function () { return cur; }, function (v) { edit(key, M.clamp(p, v)); }, 104);
        set(null, null, false);
        return { el: c.el, set: set };
    }

    /* The panel prints a waveform beside every waveform button and a curve
     * beside every filter type. Same here, as 24x12 strokes. */
    var GLYPH = {
        "TRIANGLE": "M0 9 L6 3 L12 9 L18 3 L24 9",
        "SAWTOOTH": "M0 9 L8 3 L8 9 L16 3 L16 9 L24 3",
        "SAW": "M0 9 L8 3 L8 9 L16 3 L16 9 L24 3",
        "SQUARE": "M0 9 L0 3 L8 3 L8 9 L16 9 L16 3 L24 3",
        "SQR (PWM)": "M0 9 L0 3 L5 3 L5 9 L12 9 L12 3 L17 3 L17 9 L24 9",
        "RANDOM": "M0 6 L4 6 L4 3 L8 3 L8 8 L12 8 L12 5 L16 5 L16 9 L20 9 L20 4 L24 4",
        "NOISE": "M0 6 L2 3 L4 8 L6 4 L8 9 L10 5 L12 7 L14 3 L16 8 L18 5 L20 7 L22 4 L24 6",
        "SUPER SAW": "M0 9 L8 3 L8 9 L16 3 L16 9 L24 3 M0 10.5 L8 4.5 L8 10.5 L16 4.5 L16 10.5 L24 4.5 M0 7.5 L8 1.5 L8 7.5 L16 1.5 L16 7.5 L24 1.5",
        "TRIANGLE MOD": "M0 9 C4 9 5 3 8 3 C11 3 12 9 16 9 C20 9 21 3 24 3",
        "FEEDBACK OSC": "M0 9 L8 3 L6 7 L8 9 L16 3 L14 7 L16 9 L24 3",
        "HPF": "M0 10 C8 10 10 2 14 2 L24 2",
        "BPF": "M0 10 C8 10 9 2 12 2 C15 2 16 10 24 10",
        "LPF": "M0 2 L10 2 C14 2 16 10 24 10",
    };
    var GLYPH_KEYS = { lfo1_waveform: 1, osc1_waveform: 1, osc2_waveform: 1, filter_type: 1 };
    function glyphFor(key, name) {
        if (!GLYPH_KEYS[key] || !GLYPH[name]) return "";
        var w = name === "SUPER SAW" ? 1.2 : 1.5;
        return '<svg class="glyph" viewBox="-1 -1 26 14" aria-hidden="true"><path d="' + GLYPH[name] + '" fill="none" stroke="currentColor" stroke-width="' + w + '" stroke-linejoin="round" stroke-linecap="round"/></svg>';
    }

    function ledsWidget(key, opts) {
        var p = byKey[key];
        var c = document.createElement("div"); c.className = "cell leds" + (opts.inline ? " row" : "");
        var l = document.createElement("div"); l.className = "label"; l.textContent = opts.label || p.name; c.appendChild(l);
        var g = document.createElement("div"); g.className = "opts"; g.setAttribute("role", "radiogroup"); g.setAttribute("aria-label", p.name);
        var btns = p.options.map(function (name, i) {
            var b = document.createElement("button"); b.type = "button"; b.className = "opt"; b.setAttribute("role", "radio");
            b.setAttribute("aria-checked", "false");
            b.innerHTML = '<span class="led"></span>' + glyphFor(key, name) + '<span class="txt"></span>';
            b.querySelector(".txt").textContent = name;
            b.addEventListener("click", function () { (opts.commit || edit)(key, i); });
            g.appendChild(b); return b;
        });
        c.appendChild(g);
        function set(v, ghost, pending) {
            btns.forEach(function (b, i) { b.setAttribute("aria-checked", String(v === i)); b.classList.toggle("ghosted", ghost === i); });
            c.classList.toggle("pending", !!pending);
        }
        return { el: c, set: set };
    }

    function switchWidget(key, opts) {
        var p = byKey[key];
        var c = document.createElement("div"); c.className = "cell switch";
        var l = document.createElement("div"); l.className = "label"; l.textContent = opts.label || p.name; c.appendChild(l);
        var b = document.createElement("button"); b.type = "button"; b.className = "btn"; b.setAttribute("role", "switch"); b.setAttribute("aria-checked", "false");
        b.setAttribute("aria-label", p.name);
        b.innerHTML = '<span class="led"></span><span class="txt">OFF</span>';
        var cur = 0;
        b.addEventListener("click", function () { edit(key, cur ? 0 : 1); });
        c.appendChild(b);
        function set(v, ghost, pending) {
            cur = v ? 1 : 0;
            b.setAttribute("aria-checked", String(!!v));
            b.querySelector(".led").classList.toggle("on", !!v);
            b.querySelector(".txt").textContent = v === null || v === undefined ? "--" : (p.options ? p.options[cur] : (cur ? "ON" : "OFF"));
            c.classList.toggle("pending", !!pending);
        }
        return { el: c, set: set };
    }

    function selectWidget(key, opts) {
        var p = byKey[key];
        var c = document.createElement("div"); c.className = "cell select" + (opts.cls ? " " + opts.cls : "");
        var l = document.createElement("label"); l.className = "label"; l.textContent = opts.label || p.name;
        var s = document.createElement("select"); s.setAttribute("aria-label", p.name);
        var id = "sel_" + key + "_" + Math.random().toString(36).slice(2, 7); s.id = id; l.htmlFor = id;
        p.options.forEach(function (name, i) { var o = document.createElement("option"); o.value = i; o.textContent = name; s.appendChild(o); });
        s.addEventListener("change", function () { edit(key, parseInt(s.value, 10)); });
        c.appendChild(l); c.appendChild(s);
        function set(v, ghost, pending) { if (v !== null && v !== undefined) s.value = String(v); c.classList.toggle("pending", !!pending); }
        return { el: c, set: set };
    }

    /* LFO 2: one Depth knob following the Depth Select switch, as on the panel. */
    function lfo2DepthWidget(item) {
        var keys = item.keys;
        var wrap = document.createElement("div"); wrap.className = "stack";
        var knobs = keys.map(function (k) { var w = knobWidget(k, { label: byKey[k].name.replace(/ LFO2$/, "") + " depth" }); wrap.appendChild(w.el); return w; });
        function show() {
            var sel = valueOf("lfo2_depth_select");
            knobs.forEach(function (w, i) { w.el.hidden = sel !== null && sel !== i; });
        }
        return { el: wrap, knobs: knobs, show: show };
    }

    /* ---- build the panel ------------------------------------------------------------ */
    function build() {
        var root = document.getElementById("app");
        root.innerHTML = "";
        ensureDefs();
        var panel = document.createElement("div"); panel.className = "panel"; panel.setAttribute("aria-busy", "true");
        root.appendChild(panel);

        /* header */
        var head = document.createElement("div"); head.className = "head";
        var brand = document.createElement("div"); brand.className = "brand";
        brand.innerHTML = '<div class="logo">JE-8086<small>Analog Modeling</small></div><div class="link"><span class="dot"></span><span class="ltxt">waiting for the slot</span></div>';
        el.dot = brand.querySelector(".dot"); el.linkText = brand.querySelector(".ltxt");

        var lcdWrap = document.createElement("div"); lcdWrap.className = "lcd-wrap";
        var lcd = document.createElement("div"); lcd.className = "lcd"; lcd.setAttribute("role", "status"); lcd.setAttribute("aria-live", "polite");
        lcd.innerHTML = '<span class="line"></span><span class="line"></span>';
        el.lcd1 = lcd.children[0]; el.lcd2 = lcd.children[1];
        var tools = document.createElement("div"); tools.className = "lcd-tools";
        var prev = mkBtn("◂ Prev", "quiet"), next = mkBtn("Next ▸", "quiet"), browse = mkBtn("Presets", "");
        browse.setAttribute("aria-pressed", "false"); browse.innerHTML = '<span class="led"></span>Presets'; el.browseBtn = browse;
        prev.addEventListener("click", function () { stepRow(-1); });
        next.addEventListener("click", function () { stepRow(1); });
        var sync = mkBtn("Re-read", "quiet"); sync.title = "Ask the module for a fresh dump of everything";
        sync.addEventListener("click", nudge);
        tools.appendChild(prev); tools.appendChild(next); tools.appendChild(browse);
        var sp = document.createElement("span"); sp.className = "spacer"; tools.appendChild(sp); tools.appendChild(sync);
        lcdWrap.appendChild(lcd); lcdWrap.appendChild(tools);

        var side = document.createElement("div"); side.className = "side";
        var modes = document.createElement("div"); modes.className = "modes"; modes.setAttribute("role", "group"); modes.setAttribute("aria-label", "Mode");
        modes.innerHTML = '<div class="group-label">Mode</div><div class="seg"></div>';
        el.modeBtns = ["Patch", "Performance"].map(function (n, i) {
            var b = mkBtn(n, ""); b.setAttribute("aria-pressed", "false"); b.innerHTML = '<span class="led"></span>' + n;
            b.addEventListener("click", function () { setMode(i); }); modes.lastChild.appendChild(b); return b;
        });
        var partsel = document.createElement("div"); partsel.className = "partsel"; partsel.setAttribute("role", "radiogroup"); partsel.setAttribute("aria-label", "Panel select");
        partsel.innerHTML = '<div class="group-label">Panel Select <span class="hint"><i></i> other part</span></div><div class="seg"></div>';
        el.partBtns = ["Upper", "Lower", "Both"].map(function (n, i) {
            var b = mkBtn(n, ""); b.setAttribute("role", "radio"); b.setAttribute("aria-checked", "false"); b.innerHTML = '<span class="led"></span>' + n;
            b.addEventListener("click", function () { setPart(i); }); partsel.lastChild.appendChild(b); return b;
        });
        el.partsel = partsel;
        side.appendChild(modes); side.appendChild(partsel);

        head.appendChild(brand); head.appendChild(lcdWrap); head.appendChild(side);
        panel.appendChild(head);

        /* tabs */
        var tabs = document.createElement("div"); tabs.className = "tabs"; tabs.setAttribute("role", "tablist"); tabs.setAttribute("aria-label", "Views");
        el.tabBtns = M.TABS.map(function (t) {
            var b = document.createElement("button"); b.type = "button"; b.setAttribute("role", "tab"); b.dataset.tab = t.id;
            b.textContent = t.label; b.setAttribute("aria-selected", "false"); b.tabIndex = -1;
            b.addEventListener("click", function () { setTab(t.id); });
            tabs.appendChild(b); return b;
        });
        tabs.addEventListener("keydown", tabKeys);
        panel.appendChild(tabs);

        /* browser */
        var browser = document.createElement("div"); browser.className = "browser"; browser.setAttribute("role", "region"); browser.setAttribute("aria-label", "Preset browser");
        var bar = document.createElement("div"); bar.className = "bbar";
        var title = document.createElement("span"); title.className = "title"; title.textContent = "Patches"; el.browserTitle = title;
        var search = document.createElement("input"); search.type = "search"; search.placeholder = "Search every bank: name, A23, or 17"; search.setAttribute("aria-label", "Search presets in every bank");
        search.addEventListener("input", function () { B.q = search.value.trim().toLowerCase(); refreshBrowser(); });
        var favBtn = document.createElement("button"); favBtn.type = "button"; favBtn.className = "chip fav"; favBtn.textContent = "★ Favourites"; favBtn.setAttribute("aria-pressed", "false");
        favBtn.addEventListener("click", function () { B.favsOnly = !B.favsOnly; refreshBrowser(); });
        var rnd = document.createElement("button"); rnd.type = "button"; rnd.className = "chip"; rnd.textContent = "⚄ Random"; rnd.title = "Load a preset at random from what is listed";
        rnd.addEventListener("click", randomRow);
        var chips = document.createElement("div"); chips.className = "chips"; chips.setAttribute("role", "group"); chips.setAttribute("aria-label", "Filter by name prefix");
        bar.appendChild(title); bar.appendChild(search); bar.appendChild(favBtn); bar.appendChild(rnd);
        var panes = document.createElement("div"); panes.className = "bpanes";
        var banksUl = document.createElement("ul"); banksUl.className = "banks"; banksUl.setAttribute("aria-label", "Banks");
        var right = document.createElement("div"); right.className = "presets";
        var bsel = document.createElement("select"); bsel.className = "bank-select"; bsel.setAttribute("aria-label", "Bank");
        bsel.addEventListener("change", function () { selectBank(parseInt(bsel.value, 10)); });
        var bhead = document.createElement("div"); bhead.className = "bhead";
        var bankName = document.createElement("span"); bankName.className = "bname";
        var count = document.createElement("span"); count.className = "count";
        var sounding = document.createElement("span"); sounding.className = "sounding"; sounding.hidden = true;
        var partHint = document.createElement("span"); partHint.className = "parthint";
        bhead.appendChild(bankName); bhead.appendChild(count); bhead.appendChild(sounding); bhead.appendChild(partHint);
        var grid = document.createElement("div"); grid.className = "pgrid"; grid.setAttribute("role", "grid");
        grid.addEventListener("keydown", gridKeys);
        right.appendChild(bsel); right.appendChild(chips); right.appendChild(bhead); right.appendChild(grid);
        panes.appendChild(banksUl); panes.appendChild(right);
        browser.appendChild(bar); browser.appendChild(panes);
        el.browser = browser; el.bankSel = bsel; el.banks = banksUl; el.search = search; el.chips = chips; el.favBtn = favBtn;
        el.count = count; el.bankName = bankName; el.sounding = sounding; el.partHint = partHint; el.grid = grid;
        browse.addEventListener("click", function () { setTab("presets"); });
        document.addEventListener("keydown", function (e) {
            if (e.key === "/" && !/^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement.tagName) && !browser.hidden) { e.preventDefault(); search.focus(); }
        });
        panel.appendChild(browser);

        /* the rows */
        var rows = document.createElement("div"); rows.className = "rows"; el.rows = rows;
        /* Column shares per hardware row. Two sets: the wide one for a pop-out
         * window or a big screen, the medium one for the page as Schwung Manager
         * embeds it (its column is capped at 1200px, so the iframe is ~1084px on a
         * Mac and ~908px on an iPad in landscape). Under 1000px the rows dissolve
         * into a two-column flow (style.css). */
        var COLS = { "row-top": "1.45fr 1.95fr 1.6fr", "row-main": "1fr 1.65fr 2.6fr 1.75fr", "row-lower": "1.9fr 0.9fr 1fr 1.6fr", "row-left": "1.9fr 1.1fr 1.7fr", "row-parts": "2fr 1.4fr" };
        var COLS_MD = { "row-top": "1.35fr 1.85fr 1.8fr", "row-main": "1.3fr 1.5fr 2.3fr 1.9fr", "row-lower": "2fr 1fr 1.1fr 1.7fr", "row-left": "1.9fr 1.2fr 1.8fr", "row-parts": "2fr 1.4fr" };
        var sectionNo = 0;
        M.PANEL.forEach(function (row) {
            var r = document.createElement("div"); r.className = "row"; r.id = row.id;
            if (COLS[row.id]) { r.style.setProperty("--cols", COLS[row.id]); r.style.setProperty("--cols-md", COLS_MD[row.id]); }
            row.sections.forEach(function (sec) {
                sectionNo++;
                var s = document.createElement("section"); s.className = "section"; s.id = "sec-" + sec.id;
                var h = document.createElement("h2");
                if (sec.num) { var nn = document.createElement("span"); nn.className = "num"; nn.textContent = (sec.num < 10 ? "0" : "") + sec.num; nn.title = "Section " + sec.num + " in the Owner's Manual"; h.appendChild(nn); }
                h.appendChild(document.createTextNode(sec.title)); s.appendChild(h);
                if (sec.viz) { s.classList.add("has-viz"); s.appendChild(vizFor(sec.viz)); }
                var cells = document.createElement("div"); cells.className = "cells";
                if (sec.parts) buildParts(cells, sec);
                else {
                    var env = null;
                    sec.items.forEach(function (it) {
                        var w = null;
                        switch (it.widget) {
                            case "knob": w = knobWidget(it.key, it); break;
                            case "slider":
                                w = sliderWidget(it.key, it);
                                if (!env) { env = document.createElement("div"); env.className = "env"; env.setAttribute("role", "group"); env.setAttribute("aria-label", "Envelope"); cells.appendChild(env); }
                                env.appendChild(w.el); W[it.key] = w; return;
                            case "leds": w = ledsWidget(it.key, it); break;
                            case "switch": w = switchWidget(it.key, it); break;
                            case "select": w = selectWidget(it.key, it); break;
                            case "drawer": cells.appendChild(drawerFor(it)); return;
                            case "lfo2depth": {
                                var ld = lfo2DepthWidget(it);
                                ld.knobs.forEach(function (k, i) { W[it.keys[i]] = k; });
                                var sel = W.lfo2_depth_select;
                                if (sel) { var oset = sel.set; sel.set = function (v, g, pnd) { oset(v, g, pnd); ld.show(); }; }
                                cells.appendChild(ld.el); return;
                            }
                        }
                        if (w) { W[it.key] = w; cells.appendChild(w.el); }
                    });
                }
                s.appendChild(cells);
                r.appendChild(s);
            });
            rows.appendChild(r);
        });
        panel.appendChild(rows);

        /* system */
        var system = document.createElement("div"); system.className = "system"; system.hidden = true; el.system = system;
        M.SYSTEM.forEach(function (g) {
            var s = document.createElement("section"); s.className = "section";
            var h = document.createElement("h2"); h.textContent = g.title; s.appendChild(h); s.appendChild(document.createElement("div"));
            var cells = document.createElement("div"); cells.className = "cells";
            g.keys.forEach(function (k) {
                var w;
                if (k === "buffer_ms") w = bufferWidget();
                else if (byKey[k].options) w = selectWidget(k, { label: byKey[k].name });
                else w = inlineKnob(k);
                W[k] = w; cells.appendChild(w.el);
            });
            s.appendChild(cells); system.appendChild(s);
        });
        var note = document.createElement("p"); note.className = "note";
        note.textContent = "System settings live outside the performance: they survive a patch change and are saved with this slot. Remote Ctrl Ch is the channel the arpeggiator listens on.";
        system.appendChild(note);
        panel.appendChild(system);

        var foot = document.createElement("div"); foot.className = "foot";
        foot.innerHTML = '<span>Drag a knob up or down. <span class="kbd">Shift</span> for fine steps, double-click to reset, arrow keys when focused.</span><span>Roland JP-8000 panel order, sections 1–22 of the Owner’s Manual.</span>';
        panel.appendChild(foot);

        el.panel = panel;
        refreshHeader(); refreshModes(); applyTab();
    }

    function mkBtn(text, cls) { var b = document.createElement("button"); b.type = "button"; b.className = "btn " + cls; b.textContent = text; return b; }
    function setBusy(b) { el.panel.setAttribute("aria-busy", String(b)); }

    function inlineKnob(k) {
        var p = byKey[k];
        var c = document.createElement("div"); c.className = "cell";
        var l = document.createElement("div"); l.className = "label"; l.textContent = p.name;
        var inline = document.createElement("div"); inline.className = "inline";
        var kw = knobWidget(k, { label: p.name });
        kw.el.querySelector(".label").remove();
        inline.appendChild(kw.el);
        c.appendChild(l); c.appendChild(inline);
        return { el: c, set: kw.set };
    }

    /* Audio Buffer is the module's, not the firmware's: a plain integer param. */
    function bufferWidget() {
        var meta = (JP.controls || []).filter(function (x) { return x.key === "buffer_ms"; })[0] || { min: 4, max: 90, default: 10, name: "Audio Buffer", unit: "ms" };
        var p = { key: "buffer_ms", min: meta.min, max: meta.max, default: meta.default, name: meta.name, unit: meta.unit, off: 0, neg: false };
        var c = document.createElement("div"); c.className = "cell";
        var l = document.createElement("div"); l.className = "label"; l.textContent = "Audio buffer (this slot's latency)";
        var inline = document.createElement("div"); inline.className = "inline";
        var val = document.createElement("span"); val.className = "value";
        var k = document.createElement("div"); k.className = "knob"; k.tabIndex = 0; k.setAttribute("role", "slider"); k.setAttribute("aria-label", "Audio buffer in milliseconds");
        k.setAttribute("aria-valuemin", p.min); k.setAttribute("aria-valuemax", p.max);
        var svg = svgEl("svg", { viewBox: "0 0 64 64" });
        var track = svgEl("path", { class: "track", d: arcPath(32, 32, 27, -135, 135) }), arcGlow = svgEl("path", { class: "arcglow" }), arc = svgEl("path", { class: "arc" });
        svg.appendChild(track); svg.appendChild(arcGlow); svg.appendChild(arc); svg.appendChild(svgEl("circle", { class: "cap", cx: 32, cy: 32, r: 21 }));
        var ptr = svgEl("line", { class: "ptr", x1: 32, y1: 32, x2: 32, y2: 14 }); svg.appendChild(ptr); k.appendChild(svg);
        var cur = p.default;
        function set(v) { if (v === null || v === undefined || isNaN(v)) { val.textContent = "--"; return; } cur = v; var a = -135 + 270 * (v - p.min) / (p.max - p.min); ptr.style.transform = "rotate(" + a + "deg)"; arc.setAttribute("d", arcPath(32, 32, 27, -135, a)); arcGlow.setAttribute("d", arcPath(32, 32, 27, -135, a)); val.textContent = v + " ms"; k.setAttribute("aria-valuenow", v); }
        bindRange(k, p, function () { return cur; }, function (v) { v = Math.max(p.min, Math.min(p.max, Math.round(v))); S.buffer_ms = v; set(v); setParam("buffer_ms", v); }, 140);
        inline.appendChild(k); inline.appendChild(val); c.appendChild(l); c.appendChild(inline);
        return { el: c, set: set };
    }

    function buildParts(cells, sec) {
        var grid = document.createElement("div"); grid.className = "parts-grid";
        [0, 1].forEach(function (part) {
            var col = document.createElement("div");
            var h = document.createElement("h3"); h.innerHTML = '<span class="led"></span>' + (part ? "Lower" : "Upper");
            col.appendChild(h);
            var cc = document.createElement("div"); cc.className = "cells";
            sec.items.forEach(function (it) {
                var key = M.partKey(it, part), w;
                if (it.widget === "select") w = selectWidget(key, { label: it.label });
                else w = knobWidget(key, { label: it.label });
                W[key] = w; cc.appendChild(w.el);
            });
            col.appendChild(cc); grid.appendChild(col);
        });
        cells.appendChild(grid);
        cells.style.display = "block";
    }

    function drawerFor(item) {
        var d = document.createElement("details"); d.className = "drawer";
        var sm = document.createElement("summary"); sm.textContent = item.label + " — 40 depths"; d.appendChild(sm);
        var mx = document.createElement("div"); mx.className = "matrix";
        M.drawerGroups(item.prefix).forEach(function (g) {
            var gd = document.createElement("div"); gd.className = "mgroup";
            var h = document.createElement("h3"); h.textContent = g.title; gd.appendChild(h);
            var cc = document.createElement("div"); cc.className = "cells";
            g.keys.forEach(function (k) {
                var w = knobWidget(k, { label: byKey[k].name.replace(/^(Ctrl|Vel) /, "") });
                W[k] = w; cc.appendChild(w.el);
            });
            gd.appendChild(cc); mx.appendChild(gd);
        });
        d.appendChild(mx);
        return d;
    }

    /* ---- scopes --------------------------------------------------------------------
     * Small phosphor displays: a fine grid, a glowing trace, an area fill under
     * it. The LFO trace runs at the LFO's own rate while its tab is showing
     * (and stands still for prefers-reduced-motion). */
    function vizFor(id) {
        if (id === "filter") {
            var wrap = document.createElement("div"); wrap.className = "viz-row";
            wrap.appendChild(mkViz("filter")); wrap.appendChild(mkViz("fenv"));
            return wrap;
        }
        return mkViz(id);
    }
    /* The canvas sits ABSOLUTELY inside a box that owns the size. A canvas's
     * intrinsic height is its backing store's, which ctx2d sets to the CSS
     * height times devicePixelRatio -- so a canvas that is itself the grid item
     * feeds 2x its height back into an auto track on a Retina screen and the
     * display grows on every redraw, forever. Out of the flow, it cannot. */
    function mkViz(id) {
        var box = document.createElement("div"); box.className = "vizbox";
        var c = document.createElement("canvas"); c.className = "viz"; c.setAttribute("aria-hidden", "true");
        box.appendChild(c);
        vizByGroup[id] = c;
        return box;
    }
    /* A knob drag redraws the display(s) that READ that key, not the panel.
     *
     * Each display records, while it draws, every parameter it read through
     * v() / vOther(); redrawViz(keys) then redraws only the displays whose last
     * drawing depended on one of those keys. A display never drawn (its tab was
     * hidden) has no record and is drawn when asked, which records it. With no
     * argument -- a dump, a tab change, a resize -- everything is drawn. This
     * took a drag frame from fifteen displays to one. */
    var vizRaf = 0, vizPending = null;      // null: nothing queued; true: everything; {key:1}: those
    var vizDeps = {}, depTrack = null;      // display id -> keys read during its last draw
    function redrawViz(keys) {
        if (!keys) vizPending = true;
        else if (vizPending !== true) { vizPending = vizPending || {}; for (var i = 0; i < keys.length; i++) vizPending[keys[i]] = 1; }
        if (vizRaf) return;
        vizRaf = requestAnimationFrame(function () { vizRaf = 0; var q = vizPending; vizPending = null; if (q === true) drawAllViz(); else if (q) drawVizFor(q); });
    }
    function drawVizFor(keys) {
        if (!S.temp) return;
        for (var id in DRAW) {
            var d = vizDeps[id];
            if (!d) { drawViz(id); continue; }
            for (var k in keys) if (d[k]) { drawViz(id); break; }
        }
    }
    function drawViz(id) {
        var c = vizByGroup[id]; if (!c || !visible(c)) return;
        depTrack = vizDeps[id] = {};
        try { DRAW[id](c); } finally { depTrack = null; }
    }
    function visible(c) { return c.parentNode.offsetParent !== null && c.parentNode.clientWidth > 0; }
    function ctx2d(c) {
        var box = c.parentNode, dpr = window.devicePixelRatio || 1, w = box.clientWidth || 200, h = box.clientHeight || 56;
        if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
        var g = c.__g || (c.__g = c.getContext("2d")); g.setTransform(dpr, 0, 0, dpr, 0, 0); g.clearRect(0, 0, w, h);
        return { g: g, w: w, h: h };
    }
    var AMBER = "#f2b13d", AMBER_DIM = "rgba(242,177,61,0.45)", GRID = "rgba(242,177,61,0.09)", GRID2 = "rgba(242,177,61,0.16)";
    var MONO = null;
    function mono() { return MONO || (MONO = getComputedStyle(document.body).getPropertyValue("--font-mono")); }
    function grid(g, w, h) {
        g.strokeStyle = GRID; g.lineWidth = 1; g.beginPath();
        for (var x = 0; x <= w; x += w / 16) { g.moveTo(Math.round(x) + 0.5, 0); g.lineTo(Math.round(x) + 0.5, h); }
        for (var y = 0; y <= h; y += h / 4) { g.moveTo(0, Math.round(y) + 0.5); g.lineTo(w, Math.round(y) + 0.5); }
        g.stroke();
        g.strokeStyle = GRID2; g.beginPath(); g.moveTo(0, Math.round(h / 2) + 0.5); g.lineTo(w, Math.round(h / 2) + 0.5); g.stroke();
    }
    /* Curves are built ONCE into a Path2D and reused by the fill and both
     * strokes; the per-pixel loops used to run three times per display.
     * shadowBlur is the slowest primitive the 2D context has, so while a finger
     * is down the glow is a wide translucent stroke instead. */
    function shadow(g, col, blur) { if (draggingNow()) return; g.shadowColor = col; g.shadowBlur = blur; }
    function glowStroke(g, P) {
        g.save(); g.lineJoin = "round"; g.lineCap = "round";
        if (draggingNow()) { g.strokeStyle = "rgba(242,177,61,0.22)"; g.lineWidth = 5; g.stroke(P); }
        else shadow(g, "rgba(242,177,61,0.85)", 7);
        g.strokeStyle = AMBER; g.lineWidth = 1.7; g.stroke(P); g.restore();
        g.strokeStyle = "#fff0cf"; g.lineWidth = 0.6; g.globalAlpha = 0.55; g.stroke(P); g.globalAlpha = 1;
    }
    function areaFill(g, w, h, P, base) {
        var grad = g.createLinearGradient(0, 0, 0, h); grad.addColorStop(0, "rgba(242,177,61,0.28)"); grad.addColorStop(1, "rgba(242,177,61,0.02)");
        var A = new Path2D(P); A.lineTo(w, base); A.lineTo(0, base); A.closePath(); g.fillStyle = grad; g.fill(A);
    }
    function label(g, h, text) { g.fillStyle = AMBER_DIM; g.font = "600 9px " + mono(); g.fillText(text, 6, h - 5); }
    function v(key, d) { if (depTrack) depTrack[key] = 1; var x = valueOf(key); return x === null || x === undefined ? d : x; }
    /* a patch parameter read for the OTHER part, when both parts sound */
    function bothPartsSound() { return v("key_mode", 0) !== 0; }
    function vOther(key, d) { if (depTrack) depTrack[key] = 1; if (!S.temp) return d; var x = M.readParam(S.temp, byKey[key], otherPart()); return x === null ? d : x; }
    var GHOST = "#7fb9ff", GHOST_DIM = "rgba(127,185,255,0.55)";

    function drawEnv(c, a, dd, s, r, text) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var pad = 6, W0 = w - 2 * pad, H0 = h - 2 * pad - 8;
        var ta = 0.05 + a / 127 * 0.3, td = 0.05 + dd / 127 * 0.3, tr = 0.05 + r / 127 * 0.3, hold = 0.2;
        var tot = ta + td + hold + tr;
        var x0 = pad, x1 = x0 + ta / tot * W0, x2 = x1 + td / tot * W0, x3 = x2 + hold / tot * W0, x4 = x3 + tr / tot * W0;
        var yb = pad + H0, yt = pad + 2, ys = yb - s / 127 * (H0 - 2);
        var P = new Path2D(); P.moveTo(x0, yb); P.lineTo(x1, yt); P.lineTo(x2, ys); P.lineTo(x3, ys); P.lineTo(x4, yb);
        areaFill(g, w, h, P, yb);
        glowStroke(g, P);
        /* the gate: where the key goes up */
        g.strokeStyle = GRID2; g.setLineDash([2, 3]); g.beginPath(); g.moveTo(x3 + 0.5, pad); g.lineTo(x3 + 0.5, yb); g.stroke(); g.setLineDash([]);
        label(g, h, text);
    }
    var lfoPhase = 0;
    /* ---- the other displays ------------------------------------------------------
     * Every section carries one, and it grows to fill whatever height the row
     * gives the section, so a row of uneven sections reads as a row of displays
     * rather than a row of gaps. Each draws something true about its section. */
    var lfo2Phase = 0;
    function wavePath(g, w, h, shape, ctrl1, ctrl2, cycles, phase, amp) {
        /* shape names follow the option strings so OSC 1, OSC 2 and the LFOs share it */
        var mid = h / 2, A = (amp === undefined ? 1 : amp) * (h / 2 - 8);
        function tri(ph) { return 1 - Math.abs(ph * 4 - 2); }
        function saw(ph) { return 1 - ph * 2; }
        function pulse(ph, pw) { return ph < pw ? 1 : -1; }
        var seed = 12345;
        function rnd() { seed = (seed * 9301 + 49297) % 233280; return seed / 233280; }
        var noise = []; for (var i = 0; i <= w; i++) noise.push(rnd() * 2 - 1);
        for (var x = 0; x <= w; x++) {
            var t = x / w * cycles + (phase || 0), ph = t - Math.floor(t), y0;
            switch (shape) {
                case "SUPER SAW": {   // seven detuned saws, the detune from Control 1, mix from Control 2
                    var det = 0.02 + ctrl1 / 127 * 0.16, mix = 0.3 + ctrl2 / 127 * 0.7, sum = saw(ph);
                    for (var k = 1; k <= 3; k++) { var d = det * k; sum += mix * (saw((t * (1 + d)) % 1) + saw((t * (1 - d)) % 1)) / 2; }
                    y0 = sum / (1 + 3 * mix); break;
                }
                case "TRIANGLE MOD": { var m = 1 + ctrl1 / 127 * 6; y0 = Math.sin(Math.PI * tri(ph) / 2 * m) / Math.abs(Math.sin(Math.PI * m / 2) || 1); y0 = Math.max(-1, Math.min(1, y0)); break; }
                case "NOISE": y0 = noise[x] * 0.9; break;
                case "FEEDBACK OSC": { var fb = ctrl1 / 127; y0 = saw(ph) * (1 - fb * 0.5) + fb * 0.5 * Math.sin(ph * Math.PI * 2 * (2 + Math.round(fb * 6))); break; }
                case "SQR (PWM)": case "SQUARE": y0 = pulse(ph, shape === "SQUARE" ? 0.5 : 0.5 - ctrl1 / 127 * 0.45); break;
                case "SAW": case "SAWTOOTH": y0 = saw(ph); break;
                case "RANDOM": y0 = noise[Math.floor(t) % noise.length]; break;
                default: y0 = tri(ph);
            }
            var y = mid - y0 * A;
            if (x === 0) g.moveTo(x, y); else g.lineTo(x, y);
        }
    }
    function drawOsc(c, n) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var p = byKey[n === 1 ? "osc1_waveform" : "osc2_waveform"], shape = p.options[v(p.key, 0)] || "SAW";
        var c1 = v(n === 1 ? "osc1_ctrl1" : "osc2_ctrl1", 0), c2 = v(n === 1 ? "osc1_ctrl2" : "osc2_ctrl2", 0);
        var P = new Path2D(); wavePath(P, w, h, shape, c1, c2, 2, 0); glowStroke(g, P);
        var txt = shape;
        if (n === 2) { var rg = v("osc2_range", 0), fn = v("osc2_fine", 0); txt += "   " + (rg > 0 ? "+" : "") + rg + " st " + (fn > 0 ? "+" : "") + fn + (v("osc2_sync", 0) ? "  SYNC" : ""); }
        label(g, h, txt);
    }
    function drawLfoScope(c, shape, rate, fade, phase, text) {
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var cycles = 1.5 + rate * 6;
        var P = new Path2D();
        {
            var mid = h / 2, A = h / 2 - 8, seed = 7;
            for (var x = 0; x <= w; x++) {
                var t = x / w * cycles + phase, ph = t - Math.floor(t), y0;
                if (shape === 0) y0 = 1 - Math.abs(ph * 4 - 2);
                else if (shape === 1) y0 = 1 - ph * 2;
                else if (shape === 2) y0 = ph < 0.5 ? 1 : -1;
                else { var k = Math.floor(t); seed = ((k * 9301 + 49297) % 233280); y0 = (seed / 233280) * 2 - 1; }
                var ampF = fade > 0 ? Math.min(1, (x / w) / (0.15 + fade * 0.85)) : 1;
                var y = mid - y0 * ampF * A;
                if (x === 0) P.moveTo(x, y); else P.lineTo(x, y);
            }
        }
        glowStroke(g, P);
        label(g, h, text);
    }
    var animRaf = 0, animLast = 0;
    function drawLfo1(c) {
        var r1 = v("lfo1_rate", 64) / 127;
        drawLfoScope(c, v("lfo1_waveform", 0), r1, v("lfo1_fade", 0) / 127, lfoPhase, ["TRI", "SAW", "SQR", "RND"][v("lfo1_waveform", 0)] + "  " + v("lfo1_rate", 64));
    }
    function drawLfo2(c) {
        var r2 = v("lfo2_rate", 64) / 127, dest = v("lfo2_depth_select", 0);
        var depthKey = ["pitch_lfo2_depth", "filter_lfo2_depth", "amp_lfo2_depth"][dest];
        drawLfoScope(c, 0, r2, 0, lfo2Phase, "TRI  " + v("lfo2_rate", 64) + "  \u2192 " + ["PITCH", "FILTER", "AMP"][dest] + " " + (v(depthKey, 0) > 0 ? "+" : "") + v(depthKey, 0));
    }
    /* The two scopes move at 30 fps, and stand still while a finger is on a
     * control: a drag frame is not the moment to redraw two wide displays. */
    function animateScopes(now) {
        animRaf = 0;
        if (!S.temp) return;
        var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
        var dt = animLast ? (now - animLast) / 1000 : 0;
        if (draggingNow() || (animLast && dt < 1 / 32)) { if (draggingNow()) animLast = now; animRaf = requestAnimationFrame(animateScopes); return; }
        animLast = now;
        var any = false;
        if (vizByGroup.lfo1 && visible(vizByGroup.lfo1)) { if (!reduce) lfoPhase += dt * (0.15 + v("lfo1_rate", 64) / 127 * 2.2); drawViz("lfo1"); any = true; }
        if (vizByGroup.lfo2 && visible(vizByGroup.lfo2)) { if (!reduce) lfo2Phase += dt * (0.15 + v("lfo2_rate", 64) / 127 * 2.2); drawViz("lfo2"); any = true; }
        if (any && !reduce && T.cur !== "presets" && T.cur !== "system") animRaf = requestAnimationFrame(animateScopes);
    }
    function drawTone(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var bass = v("tone_bass", 0) / 64, treb = v("tone_treble", 0) / 64, mid = h / 2, A = h / 2 - 9;
        var P = new Path2D(); for (var x = 0; x <= w; x++) { var f = x / w; var lo = 1 / (1 + Math.exp((f - 0.28) * 18)), hi = 1 / (1 + Math.exp(-(f - 0.7) * 18)); var y = mid - (bass * lo + treb * hi) * A; if (x === 0) P.moveTo(x, y); else P.lineTo(x, y); }
        areaFill(g, w, h, P, mid);
        glowStroke(g, P);
        label(g, h, "BASS " + (bass > 0 ? "+" : "") + v("tone_bass", 0) + "   TREBLE " + (treb > 0 ? "+" : "") + v("tone_treble", 0));
    }
    function drawChorus(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var type = v("chorus_type", 0), lvl = v("chorus_level", 0) / 127, mid = h / 2, A = (h / 2 - 9);
        var spread = 0.05 + lvl * 0.35, rateF = [1.5, 2.5, 4, 2, 1.2, 1.6, 3.5, 1, 3, 2.2, 0.8, 0.9][type] || 2;
        [-1, 0, 1].forEach(function (k, i) {
            g.globalAlpha = i === 1 ? 1 : 0.55;
            var P = new Path2D(); for (var x = 0; x <= w; x++) { var t = x / w; var y = mid - Math.sin(t * Math.PI * 2 * rateF + k * spread * Math.PI) * A * (0.55 + lvl * 0.45); if (x === 0) P.moveTo(x, y); else P.lineTo(x, y); }
            glowStroke(g, P);
        });
        g.globalAlpha = 1;
        var ctxt = byKey.chorus_type.options[type] + "   " + v("chorus_level", 0);
        if (bothPartsSound()) ctxt += "     " + (otherPart() ? "LOWER" : "UPPER") + " part: " + byKey.chorus_type.options[vOther("chorus_type", 0)] + " " + vOther("chorus_level", 0);
        label(g, h, ctxt);
    }
    function drawEchoes(g, w, h, type, time, fb, lvl, color, glow, xoff) {
        var mid = h / 2, x = 10 + (xoff || 0), gap = 14 + time * (w * 0.22), amp = 1, n = 0, pan = type <= 2;
        g.save(); shadow(g, glow, 6); g.fillStyle = color;
        if (!xoff) g.fillRect(x - 1.5, mid - (h / 2 - 9), 3, (h - 18));                  // the dry hit
        while (x + gap < w - 6 && n < 12) {
            x += gap; n++; amp *= (n === 1 ? lvl : Math.max(0.15, fb));
            if (amp < 0.03) break;
            var hh = (h / 2 - 10) * amp, side = pan ? (((n + (type === 1 ? 1 : 0)) % 2) ? -1 : 1) : 0;
            g.globalAlpha = 0.45 + 0.55 * amp;
            if (side === 0) g.fillRect(x - 1.5, mid - hh, 3, hh * 2);
            else if (side > 0) g.fillRect(x - 1.5, mid - hh, 3, hh); else g.fillRect(x - 1.5, mid, 3, hh);
        }
        g.restore(); g.globalAlpha = 1;
    }
    /* Both parts have their own delay and chorus, and in Dual or Split both
     * sound. The panel edits one part at a time, so the OTHER part's echoes are
     * drawn too, in the ghost colour -- an echo you hear with the level at 0 is
     * almost always the other part's, and this is where it shows. */
    function drawDelay(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var other = bothPartsSound();
        if (other && vOther("delay_level", 0) > 0)
            drawEchoes(g, w, h, vOther("delay_type", 0), vOther("delay_time", 64) / 127, vOther("delay_feedback", 0) / 127, vOther("delay_level", 0) / 127, GHOST, "rgba(127,185,255,0.8)", 3);
        drawEchoes(g, w, h, v("delay_type", 0), v("delay_time", 64) / 127, v("delay_feedback", 0) / 127, v("delay_level", 0) / 127, AMBER, "rgba(242,177,61,0.8)", 0);
        var txt = byKey.delay_type.options[v("delay_type", 0)] + "   " + v("delay_time", 64) + " / " + v("delay_feedback", 0) + " / " + v("delay_level", 0);
        if (other) txt += "     " + (otherPart() ? "LOWER" : "UPPER") + " part: level " + vOther("delay_level", 0);
        label(g, h, txt);
    }
    function drawKeys(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h;
        var mode = v("key_mode", 0), split = v("split_point", 60), lo = 36, hi = 96;   // C2..C7 on the strip
        var nWhite = 0, isBlack = [0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0];
        for (var n = lo; n <= hi; n++) if (!isBlack[n % 12]) nWhite++;
        var kw = (w - 8) / nWhite, x0 = 4, top = 6, bot = h - 16, wi = 0;
        var xOf = {};
        for (var n2 = lo; n2 <= hi; n2++) { if (!isBlack[n2 % 12]) { xOf[n2] = x0 + wi * kw; wi++; } }
        function zone(n3) { if (mode === 2) return n3 < split ? "lo" : "up"; if (mode === 1) return "both"; return "up"; }
        var col = { up: "rgba(242,177,61,0.85)", lo: "rgba(127,185,255,0.85)", both: "rgba(190,181,158,0.85)" };
        for (var n4 = lo; n4 <= hi; n4++) {
            if (isBlack[n4 % 12]) continue;
            g.fillStyle = col[zone(n4)]; g.globalAlpha = 0.35; g.fillRect(xOf[n4] + 0.5, top, kw - 1, bot - top);
            g.globalAlpha = 1; g.fillStyle = "rgba(0,0,0,0.35)"; g.fillRect(xOf[n4] + kw - 1, top, 1, bot - top);
        }
        for (var n5 = lo; n5 <= hi; n5++) {
            if (!isBlack[n5 % 12]) continue;
            var xb = xOf[n5 - 1] + kw * 0.65;
            g.fillStyle = "#14100a"; g.fillRect(xb, top, kw * 0.7, (bot - top) * 0.6);
            g.fillStyle = col[zone(n5)]; g.globalAlpha = 0.5; g.fillRect(xb, top, kw * 0.7, (bot - top) * 0.6); g.globalAlpha = 1;
        }
        if (mode === 2 && xOf[split] !== undefined || (mode === 2 && split >= lo && split <= hi)) {
            var xs = xOf[split] !== undefined ? xOf[split] : xOf[split - 1] + kw * 0.65;
            g.save(); shadow(g, "rgba(255,255,255,0.8)", 5); g.fillStyle = "#fff"; g.fillRect(Math.round(xs) - 1, top - 2, 2, bot - top + 4); g.restore();
        }
        label(g, h, ["SINGLE", "DUAL  upper + lower", "SPLIT at " + M.noteName(split) + "  lower | upper"][mode] + "   voices " + byKey.voice_assign.options[v("voice_assign", 0)]);
    }
    function drawArp(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var on = v("arp_switch", 0), mode = v("arp_mode", 0), range = v("arp_range", 0) + 1, beatName = byKey.arp_beat.options[v("arp_beat", 0)] || "";
        var m = /NORMAL 1\/(\d+)/.exec(beatName), steps = m ? Math.min(32, parseInt(m[1], 10)) : 16;
        var pad = 8, sw = (w - 2 * pad) / steps, top = 8, bot = h - 16, seed = 3;
        g.globalAlpha = on ? 1 : 0.35;
        for (var i = 0; i < steps; i++) {
            var pos = i / Math.max(1, steps - 1), lvl;
            if (mode === 0) lvl = pos; else if (mode === 1) lvl = 1 - pos; else if (mode === 2) lvl = 1 - Math.abs(pos * 2 - 1);
            else if (mode === 3) { seed = (seed * 9301 + 49297) % 233280; lvl = seed / 233280; } else lvl = 0.5;
            var y = bot - lvl * (bot - top) * Math.min(1, range / 3), x = pad + i * sw + sw / 2;
            g.save(); shadow(g, "rgba(242,177,61,0.8)", 5); g.fillStyle = mode === 4 ? AMBER_DIM : AMBER;
            g.beginPath(); g.arc(x, y, Math.min(3, sw * 0.3), 0, Math.PI * 2); g.fill(); g.restore();
            g.strokeStyle = GRID2; g.beginPath(); g.moveTo(x, y + 3); g.lineTo(x, bot); g.stroke();
        }
        g.globalAlpha = 1;
        label(g, h, (on ? "ON  " : "OFF  ") + byKey.arp_mode.options[mode] + "  " + beatName + "  " + range + " OCT  " + v("tempo", 120) + " BPM");
    }
    function drawBend(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var up = v("bend_up", 2), down = v("bend_down", -2), mid = h / 2, cx = w / 2, unit = (w - 16) / 48;
        var xl = cx + down * unit, xr = cx + up * unit;
        var grad = g.createLinearGradient(xl, 0, xr, 0); grad.addColorStop(0, "rgba(127,185,255,0.35)"); grad.addColorStop(0.5, "rgba(242,177,61,0.15)"); grad.addColorStop(1, "rgba(242,177,61,0.4)");
        g.fillStyle = grad; g.fillRect(xl, mid - 10, xr - xl, 20);
        g.strokeStyle = GRID2; g.beginPath(); for (var s = -24; s <= 24; s += 12) { var x = cx + s * unit; g.moveTo(x + 0.5, mid - 14); g.lineTo(x + 0.5, mid + 14); } g.stroke();
        g.save(); shadow(g, "rgba(242,177,61,0.8)", 5); g.fillStyle = AMBER; g.fillRect(Math.round(xr) - 1, mid - 12, 2, 24); g.fillStyle = "#7fb9ff"; shadow(g, "rgba(127,185,255,0.8)", 5); g.fillRect(Math.round(xl) - 1, mid - 12, 2, 24); g.restore();
        g.fillStyle = "#fff"; g.fillRect(cx - 0.5, mid - 6, 1, 12);
        label(g, h, "BEND " + down + " / +" + up + " st" + (v("morph_bend", 0) ? "   MORPH" : "") + "   " + (v("active_bender", 0) ? "bender" : "") + " " + (v("active_control", 0) ? "ribbon" : "") + " " + (v("active_velocity", 0) ? "velocity" : ""));
    }
    function drawPorta(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var on = v("portamento", 0), time = v("portamento_time", 0) / 127, y1 = h - 14, y2 = 10, x1 = w * 0.3, glide = on ? 8 + time * (w * 0.55) : 2;
        var P = new Path2D(); P.moveTo(6, y1); P.lineTo(x1, y1); P.bezierCurveTo(x1 + glide * 0.4, y1, x1 + glide * 0.6, y2, x1 + glide, y2); P.lineTo(w - 6, y2);
        glowStroke(g, P);
        var shift = byKey.osc_shift.options[v("osc_shift", 2)];
        label(g, h, (on ? "PORTAMENTO " + v("portamento_time", 0) : "PORTAMENTO OFF") + (v("mono", 0) ? "   MONO" : "   POLY") + (v("legato", 0) ? " LEGATO" : "") + "   OSC SHIFT " + shift);
    }

    function drawParts(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        /* two bars, one per part, offset by transpose: the split's pitch picture */
        var mid = h / 2, unit = (w - 24) / 48;
        [["up", v("up_transpose", 0), v("up_midi_ch", 0), "rgba(242,177,61,", mid - 12], ["lo", v("lo_transpose", 0), v("lo_midi_ch", 0), "rgba(127,185,255,", mid + 4]].forEach(function (pt) {
            var x0 = w / 2 + pt[1] * unit - 40, chName = byKey.up_midi_ch.options[pt[2]];
            g.save(); shadow(g, pt[3] + "0.8)", 6); g.fillStyle = pt[3] + "0.85)"; g.fillRect(x0, pt[4], 80, 8); g.restore();
            g.fillStyle = pt[3] + "0.9)"; g.font = "600 9px " + mono();
            g.fillText((pt[0] === "up" ? "UPPER" : "LOWER") + "  ch " + chName + "  " + (pt[1] > 0 ? "+" : "") + pt[1] + " st", x0 + 86 > w - 60 ? x0 - 96 : x0 + 86, pt[4] + 8);
        });
        g.fillStyle = "rgba(255,255,255,0.5)"; g.fillRect(Math.round(w / 2) - 0.5, 8, 1, h - 22);
        label(g, h, "TRANSPOSE   " + (v("up_delay_sync", 0) || v("lo_delay_sync", 0) ? "delay sync on" : "no sync"));
    }
    function drawTrig(c) {
        if (!visible(c)) return;
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var on = v("trigger_switch", 0), dest = v("trigger_dest", 0), mid = h / 2, A = h / 2 - 10;
        g.globalAlpha = on ? 1 : 0.4;
        var P = new Path2D();
        for (var x = 0; x <= w; x++) { var t = x / w * 4, ph = t - Math.floor(t); var env = ph < 0.08 ? ph / 0.08 : Math.exp(-(ph - 0.08) * 6); var y = mid + A - env * A * 2 * (dest === 0 ? 0.7 : 1); if (x === 0) P.moveTo(x, y); else P.lineTo(x, y); }
        glowStroke(g, P);
        g.globalAlpha = 1;
        label(g, h, (on ? "ON  " : "OFF  ") + byKey.trigger_dest.options[dest] + "   ch " + v("trigger_ch", 1) + "  " + M.valueText(byKey.trigger_note, v("trigger_note", 0)));
    }
    function drawFilter(c) {
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var type = v("filter_type", 2), slope = v("cutoff_slope", 1), cut = v("cutoff", 127) / 127, res = v("resonance", 0) / 127;
        var fc = 0.06 + cut * 0.86, order = slope ? 4 : 2, q = 0.5 + res * 6;
        var base = h - 6;
        function resp(f) {
            var ratio = Math.max(f, 1e-4) / fc, mag;
            if (type === 2) mag = 1 / Math.sqrt(1 + Math.pow(ratio, 2 * order));
            else if (type === 0) mag = 1 / Math.sqrt(1 + Math.pow(1 / ratio, 2 * order));
            else mag = 1 / Math.sqrt(1 + Math.pow((ratio - 1 / ratio) * 2, 2));
            var peak = 1 + (q - 0.5) * Math.exp(-Math.pow((f - fc) / (0.06 + 0.02 * (1 - res)), 2));
            return base - Math.min(1.7, mag * peak) * (h - 14) * 0.58;
        }
        var P = new Path2D(); for (var i = 0; i <= w; i++) { var y = resp(i / w); if (i === 0) P.moveTo(0, y); else P.lineTo(i, y); }
        areaFill(g, w, h, P, base);
        /* the cutoff marker */
        var xc = fc * w;
        g.strokeStyle = GRID2; g.setLineDash([2, 3]); g.beginPath(); g.moveTo(Math.round(xc) + 0.5, 4); g.lineTo(Math.round(xc) + 0.5, base); g.stroke(); g.setLineDash([]);
        glowStroke(g, P);
        label(g, h, ["HPF", "BPF", "LPF"][type] + " " + (slope ? "-24" : "-12") + " dB   " + v("cutoff", 127) + " / " + v("resonance", 0));
    }
    function drawPenv(c) {
        var o = ctx2d(c), g = o.g, w = o.w, h = o.h; grid(g, w, h);
        var dep = v("pitch_env_depth", 0) / 64, pa = v("pitch_env_attack", 0) / 127, pd = v("pitch_env_decay", 64) / 127;
        var mid = h / 2, xa = 8 + pa * (w * 0.35), xd = xa + 8 + pd * (w * 0.5), yp = mid - dep * (mid - 8);
        var P = new Path2D(); P.moveTo(6, mid); P.lineTo(xa, yp); P.lineTo(Math.min(w - 6, xd), mid); P.lineTo(w - 6, mid);
        if (dep !== 0) { var A = new Path2D(P); A.lineTo(6, mid); A.closePath(); g.fillStyle = "rgba(242,177,61,0.14)"; g.fill(A); }
        glowStroke(g, P);
        label(g, h, "PITCH ENV  " + (dep > 0 ? "+" : "") + v("pitch_env_depth", 0));
    }
    /* Every display, by the id mkViz registered it under. drawViz(id) draws one
     * and records what it read; drawAllViz draws them all. */
    var DRAW = {
        fenv: function (c) { drawEnv(c, v("filter_attack", 0), v("filter_decay", 64), v("filter_sustain", 127), v("filter_release", 64), "FILTER ENV"); },
        aenv: function (c) { drawEnv(c, v("amp_attack", 0), v("amp_decay", 0), v("amp_sustain", 127), v("amp_release", 40), "AMP ENV"); },
        filter: drawFilter,
        osc1: function (c) { drawOsc(c, 1); },
        osc2: function (c) { drawOsc(c, 2); },
        lfo1: drawLfo1, lfo2: drawLfo2,
        tone: drawTone, chorus: drawChorus, delay: drawDelay, keys: drawKeys, arp: drawArp, bend: drawBend,
        porta: drawPorta, parts: drawParts, trig: drawTrig, penv: drawPenv,
    };
    function drawAllViz() {
        if (!S.temp) return;
        for (var id in DRAW) drawViz(id);
        if (!animRaf) { animLast = 0; animateScopes(performance.now()); }
    }

    /* ---- go ------------------------------------------------------------------------ */
    build();
    if (remote) {
        remote.onParamChange(onParams);
        if (remote.getChainParams) remote.getChainParams().then(function (cp) { if (Array.isArray(cp)) { S.chain = cp; refreshBrowser(); } });
    }
    loadCatalog();
    window.addEventListener("resize", redrawViz);
    /* The displays fill their section's leftover height, so they resize
     * whenever a neighbour does; redraw on any size change, not just the window's. */
    if (typeof ResizeObserver === "function") {
        var ro = new ResizeObserver(redrawViz);
        for (var vid in vizByGroup) ro.observe(vizByGroup[vid].parentNode);
    }
    if (!remote) {
        /* Opened as a plain file: show the panel at defaults so it can be looked at. */
        S.temp = M.defaultImage(); S.connected = true; setBusy(false); refreshAll();
    }
})();
