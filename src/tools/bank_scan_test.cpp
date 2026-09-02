/* Mac-side check of jp8000_banks.h against a directory of JP-8000 files.
 *   c++ -std=c++17 -O1 -I src/dsp src/tools/bank_scan_test.cpp -o /tmp/bank_scan && /tmp/bank_scan <dir>
 * Prints one line per bank (file, kind, count, first three names) plus, for
 * every preset, verifies each message re-addresses to the temp area with a
 * valid checksum. */
#include "jp8000_banks.h"
#include <cstdio>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dir>\n", argv[0]); return 2; }
    std::vector<jpbank::Bank> banks;
    jpbank::scan_dir(argv[1], banks);
    int presets = 0, msgs = 0, bad = 0;
    for (auto &b : banks) {
        printf("%-28s %-5s %3zu", b.name.c_str(), b.is_perf ? "perf" : "patch", b.presets.size());
        for (size_t i = 0; i < b.presets.size() && i < 3; i++) printf("  [%s]", b.presets[i].name.c_str());
        printf("\n");
        for (auto &p : b.presets) {
            presets++;
            for (auto m : p.msgs) {
                msgs++;
                uint32_t a = jpbank::dt1_addr(m);
                uint32_t local = b.is_perf ? (a & 0xFFFF) : (0x4000 | (a & 0x1FF));
                jpbank::dt1_set_addr(m, 0x01000000 | local);
                if (m[m.size() - 2] != jpbank::checksum(&m[6], m.size() - 8)) bad++;
                uint32_t lin = jpbank::packed_to_linear(local & 0xFFFF);
                uint32_t base = (local & 0xFE00);
                uint32_t limit = (base == 0x4000 || base == 0x4200)
                    ? jpbank::packed_to_linear(base) + 239 : 0;
                if (limit && lin + jpbank::dt1_data_len(m) > limit) {
                    printf("   overlong patch msg lin=%u len=%zu\n", lin, jpbank::dt1_data_len(m));
                    bad++;
                }
            }
        }
    }
    printf("%zu banks, %d presets, %d msgs, %d bad\n", banks.size(), presets, msgs, bad);
    return bad ? 1 : 0;
}
