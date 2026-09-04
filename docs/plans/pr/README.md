# Per-PR working notes

One file per upstream PR. **These live in the PARENT repo on purpose.**

The PR branches are in the submodule (`libs/gearmulator`, pushed to our
gearmulator fork); these notes are in `schwung-jp8000`. Two different
repositories, so the notes CANNOT end up in a gearmulator PR no matter what is
committed or when. No "remember to drop the docs commit" discipline needed.

## Layout

| where | what |
|-------|------|
| `libs/gearmulator` branch `pr/<name>` | ONLY the commits that go upstream, forked from `upstream-gearmulator/main` |
| `docs/plans/pr/<name>.md` (here) | findings, measurements, decisions, dead ends |

A PR branch must contain nothing but its own change: no notes, no
instrumentation, none of our other 24 commits. Verify with
`git diff --stat upstream-gearmulator/main..pr/<name>` before opening anything.

## Status

| PR | branch | state |
|----|--------|-------|
| timer event horizon | `pr/h8s-timer-event-horizon` | **measured, ready** — `h8s-timer-event-horizon.md` |
| denormal FTZ | — | **dropped**, no measurable benefit — `denormal-ftz-dropped.md` |
| MIDI printf | — | not started; reframed as hygiene, not performance |
| H8S page bitmap | — | not started |
| H8S wide access | — | not started; **depends on the page bitmap** |
| a64::Assembler | — | not started |
| ESP dirty tracking | — | not started; **flagship candidate** |
| dense ARM64 emitter | — | not started; **flagship candidate** |
| PerformanceControlChannel | — | not started; upstream's bug, frame as a question |
| parallel pipeline | — | issue first, not a patch |
