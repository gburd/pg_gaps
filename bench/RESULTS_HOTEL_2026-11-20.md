# The full hotel workload, with its vertical factors (2026-11-20)

Host: floki (48 CPUs), PostgreSQL 18.7 from the dev shell, shared with
other jobs (load average about 7 before the runs), so the timings are
indicative only and no comparison is drawn from them.  Workload:
`bench/hotel.sql`, the hotel sensor model of the BayesStore thesis
section 4.5.1 (T3259-3269) with all four first-order factors: the
temperature prior on sensors 1, 3 and 5-8, sensor 2 given sensor 1 and
sensor 4 given sensor 3 in the same room and time, and light given
temperature.  10% of values missing.  Default bounds, `gaps.samples` 0.

| T | rows | missing | components | largest component | method | gaps.marginals, 5 runs (ms) | median |
|---|---|---|---|---|---|---|---|
| 125 | 100,000 | 19,923 | 18,640 | 4 variables | exact | 3955, 3990, 3956, 3961, 3946 | 3956 |
| 500 | 400,000 | 80,577 | 75,476 | 4 variables | exact | 17056, 17045, 16909, 16855, 16885 | 16909 |

The vertical factors join at most a sensor pair's two temperatures and
their two lights, so every component has at most 4 variables and exact
inference answers all of them; sampling is not needed for this workload.
The time grows about linearly (4.3x for 4x the rows) and is mostly
grounding, as `bench/RESULTS_SHARING_2026-11-19.md` found for the
horizontal-only form.
