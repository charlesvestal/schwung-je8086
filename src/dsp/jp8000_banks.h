/*
 * JP-8000 preset bank scanner.
 *
 * Reads what people actually have lying around for a JP-8000 and turns each
 * file into banks of presets, where a preset is the list of Roland DT1
 * messages that make it up, still addressed to the USER area they came from
 * (the plugin re-addresses them to the temp area at load time, exactly like
 * gearmulator's Controller::sendSingle).
 *
 * Formats (characterised against a 97-file corpus, see src/tools/scan notes):
 *   .syx .j8k        raw concatenated SysEx
 *   .mid .MID        SMF with F0/F7 SysEx events (F7 continuation, running
 *                    status, FF meta skipped); non-Roland dumps ignored
 *   .pat             WinJPLib: 128-byte header + 128 x 239-byte patches
 *   .pfm             WinJPLib: 128-byte header + 64 x 528-byte performances
 *                    (common 36 + part 7 + part 7 + patch 239 + patch 239)
 *
 * Header-only and dependency-free so tools/bank_scan_test.cpp can run it on
 * the Mac against the corpus. Nothing here runs on the SPI thread.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <dirent.h>
#include <sys/stat.h>

namespace jpbank {

typedef std::vector<uint8_t> Msg;

struct Preset {
    std::string name;
    std::vector<Msg> msgs;
};

struct Bank {
    std::string name;
    bool is_perf;
    std::vector<Preset> presets;
    /* Folder this bank came from, "" for the top of banks/. LAST, so the
     * existing brace-initialisations of {name, is_perf, presets} still hold.
     * The browser is a hierarchy -- folder, then bank, then preset -- because a
     * real library is nested and a flat list of 36 long paths is unreadable on
     * 128 pixels. */
    std::string folder;
};

enum { AREA_SYSTEM = 0x00, AREA_TEMP = 0x01, AREA_USER_PATCH = 0x02, AREA_USER_PERF = 0x03 };

static const uint8_t DT1_HEAD[6] = {0xF0, 0x41, 0x10, 0x00, 0x06, 0x12};

/* Packed (7-bit-per-byte) address <-> linear byte offset, low 16 bits only. */
static inline uint32_t packed_to_linear(uint32_t packed) {
    return ((packed >> 8) & 0x7f) * 128 + (packed & 0x7f);
}
static inline uint32_t linear_to_packed(uint32_t lin) {
    return ((lin / 128) << 8) | (lin % 128);
}

static inline uint8_t checksum(const uint8_t *p, size_t n) {
    unsigned s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return (uint8_t)((128 - (s & 0x7f)) & 0x7f);
}

/* Build one DT1: F0 41 10 00 06 12 a0 a1 a2 a3 data.. cs F7 */
static inline Msg make_dt1(uint32_t addr, const uint8_t *data, size_t n) {
    Msg m;
    m.reserve(n + 12);
    m.insert(m.end(), DT1_HEAD, DT1_HEAD + 6);
    m.push_back((addr >> 24) & 0x7f);
    m.push_back((addr >> 16) & 0x7f);
    m.push_back((addr >> 8) & 0x7f);
    m.push_back(addr & 0x7f);
    m.insert(m.end(), data, data + n);
    m.push_back(checksum(&m[6], 4 + n));
    m.push_back(0xF7);
    return m;
}

static inline bool is_dt1(const Msg &m) {
    return m.size() >= 12 && memcmp(&m[0], DT1_HEAD, 6) == 0 && m.back() == 0xF7;
}
static inline uint32_t dt1_addr(const Msg &m) {
    return ((uint32_t)m[6] << 24) | ((uint32_t)m[7] << 16) | ((uint32_t)m[8] << 8) | m[9];
}
static inline size_t dt1_data_len(const Msg &m) { return m.size() - 12; }
static inline const uint8_t *dt1_data(const Msg &m) { return &m[10]; }

