# learn_cpts at scale (2026-11-21)

Host: floki (48 CPUs, shared; the one-minute load average was 9 to 14
throughout), PostgreSQL 18.7 from the dev shell, one throwaway cluster
per run (`dev/regress.sh --`).  "before" is wip/em at cece619; "after"
is the same tree with the fixes of the commit that adds this file.  Each
run prints an md5 of every `gaps` function body (CREATE EXTENSION copies
them into the catalog): 55e8ee58... in every before run, 0aad68f2... in
every after run.

Workloads, from `bench/learn.sql`: "chain", a -> b over n rows of one
table, a third of a missing (the reviewer's model); "latent", pg.ddx.io's
shape, a 2-state latent A with three 3-state children, 20,000 rows drawn
from planted CPTs with A observed in the first 200.  Times are `\timing`
inside one session, two calls each; "after A/A" is a second, separate
run of the after arm (its own cluster and database).

## Correctness first

The learned CPTs agree between the arms on every workload, to the last
one or two bits of a float8 (the after arm no longer forces each row's
last cell to 1 minus the others; see the commit).  EM on the latent model
with tol 0 and 45 iterations, the iteration count the before arm's
absolute tolerance stopped at, gives loglik -60593.889007508995 before
and -60593.88900750899 after.

## learn_cpts, method 'counts'

| workload | before | after | after A/A |
|---|---|---|---|
| chain, 2,550 rows | 1,926 / 1,891 ms | 75 / 67 ms | 79 / 70 ms |
| chain, 10,200 rows | 29,124 / 29,165 ms | 256 / 241 ms | 254 / 242 ms |
| chain, 40,800 rows | 473,849 / 470,022 ms | 865 / 825 ms | 859 / 825 ms |
| latent, 20,000 rows | 455,230 / 500,189 ms | 836 / 815 ms | 840 / 820 ms |

Before, every fourfold step in rows costs about 16 times the time: the
counting query self-joined pg_gaps_inst, which had no statistics, and
the planner ran it as a nested loop that rescanned the table per
instance.  After, a fourfold step costs about 3.4 times, and most of the
cost is grounding.

## learn_cpts, method 'em', latent model, 20,000 rows

| | before | after | after A/A |
|---|---|---|---|
| default tol (1e-6) | 45 iterations, 492,518 / 487,262 ms | 7 iterations, 993 / 1,014 ms | 7 iterations, 1,002 / 1,007 ms |
| tol 0, 45 iterations | (the row above) | 2,012 / 1,975 ms | |

The default stops sooner after because the tolerance is now relative to
the objective (about 6e4 here).  Max error against the planted CPTs at
7 iterations: 0.0060 (P(A)), 0.0086, 0.0139, 0.0115 (the children); at
45: 0.0071, 0.0097, 0.0146, 0.0120.

## The M-step on a large CPT

The reviewer's `mstep.sql` (child with two 60-valued parents, 36,000
cells, 2,000 rows) with a seventh of the first parent missing, so that
EM does not converge in one step.  Two runs in one session per arm.

| call | before | after |
|---|---|---|
| 'counts' | 3,201 / 3,411 ms | 624 / 632 ms |
| 'em', 1 iteration | 4,478 / 4,639 ms | 963 / 961 ms |
| 'em', 3 iterations | 7,061 / 7,182 ms | 1,531 / 1,525 ms |

Each extra iteration (E-step, objective and M-step) costs about 1,290 ms
before and 283 ms after.  Both arms report the same loglik after 3
iterations, -16766.20383328718.

## Plans

auto_explain (log_min_duration 50 ms, nested statements) over
`marginals`, `impute` and `learn_cpts(..., 'em', 3, 0)` on the latent
model and on a 40,800-row chain: no sequential scan of a pg_gaps temp
table runs more than once; the per-row loops are index scans of the
entity tables and Memoize nodes, about 1 us per probe.

## Not measured

pg.ddx.io's 790,000-row `learn_cpts` (34.5 s on their host).  The before
arm cannot explain that number at a quadratic cost, so their planner
probably chose a hash join there (inferred, not reproduced).  The
before arm's 40,800-row and latent runs were not repeated in a second
session; at 8 minutes per call the two calls in one session are its
only A/A.
