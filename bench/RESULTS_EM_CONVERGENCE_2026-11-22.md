# EM's stopping rule at pg.ddx.io's scale (2026-11-22)

pg.ddx.io (round two, N2) found that `learn_cpts(..., 'em')` stopped
about 20 iterations before an independent EM's fixed point and reported
`converged = true`.  The cause: the rule stopped when the objective
changed by at most tol * max(1, |objective|), and at |loglik| 4e4 to 5e4
that is 0.05 nats.  This file measures how far that rule and its
replacement stop from the fixed point.

Host: floki (48 CPUs, shared), PostgreSQL 18.7 from the dev shell, one
throwaway cluster per run (`dev/regress.sh --`).  Workload:
`bench/em_convergence.sql`, pg.ddx.io's shape: a 2-state latent A with
three 3-state children, drawn from planted CPTs, A observed in the first
`lab` rows.  "main" is 5842e7b (code md5 94a5583f...); "after" is the
commit that adds this file (code md5 0129089a...).  The fixed point is a
run to tol 1e-15 in the same arm; "distance" is the largest absolute
difference of any CPT cell from it.

## The trajectory

The 20,000-row model (300 labelled, seed 0.42), every iterate at tol 0:

| iteration | objective change (nats) | distance |
|---|---|---|
| 11 | 4.8e-2 | 3.6e-3 |
| 22 | 3.1e-3 | 9.4e-4 |
| 40 | 3.8e-5 | 1.0e-4 |
| 60 | 2.9e-7 | 9.0e-6 |
| 80 | 3.5e-9 | 7.8e-7 |
| 100 | 7.9e-10 | 6.8e-8 |

From about iteration 10 on, each M-step moves the cells by 0.885 times
the step before (0.888 at 40,000 rows), so the distance to the fixed
point is step * r / (1 - r), about 7.7 times the last step.  The
objective's change falls as the square of the distance, and below about
1e-8 nats it is rounding: from iteration 76 on it is as often negative
as positive.  A rule on the objective cannot ask for cells within 1e-6
at this scale: that needs a change of about 1e-8 nats, where the
objective is noise.  So the new rule watches the cells, estimating the
rate from the last two steps.

## main against after

| rows (labelled) | arm | tol | iterations | distance | time |
|---|---|---|---|---|---|
| 20,000 (300) | main | 1e-6 (relative) | 11 | 3.6e-3 | 1,097 / 1,094 ms |
| 20,000 (300) | after | 1e-6 (absolute, cells) | 78 | 1.0e-6 | 2,928 / 2,849 ms |
| 20,000 (300) | after | 1e-4 | 41 | 9.2e-5 | |
| 20,000 (300) | after | 1e-8 | 116 | 9.6e-9 | |
| 40,000 (400) | main | 1e-6 (relative) | 7 | 2.5e-3 | 1,972 / 2,014 ms |
| 40,000 (400) | after | 1e-6 (absolute, cells) | 72 | 9.8e-7 | 5,332 / 5,392 ms |
| 40,000 (400) | after | 1e-4 | 34 | 9.0e-5 | |
| 40,000 (400) | after | 1e-8 | 111 | 9.5e-9 | |

Times are two calls in one session (`\timing`, rule 4), one run per
arm: no A/A run of either arm, so read them as the cost of the extra
iterations (about 25 ms an iteration at 20,000 rows, 50 ms at 40,000),
not as a measured ratio.  At every tol the distance is within a few
percent of tol.  The fixed points of the two arms agree to 1e-10 in
loglik (-60613.97417066619 against -60613.97417056622, which main
reached after 115 iterations at tol 1e-15).

## A case the estimate misses

Early in a run, before the rate settles, the estimate can be low: on
the 4,000-row model of `sql/em.sql` at tol 1e-3, EM stopped 1.08e-3 from
the fixed point.  The rule also requires the last step itself to be at
most tol, which keeps an accidentally small ratio from stopping a run
that still moves by more than tol.

## Not measured

pg.ddx.io's data.  An A/A noise floor for the times.
