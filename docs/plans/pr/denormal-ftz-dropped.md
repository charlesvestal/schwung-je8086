# DROPPED: setFlushDenormalsToZero() is a no-op outside MSVC

Not opening this. The bug is real and provable, but it buys nothing measurable,
and without a number it reads as a scold rather than a contribution.

## The bug is real (and sharper than first stated)

`HAVE_SSE` **is** defined — in `dsp56kBase/buildconfig.h`, guarded on x86. But
`baseLib` neither includes that header nor links `dsp56kBase`
(`target_link_libraries(baseLib PUBLIC dl)`), so `#elif defined(HAVE_SSE)` in
`baseLib/os.cpp` is unreachable **on every platform**, x86 included — not just
broken on ARM, which is how it was first described here.

Proven at the preprocessor level, reproducible in two commands:

| target | upstream | patched |
|--------|----------|---------|
| arm64 | *(empty function body)* | `mrs FPCR` / bit 24 / `msr FPCR` |
| x86-64 | *(empty function body)* | `_mm_setcsr(… \| 0x8000)` |

    clang++ -std=c++17 -target x86_64-linux-gnu -I source/framework -E \
        source/framework/baseLib/os.cpp | sed -n '/setFlushDenormalsToZero/,/^ *}/p'

Same dead guard is in `synthLib/os.cpp:35` and `filesystem.cpp:35`.

## Why it buys nothing

Every emulation core in the project is fixed-point — the JP-8000 ASICs emit
`onReceiveSample(int32_t, int32_t)`, and DSP56300/MC68K/H8S are integer — so
denormals cannot arise there. The only float downstream is an FIR resampler,
which produces exact zeros from zero input rather than decaying denormals.

Measured through the CLAP (the only path that calls it):

- First A/B suggested ~2%, FTZ-on winning 3/3. **That was an artifact**: I
  alternated A,B,A,B so FTZ-off always ran second while the Pi warmed 50 -> 70 C
  with the soft-throttle flag set.
- Counterbalanced (off first), the direction stopped being consistent — one run
  had FTZ-*off* faster. Discounting cold first runs the gap is ~12 µs on
  ~12,700, i.e. **0.1%**, inside the noise.

**Always counterbalance the order (B,A,A,B). Straight alternation plus thermal
drift invented a 2% result here.**

## If it is ever revisited

It is still worth fixing in our fork (the aarch64 path is live for us), and if
someone finds a float-heavy device where denormals genuinely occur, the
measurement would be worth redoing. Frame it as "here is a free N% you are
leaving on the table", never as "your code is broken" — the difference between a
contribution and a scold is entirely in the framing.
