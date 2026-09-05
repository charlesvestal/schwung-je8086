#!/usr/bin/env python3
"""UNCOMMITTED diagnostic trace (idempotent).

Per sample, on the thread that OWNS each ASIC, record the words that ASIC hands
to the next one -- plus every uC register write and the stream delivered to the
host. Serial mode records all of it from one thread; a pipeline records each
piece from its owning stage, at the same logical point in the sample.

Windows are anchored to the FIRST RECORDED SAMPLE (je_trace_mark), not to the
start of emulation: boot renders seconds of audio before the wav opens, and an
un-anchored window lands that much too early.

  JE_TRACE_FILE prefix   JE_TRACE_FROM first sample   JE_TRACE_N count
Files: <pre>.b0<t> <pre>.b1<t> <pre>.b2<t> <pre>.uc<t> <pre>.dv<t>  (t = thread)
"""
import sys, pathlib
root = pathlib.Path(sys.argv[1])
h = root / "source/ronaldo/je8086/jeLib/je8086devices.h"
s = h.read_text()
if "JE_TRACE_FILE" in s:
    print("trace already present"); sys.exit(0)

s = s.replace("#pragma once",
              "#pragma once\n#include <cstdio>\n#include <cstdlib>\n#include <atomic>", 1)

decl = r'''
		/* ---- TRACE (uncommitted, tools/harness/apply_trace.py) ---- */
		inline thread_local uint64_t g_je_trace_sample = 0;
		inline std::atomic<uint64_t> g_je_trace_base{0};
		inline std::atomic<uint64_t> g_je_deliv_idx{0};
		inline bool g_je_trace_marked = false;
		inline void je_trace_mark() { g_je_trace_base.store(g_je_deliv_idx.load()); g_je_trace_marked = true; }

		struct JeTraceSink
		{
			FILE* f = nullptr; bool init = false, done = false; uint64_t from = 0, n = 0;
			void open(const char* _slot)
			{
				init = true;
				const char* pre = getenv("JE_TRACE_FILE");
				if (!pre) return;
				from = strtoull(getenv("JE_TRACE_FROM") ? getenv("JE_TRACE_FROM") : "0", nullptr, 10);
				n    = strtoull(getenv("JE_TRACE_N")    ? getenv("JE_TRACE_N")    : "0", nullptr, 10);
				/* One file PER THREAD: the stages all reach this and a shared name
				 * gets clobbered -- that once produced a bogus "every record differs". */
				static std::atomic<int> next{0};
				static thread_local int id = next.fetch_add(1);
				char name[512]; snprintf(name, sizeof(name), "%s.%s%d", pre, _slot, id);
				f = fopen(name, "wb");
			}
			bool want(uint64_t _raw, uint64_t& _rel, const char* _slot)
			{
				if (!init) open(_slot);
				if (!f || done || !g_je_trace_marked) return false;
				const uint64_t b = g_je_trace_base.load(std::memory_order_relaxed);
				if (_raw < b) return false;
				_rel = _raw - b;
				if (_rel < from) return false;
				if (_rel >= from + n) { fclose(f); f = nullptr; done = true; return false; }
				return true;
			}
		};

		inline void je_trace_words(const char* _slot, int _slotIdx, const int32_t* _g, int _n)
		{
			static thread_local JeTraceSink sinks[3];
			auto& sink = sinks[_slotIdx];
			uint64_t rel;
			if (!sink.want(g_je_trace_sample, rel, _slot)) return;
			fwrite(&rel, 8, 1, sink.f); fwrite(_g, 4, _n, sink.f);
		}
		inline void je_trace_uc(int _asic, uint32_t _addr, uint8_t _val)
		{
			static thread_local JeTraceSink sink;
			uint64_t rel;
			if (!sink.want(g_je_trace_sample, rel, "uc")) return;
			const uint32_t a = _asic, ad = _addr, v = _val;
			fwrite(&rel, 8, 1, sink.f); fwrite(&a, 4, 1, sink.f);
			fwrite(&ad, 4, 1, sink.f); fwrite(&v, 4, 1, sink.f);
		}
		inline void je_trace_deliv(int32_t _l, int32_t _r)
		{
			static thread_local JeTraceSink sink;
			const uint64_t raw = g_je_deliv_idx.fetch_add(1, std::memory_order_relaxed);
			uint64_t rel;
			if (!sink.want(raw, rel, "dv")) return;
			fwrite(&rel, 8, 1, sink.f); fwrite(&_l, 4, 1, sink.f); fwrite(&_r, 4, 1, sink.f);
		}

'''
anchor = "\t\tclass MultiAsic : public H8SDevice\n"
assert anchor in s
s = s.replace(anchor, decl + anchor, 1)

