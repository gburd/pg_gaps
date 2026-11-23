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

## Where the first rule stopped early, and the rule that replaced it

An independent review of the rule above found that it stopped far from
the fixed point with `converged` true in three cases, and that the
"case the estimate misses" was larger than recorded.  The rule now in
`gaps._em_settled` differs in three ways: the run's first M-step is
never a rate sample (the review's proposal); r is the larger of the
last two ratios plus their difference over 1 - r; and the rule runs per
factor, stopping when every unfrozen factor has settled.  "first rule" is 9516fe6 (code md5
0129089a...), "now" the commit that adds this section (md5 3497f27b...).
The probes are the review's (`/scratch/pg_gaps/review-learn/`, not in
the tree); the reference is a run to tol 1e-12 or tighter.

Distance from the reference, as a multiple of tol:

| model | tol | before | now (iterations) |
|---|---|---|---|
| h, 1,000 unlabelled | 1e-2 | 16x (2 its) [a] | 0.85x (248) |
| h, 1,000 unlabelled | 1e-3 | 164x (2 its) [a] | 0.98x (376) |
| h, 1,000 unlabelled | 1e-6 | | 1.00x (777) |
| h, 10,000 unlabelled | 1e-2 to 1e-6 | | 0.65x to 0.99x (3,080 to 7,988) |
| lk, 4,000 rows (100 labelled) | 1e-2 | 3.64x [a] | 1.05x (18) |
| lk | 3e-3 | 1.21x [a] | 0.93x (36) |
| lk | 1e-3 | 1.08x [a] | 0.87x (56) |
| lk | 1e-4 to 1e-8 | 0.95x to 1.00x [b] | 0.94x to 1.00x (100 to 284) |
| h + lk (1,000 labelled) in one model | 3e-3 to 1e-3 | 54x to 162x (4) [b] | 0.9x to 1.0x (314 to 382) |

[a] 9516fe6, from the review.  [b] An intermediate rule (no first-step
sample, larger of two ratios, one rate for the whole model), measured
here; the mixed-model case is what made the rule per factor.

At 100,000 unlabelled rows of h, tol 1e-5, the first rule stopped after
2 iterations; now EM runs the 500 default iterations (33.8 s) and
reports `converged` false, still moving 8e-4 over the next 400.  At
10^6 unlabelled rows and the default tol, likewise: 500 iterations,
`converged` false, 5.8 minutes grounding included, still moving 3.7e-4
over the next 60.  Neither large case was run to its fixed point.

On pg.ddx.io's shape the new rule changes little.  20,000 rows (300
labelled): 78 iterations at the default, 9.97e-7 from the fixed point
(the same as the first rule); 41 at 1e-4 (9.2e-5); 116 at 1e-8
(9.6e-9).  40,000 rows (400 labelled): 70 (9.0e-7), 31 (8.9e-5), 109
(9.2e-9), against 72, 34 and 111 for the first rule.  The cost of the
per-factor bookkeeping, both arms in one session each, run twice in
alternation on the same host at 20,000 rows, two calls a run: first
2,773 / 2,783 and 2,783 / 2,788 ms, now 2,784 / 2,776 and 2,799 / 2,827
ms.  The difference (about 1%) is within the spread between runs of the
same arm (about 1.5% for "now").

## A case the estimate can still miss

The rate estimate is still an estimate.  A model whose rate keeps
rising after the rule trusts it can stop further than tol away; the
largest miss measured is 1.05x, lk at tol 1e-2.

## Not measured

pg.ddx.io's data.  An A/A noise floor for the times.
