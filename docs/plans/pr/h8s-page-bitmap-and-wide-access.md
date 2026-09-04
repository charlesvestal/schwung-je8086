# PR: H8S page bitmap + wide access (PRs 4 and 5, combined)

**Branch** `pr/h8s-wide-access` in `libs/gearmulator` (commits `bf9fa8a0`,
`23a15709`), forked from `upstream-gearmulator/main` (`c5dddcd0`).
**Size** 46 insertions / 7 deletions, one file.
**State** DRAFT PR #296 open. Measured, bit-exact.

## Combined on purpose

PR 4 alone measures **+0.01%** -- nothing. Its job is to make PR 5 CORRECT:
`plainRun()` needs a per-PAGE answer to know no byte of the run is mapped, and a
per-address `maps[]` test would only cover the first byte. Two PRs would mean
asking a reviewer to take a change whose honest number is zero, then a second one
depending on it.

## Measured against VANILLA upstream

Six runs, palindrome U,B,W,W,B,U, idle Pi 4B @ 1.8 GHz:

| build | mean wav bytes | same-build spread |
|-------|----------------|-------------------|
| upstream | 67,981,868 | 0.42% |
| + bitmap | 67,990,316 | 0.36% |
| + bitmap + wide | 68,921,900 | 0.11% |

**bitmap alone +0.01%** (runs interleave: u1, b2x, b1x, u2 -- inside noise).
**both together +1.38%.**

Bit-exact, with both self-checks clean.

## THE TRAP THIS SESSION FELL INTO: measure against upstream, not our stack

The first attempt measured 4+5 by TOGGLING them off on our own tree -- which
already carries #293, #294 and everything else. That answers "what do these add
to our stack", not "what do these do to upstream", and the PR claims the latter.
It was caught by the user, not by me.

**And the predicted direction of the bias was WRONG.** The reasoning was: with
#294 in, ESP work is ~65% cheaper, so H8S is a bigger share of the profile and
H8S changes should look BETTER. Measured: **+0.63% stacked vs +1.38% against
upstream** -- the stacked baseline UNDERSTATED it by half. No confirmed
explanation. The lesson is not "stacked overstates", it is that a plausible
argument about the direction of a bias is not a substitute for the measurement.

The toggle trick is still fine for what it is (it is how PR 8 was measured, where
the metric was a COUNT), but a rate claimed for a PR needs the PR's own baseline.

## Method note

Splitting cost two commits of hand-editing `h8s.hpp` on top of upstream; the
`instrStartCycles` hunk in `787c8dfe` belongs to PR 6 (#293) and was left out.
Nothing else in that file's diff is ours, so no snapshot code to drop -- unlike
`h8sdevices.hpp`.
