-- Expected counts (the E-step of EM), on a network small enough to work
-- by hand.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- Three instances of x -> y, P(x) = (0.3, 0.7) at offset 1 and P(y | x) =
-- (0.9, 0.1 | 0.2, 0.8) at offset 3.  Variables 1, 2: x missing, y = 1.
-- 3, 4: both missing.  5, 6: x = 2, y = 2.
--
-- By hand: instance 1 has P(x = 1 | y = 1) = 0.27 / 0.41 and P(x = 2 |
-- y = 1) = 0.14 / 0.41; instance 2's family posterior is the joint (0.27,
-- 0.03, 0.14, 0.56); instance 3 adds 1 to its cells.  P(evidence) = 0.41
-- * 1 * (0.7 * 0.8).
CREATE TEMP TABLE hand (cpt_off int, cell int, n float8);
INSERT INTO hand VALUES
	(1, 1, 0.27 / 0.41 + 0.3), (1, 2, 0.14 / 0.41 + 0.7 + 1),
	(3, 1, 0.27 / 0.41 + 0.27), (3, 2, 0.03),
	(3, 3, 0.14 / 0.41 + 0.14), (3, 4, 0.56 + 1);
CREATE TEMP TABLE got AS
SELECT * FROM gaps._expected_counts('{2,2,2,2,2,2}', '{0,1,0,0,2,2}',
		'{1,2,3,4,5,6}', '{1,1,2,2,3,3,4}', '{1,3,5}', '{1,3,1,3,1,3}',
		'{0.3,0.7,0.9,0.1,0.2,0.8}');
SELECT g.cpt_off, g.cell, round(g.n::numeric, 9) AS n,
	   abs(g.n - h.n) < 1e-12 AS agrees
FROM got g FULL JOIN hand h USING (cpt_off, cell)
ORDER BY 1, 2;
SELECT count(DISTINCT loglik) AS one_loglik,
	   bool_and(abs(loglik - ln(0.41 * 0.56)) < 1e-12) AS loglik_agrees
FROM got;
-- Each factor's counts sum to its number of instances.
SELECT cpt_off, sum(n) FROM got GROUP BY cpt_off ORDER BY 1;

-- The same arguments are checked as for gaps._infer.
SELECT * FROM gaps._expected_counts('{2}', '{3}', '{1}', '{1,1}', '{}', '{1}',
		'{0.5,0.5}');                                       -- fail: evidence
SELECT * FROM gaps._expected_counts('{2}', '{0}', '{1}', '{1,1}', '{}', '{1}',
		'{0.5}');                                           -- fail: short CPT
-- Two CPTs at one offset must have the same size.
SELECT * FROM gaps._expected_counts('{2,3}', '{0,0}', '{1,2}', '{1,1,1}', '{}',
		'{1,1}', '{0.2,0.3,0.5}');                          -- fail
-- Evidence of probability zero.
SELECT * FROM gaps._expected_counts('{2}', '{1}', '{1}', '{1,1}', '{}', '{1}',
		'{0,1}');                                           -- fail
-- Too large for exact inference is an ERROR, sampling or not.
SET gaps.max_total_cells = 8;
SET gaps.samples = 100;
SELECT * FROM gaps._expected_counts('{2,2}', '{0,0}', '{1,2}', '{1,1,2}', '{1}',
		'{1,3}', '{0.5,0.5,0.5,0.5,0.5,0.5}');              -- fail
RESET gaps.max_total_cells;
RESET gaps.samples;
