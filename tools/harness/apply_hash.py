#!/usr/bin/env python3
"""UNCOMMITTED diagnostic: periodic hash of each ASIC's internal state (iram of
both cores, shared gram, eram contents + registers), recorded by the owning
thread. Finds WHEN state first diverges, not when it becomes audible -- an ERAM
difference can sit inaudible for minutes before a patch reads it out.

  JE_HASH_EVERY  sample period (default 8820 = 0.1 s); needs JE_TRACE_FILE set.
Files: <pre>.hs<t>  records: (u64 rel_sample, u32 asic, u32 pad, u64 hash)
"""
import sys, pathlib
root = pathlib.Path(sys.argv[1])

e = root / "source/ronaldo/esp/esp.hpp"
s = e.read_text()
if "je_hash" not in s:
    # ERAM: public hash
    a = "\tint32_t eramReadLatch = 0, eramWriteLatch = 0, eramVarOffset = 0;\n"
    assert a in s
    s = s.replace(a, a + """\tuint64_t je_hash(uint64_t h) const {
\t\tfor (int i = 0; i < eram_size; i++) h = h * 1099511628211ULL ^ (uint32_t)eram[i];
\t\th = h * 1099511628211ULL ^ eramPos; h = h * 1099511628211ULL ^ eramEffectiveAddr;
\t\th = h * 1099511628211ULL ^ (uint32_t)eramReadLatch; h = h * 1099511628211ULL ^ (uint32_t)eramWriteLatch;
\t\th = h * 1099511628211ULL ^ (uint32_t)eramWriteLatchNext; h = h * 1099511628211ULL ^ (uint32_t)eramVarOffset;
\t\th = h * 1099511628211ULL ^ ((uint64_t)eramActiveCurrent << 1 | eramActiveNext);
\t\treturn h;
\t}
""", 1)
    # ESPCore: public hash of iram (+ ring pos)
    b = "\tvoid steperam() { if (lg2eram_size) shared->eram.tickCycle((pram[pc] >> 23) & 0x1f, pc); }\n"
    assert b in s
    s = s.replace(b, b + """
\tuint64_t je_hash(uint64_t h) const {
\t\tfor (size_t i = 0; i < sizeof(iram)/sizeof(iram[0]); i++) h = h * 1099511628211ULL ^ (uint32_t)iram[i];
\t\th = h * 1099511628211ULL ^ iramPos;
\t\th = h * 1099511628211ULL ^ (uint32_t)last_mulInputA_24; h = h * 1099511628211ULL ^ (uint32_t)last_mulInputB_24;
\t\th = h * 1099511628211ULL ^ (uint32_t)skipfield;
\t\treturn h;
\t}
""", 1)
    # ESP: combine
    c = "\tvoid step_cores() { core1.steperam(); core1.step(); core0.step();}\n"
    assert c in s
    s = s.replace(c, c + """
\tuint64_t je_hash() const {
\t\tuint64_t h = 14695981039346656037ULL;
\t\th = core0.je_hash(h); h = core1.je_hash(h);
\t\tfor (size_t i = 0; i < sizeof(shared.gram)/sizeof(shared.gram[0]); i++) h = h * 1099511628211ULL ^ (uint32_t)shared.gram[i];
\t\tfor (int i = 0; i < 8; i++) h = h * 1099511628211ULL ^ (uint32_t)shared.mulcoeffs[i];
\t\th = shared.eram.je_hash(h);
\t\treturn h;
\t}
""", 1)
    e.write_text(s)
    print("esp.hpp hash methods added")

h = root / "source/ronaldo/je8086/jeLib/je8086devices.h"
s = h.read_text()
assert "JE_TRACE_FILE" in s, "apply_trace.py must run first"
if "je_trace_hash" not in s:
    decl = '''		inline void je_trace_hash(int _asic, uint64_t _hash)
		{
			static thread_local JeTraceSink sink;
			if (!sink.init) { sink.open("hs"); sink.from = 0; sink.n = ~0ULL; }
			if (!sink.f || !g_je_trace_marked) return;
			const uint64_t b = g_je_trace_base.load(std::memory_order_relaxed);
			if (g_je_trace_sample < b) return;
			const uint64_t rel = g_je_trace_sample - b;
			const uint32_t a = _asic, pad = 0;
			fwrite(&rel, 8, 1, sink.f); fwrite(&a, 4, 1, sink.f); fwrite(&pad, 4, 1, sink.f); fwrite(&_hash, 8, 1, sink.f);
		}

'''
    anchor = "\t\tclass MultiAsic : public H8SDevice\n"
    s = s.replace(anchor, decl + anchor, 1)
    member = '''			void traceHashes(int _lo, int _hi)
			{
				static thread_local uint64_t every = 0;
				if (!every) { const char* e2 = getenv("JE_HASH_EVERY"); every = e2 ? strtoull(e2, nullptr, 10) : 8820; }
				if (g_je_trace_sample % every) return;
				for (int i = _lo; i < _hi; i++) forAsic(i, [&](auto& a) { je_trace_hash(i, a.je_hash()); });
			}
'''
    a = "\t\t\tvoid traceBoundaries(int _lo, int _hi)\n"
    assert a in s
    s = s.replace(a, member + a, 1)
    # call beside every traceBoundaries call
    s = s.replace("\t\t\t\t\t\ttraceBoundaries(0, 4); ++g_je_trace_sample;\n",
                  "\t\t\t\t\t\ttraceBoundaries(0, 4); traceHashes(0, 4); ++g_je_trace_sample;\n", 1)
    s = s.replace("\t\t\t\t\t\ttraceBoundaries(0, split); ++g_je_trace_sample;\n",
                  "\t\t\t\t\t\ttraceBoundaries(0, split); traceHashes(0, split); ++g_je_trace_sample;\n", 1)
    s = s.replace("\t\t\t\ttraceBoundaries(lo, hi);\n",
                  "\t\t\t\ttraceBoundaries(lo, hi); traceHashes(lo, hi);\n", 1)
    h.write_text(s)
    print("hash trace wired in")