/* Re-address a DT1 in place and fix its checksum. */
static inline void dt1_set_addr(Msg &m, uint32_t addr) {
    m[6] = (addr >> 24) & 0x7f; m[7] = (addr >> 16) & 0x7f;
    m[8] = (addr >> 8) & 0x7f;  m[9] = addr & 0x7f;
    m[m.size() - 2] = checksum(&m[6], m.size() - 8);
}

/* ---- SysEx extraction ------------------------------------------------- */

static inline void raw_sysex(const std::vector<uint8_t> &d, std::vector<Msg> &out) {
    size_t i = 0;
    while (i < d.size()) {
        if (d[i] != 0xF0) { i++; continue; }
        size_t e = i + 1;
        while (e < d.size() && d[e] != 0xF7) e++;
        if (e >= d.size()) break;
        out.push_back(Msg(d.begin() + i, d.begin() + e + 1));
        i = e + 1;
    }
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Standard MIDI File: returns false if `d` is not an SMF. */
static inline bool smf_sysex(const std::vector<uint8_t> &d, std::vector<Msg> &out) {
    if (d.size() < 14 || memcmp(&d[0], "MThd", 4) != 0) return false;
    size_t pos = 8 + be32(&d[4]);
    while (pos + 8 <= d.size()) {
        const uint8_t *tag = &d[pos];
        size_t len = be32(&d[pos + 4]);
        size_t body = pos + 8;
        pos = body + len;
        if (pos > d.size()) break;
        if (memcmp(tag, "MTrk", 4) != 0) continue;
        size_t i = body, end = pos;
        uint8_t run = 0;
        Msg cur;
        bool have_cur = false;
        auto vlq = [&](size_t &p) -> uint32_t {
            uint32_t v = 0;
            while (p < end) { uint8_t b = d[p++]; v = (v << 7) | (b & 0x7f); if (!(b & 0x80)) break; }
            return v;
        };
        while (i < end) {
            vlq(i);
            if (i >= end) break;
            uint8_t st = d[i];
            if (st == 0xF0 || st == 0xF7) {
                i++;
                uint32_t n = vlq(i);
                if (i + n > end) break;
                if (st == 0xF0) { cur.clear(); cur.push_back(0xF0); have_cur = true; }
                else if (!have_cur) { cur.clear(); have_cur = true; }
                cur.insert(cur.end(), d.begin() + i, d.begin() + i + n);
                i += n;
                if (!cur.empty() && cur.back() == 0xF7) {
                    if (cur.size() > 1 && cur[0] == 0xF0) out.push_back(cur);
                    cur.clear(); have_cur = false;
                }
            } else if (st == 0xFF) {
                i += 2;
                uint32_t n = vlq(i);
                i += n;
            } else {
                if (st & 0x80) { run = st; i++; }
                uint8_t hi = run & 0xF0;
                i += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            }
        }
    }
    return true;
}

/* ---- Grouping DT1s into presets --------------------------------------- */

static inline std::string name_from(const Msg *offset0) {
    if (!offset0 || dt1_data_len(*offset0) < 16) return "";
    std::string s((const char *)dt1_data(*offset0), 16);
    for (auto &c : s) if (c < ' ' || c > '~') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

/* Keyboard temp patch is 239 bytes; anything beyond (JP-8080's second block
 * at linear 242) would land past the temp patch and is dropped. */
static const uint32_t PATCH_LINEAR_LEN = 239;

/* Group by (area, program) in order of first appearance. */
static inline void classify(const std::vector<Msg> &msgs, std::vector<Preset> &patches,
                            std::vector<Preset> &perfs) {
    struct Group { Preset p; const Msg *offset0; bool is_perf; };
    std::vector<Group> groups;
    std::map<uint64_t, size_t> index;
    for (const Msg &m : msgs) {
        if (!is_dt1(m)) continue;
        uint32_t a = dt1_addr(m);
        uint8_t area = (a >> 24) & 0x7f;
        uint64_t key;
        uint32_t local;
        bool is_perf;
        if (area == AREA_USER_PATCH) {
            key = ((uint64_t)area << 32) | (a & 0x00FFFE00);   /* bank/program */
            local = a & 0x1FF;
            is_perf = false;
            if (packed_to_linear(local) >= PATCH_LINEAR_LEN) continue;
        } else if (area == AREA_USER_PERF) {
            key = ((uint64_t)area << 32) | (a & 0x00FF0000);
            local = a & 0xFFFF;
            is_perf = true;
        } else if (area == AREA_TEMP) {
            key = ((uint64_t)area << 32);
            local = a & 0xFFFF;
            is_perf = true;
        } else {
            continue;
        }
        auto it = index.find(key);
        if (it == index.end()) {
            index[key] = groups.size();
            groups.push_back(Group{Preset(), nullptr, is_perf});
            it = index.find(key);
        }
        Group &g = groups[it->second];
        /* A whole-patch message that runs past the keyboard's 239 bytes
         * (JP-8080 dumps are 240/242) is cut at the boundary, not dropped. */
        uint32_t lin = packed_to_linear(local);
        uint32_t limit = 0;
        if (!is_perf) limit = PATCH_LINEAR_LEN;
        else if ((local & 0xFE00) == 0x4000) limit = packed_to_linear(0x4000) + PATCH_LINEAR_LEN;
        else if ((local & 0xFE00) == 0x4200) limit = packed_to_linear(0x4200) + PATCH_LINEAR_LEN;
        if (limit && lin >= limit) continue;   /* wholly past the boundary */
        if (limit && lin + dt1_data_len(m) > limit)
            g.p.msgs.push_back(make_dt1(a, dt1_data(m), limit - lin));
        else
            g.p.msgs.push_back(m);
        if (local == 0) g.offset0 = &g.p.msgs.back();
    }
    for (Group &g : groups) {
        /* Re-find offset 0: the vector may have reallocated. */
        const Msg *o0 = nullptr;
        for (const Msg &m : g.p.msgs) {
            uint32_t a = dt1_addr(m);
            uint32_t local = g.is_perf ? (a & 0xFFFF) : (a & 0x1FF);
            if (local == 0) { o0 = &m; break; }
        }
        g.p.name = name_from(o0);
        (g.is_perf ? perfs : patches).push_back(g.p);
    }
}

/* ---- WinJPLib containers ---------------------------------------------- */

static inline bool parse_pat(const std::vector<uint8_t> &d, std::vector<Preset> &patches) {
    const size_t HDR = 128, N = 239;
    if (d.size() < HDR + N || memcmp(&d[0], "JP-8000 USER PATCH", 18) != 0) return false;
    size_t count = (d.size() - HDR) / N;
    for (size_t i = 0; i < count && i < 128; i++) {
        uint32_t addr = ((uint32_t)AREA_USER_PATCH << 24) | (i < 64 ? 0 : 0x10000) | ((uint32_t)(i & 63) << 9);
        Preset p;
        p.msgs.push_back(make_dt1(addr, &d[HDR + i * N], N));
        p.name = name_from(&p.msgs[0]);
        patches.push_back(p);
    }
    return true;
}

static inline bool parse_pfm(const std::vector<uint8_t> &d, std::vector<Preset> &perfs) {
    const size_t HDR = 128, N = 528;
    if (d.size() < HDR + N || memcmp(&d[0], "JP-8000 USER PERF", 17) != 0) return false;
    size_t count = (d.size() - HDR) / N;
    static const uint32_t off[5]  = {0x0000, 0x1000, 0x1100, 0x4000, 0x4200};
    static const size_t   size[5] = {36, 7, 7, 239, 239};
    for (size_t i = 0; i < count && i < 64; i++) {
        uint32_t base = ((uint32_t)AREA_USER_PERF << 24) | ((uint32_t)i << 16);
        const uint8_t *p = &d[HDR + i * N];
        Preset pr;
        for (int k = 0; k < 5; k++) {
            pr.msgs.push_back(make_dt1(base | off[k], p, size[k]));
            p += size[k];
        }
        pr.name = name_from(&pr.msgs[0]);
        perfs.push_back(pr);
    }
    return true;
}

/* ---- Files and directories -------------------------------------------- */

static inline bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    out.resize(got);
    return got > 0;
}

static inline std::string lower_ext(const std::string &name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string e = name.substr(dot + 1);
    for (auto &c : e) c = (char)tolower((unsigned char)c);
    return e;
}

/* One file -> up to two banks (its patches, its performances). */
static inline void scan_file(const std::string &path, const std::string &display,
                             std::vector<Bank> &banks) {
    std::vector<uint8_t> d;
    if (!read_file(path, d)) return;
    std::string ext = lower_ext(path);
    std::vector<Preset> patches, perfs;
    if (ext == "pat") {
        parse_pat(d, patches);
    } else if (ext == "pfm") {
        parse_pfm(d, perfs);
    } else if (ext == "syx" || ext == "j8k" || ext == "mid" || ext == "midi") {
        std::vector<Msg> msgs;
        if (!smf_sysex(d, msgs)) raw_sysex(d, msgs);
        classify(msgs, patches, perfs);
    } else {
        return;
    }
    if (!patches.empty()) banks.push_back(Bank{display, false, patches});
    if (!perfs.empty()) banks.push_back(Bank{display, true, perfs});
}

/* A bank label the user can tell apart.
 *
 * The extension goes, then the name is trimmed to fit -- FROM THE LEFT when a
 * folder prefix is present, because the distinguishing part of
 * "AZS JP-Eternal/JP-8000/Bank A" is the tail. Trimming from the right turned
 * two different files into the identical label "AZS JP-Eternal/JP-8000/", which
 * is worse than no prefix at all. A leading "~" marks the elision. */
static inline std::string display_name(const std::string &file, size_t max_len) {
    std::string s = file;
    size_t dot = s.rfind('.');
    if (dot != std::string::npos && dot > 0) s = s.substr(0, dot);
    if (s.size() <= max_len) return s;
    const bool nested = s.find('/') != std::string::npos;
    if (!nested) return s.substr(0, max_len);
    /* keep the basename whole if it fits at all */
    const size_t slash = s.rfind('/');
    const std::string base = s.substr(slash + 1);
    if (base.size() + 1 >= max_len) return "~" + base.substr(base.size() - (max_len - 1));
    return "~" + s.substr(s.size() - (max_len - 1));
}

/* Every supported file directly inside `dir`, sorted by name. */
/* Recursive, because a real preset collection is nested: Charles's is 97 files
 * in subdirectories (Jexus/ alone is 20 banks), and a flat scan found none of
 * them. Subdirectory names are carried into the bank label so two files called
 * "Bank A" in different folders stay distinguishable.
 *
 * `skipped` counts files that matched an extension but produced no bank. That
 * number is the whole point of returning it: 41 of those 97 files parse to
 * nothing, and a browser that silently lists nothing is indistinguishable from
 * one that is broken. The caller reports it.
 *
 * Depth is bounded so a symlink loop cannot hang the boot scan. */
static inline void scan_dir(const std::string &dir, std::vector<Bank> &banks, size_t name_max = 23,
                            int *skipped = nullptr, const std::string &prefix = std::string(),
                            int depth = 0)
{
    if (depth > 4) return;
    DIR *dp = opendir(dir.c_str());
    if (!dp) return;
    std::vector<std::string> files, subdirs;
    while (struct dirent *de = readdir(dp)) {
        if (de->d_name[0] == '.') continue;
        std::string name = de->d_name;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { subdirs.push_back(name); continue; }
        std::string ext = lower_ext(de->d_name);
        if (ext != "syx" && ext != "j8k" && ext != "mid" && ext != "midi" && ext != "pat" && ext != "pfm") continue;
        files.push_back(name);
    }
    closedir(dp);
    std::sort(files.begin(), files.end());
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto &f : files) {
        const size_t before = banks.size();
        scan_file(dir + "/" + f, display_name(f, name_max), banks);
        if (banks.size() == before) { if (skipped) (*skipped)++; continue; }
        for (size_t i = before; i < banks.size(); i++) banks[i].folder = prefix;
    }
    for (const auto &d : subdirs)
        scan_dir(dir + "/" + d, banks, name_max, skipped,
                 prefix.empty() ? d : prefix + "/" + d, depth + 1);
}

} // namespace jpbank
