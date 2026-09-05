#!/usr/bin/env python3
"""Apply the UNCOMMITTED jeTestConsole harness hooks (idempotent).

  JE_DSP_THREADS -- pipeline stage count (the real selector is the plugin setting)
  JE_TC_SECONDS  -- stop after N seconds of RENDERED AUDIO, so a bit-exactness
                    run is bounded by samples and not by a wall-clock race.
"""
import sys, pathlib

p = pathlib.Path(sys.argv[1]) / "source/ronaldo/je8086/jeTestConsole/jeTestConsole.cpp"
s = p.read_text()
if "JE_TC_SECONDS" in s:
    print("harness already present"); sys.exit(0)

s = s.replace("#include <chrono>\n#include <iostream>\n",
              "#include <chrono>\n#include <iostream>\n#include <cstdlib>\n", 1)

anchor = "\tparams.preferredSamplerate = samplerate;\n"
assert anchor in s, "samplerate anchor missing"
# dspThreads only exists on trees carrying the pipeline PR
have_threads = "dspThreads" in (pathlib.Path(sys.argv[1]) / "source/framework/synthLib/device.h").read_text()
threads_line = ('\tif (const char* t = getenv("JE_DSP_THREADS")) params.dspThreads = static_cast<uint32_t>(atoi(t));\n'
                if have_threads else "")
s = s.replace(anchor, anchor + """
\t// HARNESS (uncommitted, tools/harness/apply_tc_harness.py)
""" + threads_line + """\tconst uint64_t limitSamples = getenv("JE_TC_SECONDS")
\t\t? static_cast<uint64_t>(atof(getenv("JE_TC_SECONDS")) * samplerate) : 0;
""", 1)

anchor2 = "\t\t\tintervalProcessedSamples += blocksize;\n"
assert anchor2 in s, "counter anchor missing"
s = s.replace(anchor2, anchor2 + """
\t\t\tif (limitSamples && totalProcessedSamples >= limitSamples)
\t\t\t{
\t\t\t\tstd::cout << "HARNESS: stop at " << totalProcessedSamples << " samples\\n";
\t\t\t\tbreak;
\t\t\t}
""", 1)

p.write_text(s)
print("harness applied")
