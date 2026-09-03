/* bank_scan.cpp -- what the module's bank parser makes of a file, from the
 * command line.
 *
 * The parser is the thing that decides whether a preset library the user just
 * dropped into banks/ becomes 64 presets, one preset, or nothing at all, and
 * until now the only way to ask it was to boot the emulator and read params
 * back one at a time. This links jp8000_banks.h directly, so a whole corpus
 * can be audited in a second.
 *
 *   bank_scan <file|dir> ...        one line per file, --names for the presets
 */
#include "../dsp/jp8000_banks.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <algorithm>

static bool g_names = false;

static void scan_one(const std::string &path) {
    std::vector<jpbank::Bank> banks;
    jpbank::scan_file(path, path, banks);
    size_t np = 0, nf = 0, bp = 0, bf = 0;
    for (const auto &b : banks) {
        if (b.is_perf) { nf += b.presets.size(); bf++; }
        else           { np += b.presets.size(); bp++; }
    }
    const char *base = strrchr(path.c_str(), '/');
    printf("%-52.52s  patches %3zu in %zu bank(s)   performances %3zu in %zu bank(s)%s\n",
           base ? base + 1 : path.c_str(), np, bp, nf, bf,
           (np == 0 && nf == 0) ? "   <-- NOTHING" : "");
    if (!g_names) return;
    for (const auto &b : banks)
        for (size_t i = 0; i < b.presets.size(); i++)
            printf("      %c %2zu %s\n", b.is_perf ? 'F' : 'P', i, b.presets[i].name.c_str());
}

static void walk(const std::string &p, std::vector<std::string> &out) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { out.push_back(p); return; }
    DIR *d = opendir(p.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        walk(p + "/" + e->d_name, out);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--names")) { g_names = true; continue; }
        walk(argv[i], files);
    }
    std::sort(files.begin(), files.end());
    for (const auto &f : files) {
        std::string lower = f;
        for (auto &c : lower) c = (char)tolower((unsigned char)c);
        if (lower.size() < 4) continue;
        const std::string ext = lower.substr(lower.rfind('.') == std::string::npos ? lower.size() : lower.rfind('.'));
        if (ext != ".syx" && ext != ".mid" && ext != ".pat" && ext != ".pfm" && ext != ".j8k") continue;
        scan_one(f);
    }
    return 0;
}