# a member that traces the boundaries owned by [lo, hi)
member = r'''			/* TRACE: the handoff each owned ASIC publishes, read at the same point
			 * the serial path reads it (post-run, pre-sync). */
			void traceBoundaries(int _lo, int _hi)
			{
				int32_t g[10];
				if (_lo <= 0 && 0 < _hi) { for (int k = 0; k < 3; k++) g[k] = asic0.readGRAM(0x80 + k * 2); je_trace_words("b0", 0, g, 3); }
				if (_lo <= 1 && 1 < _hi) { for (int k = 0; k < 6; k++) g[k] = asic1.readGRAM(0x80 + k * 2); je_trace_words("b1", 1, g, 6); }
				if (_lo <= 2 && 2 < _hi) { for (int k = 0; k < 8; k++) g[k] = asic2.readGRAM(0x80 + k * 2);
					g[8] = asic2.readGRAM(0xa0); g[9] = asic2.readGRAM(0xa2); je_trace_words("b2", 2, g, 10); }
			}
'''
a = "\t\t\tvoid runAsics(int first, int last) {\n"
assert a in s
s = s.replace(a, member + a, 1)

# serial mode 0
a0 = "\t\t\t\t\t\temitAudio(asic3.readGRAM(0xe8), asic3.readGRAM(0xec));\n"
assert a0 in s
s = s.replace(a0, a0 + "\n\t\t\t\t\t\ttraceBoundaries(0, 4); ++g_je_trace_sample;\n", 1)

# pipeline parent (mode 1): owns [0, split)
a1 = "\t\t\t\t\t\trunAsics(0, split);\n"
assert a1 in s
s = s.replace(a1, a1 + "\t\t\t\t\t\ttraceBoundaries(0, split); ++g_je_trace_sample;\n", 1)

# pipeline stage
a2 = "\t\t\t\trunAsics(lo, hi);\n"
assert a2 in s
s = s.replace(a2, a2 + "\t\t\t\ttraceBoundaries(lo, hi);\n", 1)
a3 = "\t\t\t\t// 5. sync_cores\n\t\t\t\tsyncAsics(lo, hi);\n\t\t\t\treturn true;\n"
assert a3 in s
s = s.replace(a3, "\t\t\t\t// 5. sync_cores\n\t\t\t\tsyncAsics(lo, hi);\n\t\t\t\t++g_je_trace_sample;\n\t\t\t\treturn true;\n", 1)

# uC writes: serial/owned path and the forwarded path
a4 = "\t\t\t\tforAsic(asic, [&](auto& a) { a.writeuC(_address, _value); });\n"
assert a4 in s
s = s.replace(a4, "\t\t\t\tje_trace_uc(asic, _address, _value);\n" + a4, 1)
a5 = "\t\t\t\tforAsic(asic, [&](auto& a) { a.writeuC(addr, val); });"
assert a5 in s
s = s.replace(a5, "\t\t\t\tje_trace_uc(asic, addr, val);\n" + a5, 1)
h.write_text(s)

c = root / "source/ronaldo/je8086/jeLib/je8086.cpp"
t = c.read_text()
a6 = "\tvoid Je8086::onReceiveSample(int32_t _left, int32_t _right)\n\t{\n"
assert a6 in t
t = t.replace(a6, a6 + "\t\tdevices::je_trace_deliv(_left, _right);\n", 1)
c.write_text(t)

j = root / "source/ronaldo/je8086/jeTestConsole/jeTestConsole.cpp"
u = j.read_text()
a7 = '\t\tsynthLib::AsyncWriter writer("je8086_out.wav", samplerate);\n'
assert a7 in u
u = u.replace(a7, "\t\tjeLib::devices::je_trace_mark();\t// anchor trace windows to the wav\n" + a7, 1)
j.write_text(u)
print("trace applied")
