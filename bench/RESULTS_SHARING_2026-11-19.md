# First-order sharing and where the time goes (2026-11-19)

Host: EC2 c7i.4xlarge (16 vCPU, Intel Xeon Platinum 8488C, 30 GB), Fedora
44, PGDG PostgreSQL 18.7, gcc 16.2.1, idle (load 0.00 before the runs).
Workload: `bench/sharing.sql`, the hotel sensor model of the BayesStore
thesis section 4.5.1 without its two vertical factors: 100 rooms x 8
sensors x T time steps, a temperature prior and light-given-temperature
CPT shared by every sensor, 10% of values missing.  Timings are
`\timing` inside one session (AGENTS.md rule 4).

## Correctness first

With `gaps.share_components` on and off, `gaps.marginals('hotel')`
returned the same rows with bit-identical probabilities (T = 125: 39,846
rows, 18,901 components, 0 rows differ).

## Inference alone (gaps._infer on a pre-grounded network)

| T | variables | sharing | runs (ms) | median |
|---|---|---|---|---|
| 125 | 200,000 | on | 24.4, 17.2, 17.5 | 17.5 |
| 125 | 200,000 | off | 33.9, 38.4, 38.0 | 38.0 |
| 125 | 200,000 | on (A/A) | 22.8, 17.1, 16.7 | 17.1 |
| 1250 | 2,000,000 | on | 271.0, 269.0, 269.6 | 269.6 |
| 1250 | 2,000,000 | off | 431.1, 432.9, 435.1 | 432.9 |
| 1250 | 2,000,000 | on (A/A) | 273.1, 274.8, 273.6 | 273.6 |

Sharing makes inference 2.2x faster at T = 125 and 1.6x at T = 1250.  The
A/A runs agree within 2% of their first arm, well under the difference.
The thesis reported 5-14x for its best case (T3504-3508), but against a
different baseline (BNT variable elimination outside the database); this
engine without sharing is already linear and fast, so there is less to
win.

## The whole call (gaps.marginals)

| T | median of 5 (ms) |
|---|---|
| 125 | 15,622 |

Of that, inference is 17 ms.  The rest is grounding in PL/pgSQL.  At
T = 1250 (2M variables) one grounding took 45.7 s, broken down with
auto_explain: building the entity tables 8.6 s, numbering the variables
4.0 s, child instances 8.9 s, the parent join and its check 10.8 s, the
coverage check 1.9 s, assembling arrays 6.2 s.  The jsonb entity keys
are built and compared in every step.

## Grounding, after the first fixes

The breakdown showed every step paying for jsonb entity keys and several
steps reading the table more than once.  Changes, each checked by the
regression suite and by 2,647 marginals against pgmpy (`test/pgmpy`):
entities, evidence and both evidence checks in one aggregation per
attribute; child instances by joining the stripe's distinct entities to
the variables instead of a correlated EXISTS; each parent mapping and its
"exactly one parent" check in one pass; the arrays assembled from one
ordered scan; and result rows named by a join, not by subscripting a
jsonb array (`array[i]` on a varlena array scans from its start, which
made `gaps.marginals` quadratic: at T = 1250 it ran 13 minutes before I
cancelled it, inside `array_seek` per gdb).

| T | variables | `_ground` before (ms) | `_ground` after, 3 runs (ms) | `marginals` after, 3 runs (ms) |
|---|---|---|---|---|
| 125 | 200,000 | 15,622 (whole call) | 2,186, 2,146, 2,152 | 2,254, 2,217, 2,220 |
| 1250 | 2,000,000 | 45,742 | 23,654, 23,336, 23,462 | 24,557, 24,465, 24,731 |

Grounding is now linear in the data (x10.9 for x10 rows) and 4.6x to 7x
faster than before; inference adds 2-5% on top.  It is still about 100
times the inference cost.  What is left is the jsonb keys themselves:
built for every row and joined on in every step.

## Conclusion

Sharing is real, 1.6-2.2x on inference, but inference is a small part of
a call.  Grounding was 900 times the cost of inference and is now about
100 times; the jsonb entity keys are the remaining cost and the first
item for Phase 6.  Unmeasured: grounding with vertical factors and
coarse entity sets, other entity-key types, and PG 17 and 19.
