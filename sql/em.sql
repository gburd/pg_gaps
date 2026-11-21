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

-- EM in learn_cpts.  pg.ddx.io's t5 case: 10 rows (x, u), 10 rows (y, v),
-- and 1000 rows with a missing and b = v.  Counting complete rows ignores
-- the 1000.  With alpha = 0 the complete-data start has P(b = v | a = x)
-- = 0, so each of the 1000 has a = y with posterior 1: P(a) moves to
-- (10, 1010) / 1020 in one step and stays, P(b | a) stays (1, 0 | 0, 1),
-- and log P(evidence) = 10 ln(10/1020) + 1010 ln(1010/1020).
CREATE TABLE h (id int PRIMARY KEY, a text, b text);
INSERT INTO h SELECT g, 'x', 'u' FROM generate_series(1, 10) g;
INSERT INTO h SELECT g, 'y', 'v' FROM generate_series(11, 20) g;
INSERT INTO h SELECT g, NULL, 'v' FROM generate_series(21, 1020) g;
SELECT gaps.create_model('h');
SELECT gaps.declare_attribute('h', 'h', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('h', 'h', 'b', ARRAY['u', 'v']);
SELECT gaps.add_stripe('h', 'a', 'h', 'a');
SELECT gaps.add_stripe('h', 'b', 'h', 'b');
SELECT gaps.add_factor('h', 'pa', 'a');
SELECT gaps.add_factor('h', 'pb', 'b', ARRAY['a']);
SELECT factor, complete_entities, method, iterations, converged,
	   abs(loglik - (10 * ln(10.0 / 1020) + 1010 * ln(1010.0 / 1020))) < 1e-9 AS loglik_ok
FROM gaps.learn_cpts('h', 0, 'em') ORDER BY factor;
SELECT bool_and(abs(c - w) < 1e-12) AS hand
FROM gaps.factor f,
	 unnest(f.cpt, CASE f.name WHEN 'pa' THEN ARRAY[10.0 / 1020, 1010.0 / 1020]
				   ELSE ARRAY[1, 0, 0, 1] END) AS u(c, w);
-- The default method is unchanged: complete rows only.
SELECT factor, method, iterations FROM gaps.learn_cpts('h', 0) ORDER BY factor;
SELECT cpt FROM gaps.factor WHERE name = 'pa';

-- With alpha > 0, P(b | a) moves too.  The update (expected count +
-- pseudocount) / (total + sum of pseudocounts) is EM for the posterior
-- under a Dirichlet(pseudocount + 1) prior, so the log posterior,
-- loglik + sum(pseudocount * ln(cell)), must not decrease from one
-- iteration to the next.  learn_cpts with max_iterations = t and tol 0
-- returns the t-th iterate and its loglik; alpha puts that pseudocount in
-- every cell.  At pseudocounts other than 1 the posterior-mean and the
-- MAP readings of the update differ, so several are checked.
CREATE TEMP TABLE path (a float8, t int, its int, obj float8);
DO $$
DECLARE
	r record;
	a float8;
BEGIN
	FOREACH a IN ARRAY ARRAY[0.5, 1, 3, 50] LOOP
		FOR t IN 1 .. 10 LOOP
			SELECT max(l.iterations) AS its, max(l.loglik) AS ll INTO r
			FROM gaps.learn_cpts('h', a, 'em', t, 0) l;
			INSERT INTO path
			SELECT a, t, r.its, r.ll + sum(a * ln(x))
			FROM gaps.factor f, unnest(f.cpt) x WHERE f.model_id = gaps._model_id('h');
		END LOOP;
	END LOOP;
END
$$;
SELECT a, count(*) AS steps, bool_and(its = t) AS all_ran,
	   bool_and(d >= -1e-12) AS monotone, sum(d) > 1e-4 AS moved
FROM (SELECT a, t, its, obj - lag(obj) OVER (PARTITION BY a ORDER BY t) AS d FROM path) x
WHERE d IS NOT NULL GROUP BY a ORDER BY a;
-- And it converges: by 500 iterations an iteration changes the objective
-- by at most 1e-6 of its size.
SELECT DISTINCT method, converged, iterations < 500 AS early
FROM gaps.learn_cpts('h', 1, 'em', 500);

-- A tiny positive pseudocount keeps its cells positive, and EM's
-- objective stays finite: P(b = v | a = x) is 1e-20 / (10 + 2e-20), below
-- the rounding of the row's other cell.
SELECT factor, method, converged FROM gaps.learn_cpts('h', 1e-20, 'em') ORDER BY factor;
SELECT bool_and(x > 0) AS positive, bool_and(abs(s - 1) < 1e-12) AS rows_sum_to_1
FROM (SELECT f.name, x, sum(x) OVER (PARTITION BY f.name, (i - 1) / 2) AS s
	  FROM gaps.factor f, unnest(f.cpt) WITH ORDINALITY u(x, i)
	  WHERE f.model_id = gaps._model_id('h')) c;

SELECT * FROM gaps.learn_cpts('h', 1, 'magic');              -- fail
SELECT * FROM gaps.learn_cpts('h', 1, 'em', 0);              -- fail
SELECT * FROM gaps.learn_cpts('h', 1, 'em', 10, 'NaN');      -- fail

-- Too large for exact inference is an ERROR, sampling or not.
SET gaps.max_total_cells = 8;
SET gaps.samples = 100;
SELECT * FROM gaps.learn_cpts('h', 1, 'em');                 -- fail
RESET gaps.max_total_cells;
RESET gaps.samples;

DROP TABLE h;
SELECT gaps.drop_model('h');

-- With alpha = 0 the complete-data estimate can give probability 0 to a
-- value only incomplete rows show: here P(b = v | a) = 0 for both a, and
-- an entity with b = v and a missing is impossible under it.  EM starts
-- instead from the counts plus 1, (11, 11 | 11, 1 | 11, 1) / ..., and by
-- symmetry reaches P(a) = (0.5, 0.5) and P(b | a) = (10, 0.5) / 10.5 for
-- both a in one step.
CREATE TABLE z (id int PRIMARY KEY, a text, b text);
INSERT INTO z SELECT g, CASE WHEN g <= 10 THEN 'x' ELSE 'y' END, 'u'
FROM generate_series(1, 20) g;
INSERT INTO z VALUES (21, NULL, 'v');
SELECT gaps.create_model('z');
SELECT gaps.declare_attribute('z', 'z', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('z', 'z', 'b', ARRAY['u', 'v']);
SELECT gaps.add_stripe('z', 'a', 'z', 'a');
SELECT gaps.add_stripe('z', 'b', 'z', 'b');
SELECT gaps.add_factor('z', 'pa', 'a');
SELECT gaps.add_factor('z', 'pb', 'b', ARRAY['a']);
SELECT factor, method, iterations, converged,
	   abs(loglik - (20 * ln(0.5 * 10 / 10.5) + ln(0.5 / 10.5))) < 1e-9 AS loglik_ok
FROM gaps.learn_cpts('z', 0, 'em') ORDER BY factor;
SELECT bool_and(abs(c - w) < 1e-12) AS hand
FROM gaps.factor f,
	 unnest(f.cpt, CASE f.name WHEN 'pa' THEN ARRAY[0.5, 0.5]
				   ELSE ARRAY[10 / 10.5, 0.5 / 10.5, 10 / 10.5, 0.5 / 10.5] END) AS u(c, w)
WHERE f.model_id = gaps._model_id('z');
DROP TABLE z;
SELECT gaps.drop_model('z');

-- pg.ddx.io's shape: a latent class a, observed for a few labelled rows,
-- with two children; no labelled row shows c = 3, which most unlabelled
-- rows do.  With alpha = 0 the start is again the counts plus 1, and EM
-- learns P(c = 3 | a) from the unlabelled rows.
CREATE TABLE lc (id int PRIMARY KEY, a text, b text, c text);
INSERT INTO lc SELECT g, CASE WHEN g % 2 = 0 THEN 's' ELSE 't' END,
	   CASE WHEN g % 2 = 0 THEN '1' ELSE '2' END, CASE WHEN g % 2 = 0 THEN '1' ELSE '2' END
FROM generate_series(1, 6) g;
INSERT INTO lc SELECT g, NULL, CASE WHEN g % 3 = 0 THEN '1' ELSE '2' END,
	   CASE WHEN g % 4 = 0 THEN '1' ELSE '3' END
FROM generate_series(7, 60) g;
SELECT gaps.create_model('lc');
SELECT gaps.declare_attribute('lc', 'lc', 'a', ARRAY['s', 't']);
SELECT gaps.declare_attribute('lc', 'lc', x, ARRAY['1', '2', '3'])
FROM unnest(ARRAY['b', 'c']) x;
SELECT gaps.add_stripe('lc', x, 'lc', x) FROM unnest(ARRAY['a', 'b', 'c']) x;
SELECT gaps.add_factor('lc', 'pa', 'a');
SELECT gaps.add_factor('lc', 'p' || x, x, ARRAY['a']) FROM unnest(ARRAY['b', 'c']) x;
SELECT factor, complete_entities, method, converged FROM gaps.learn_cpts('lc', 0, 'em')
ORDER BY factor;

-- Only the start's "probability zero" error triggers the add-one restart:
-- a directed cycle (22023, the same SQLSTATE class) is reported as itself.
CREATE TABLE cyc (id int PRIMARY KEY, a text, b text);
INSERT INTO cyc VALUES (1, NULL, 'x');
SELECT gaps.create_model('cyc');
SELECT gaps.declare_attribute('cyc', 'cyc', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('cyc', 'cyc', 'b', ARRAY['x', 'y']);
SELECT gaps.add_stripe('cyc', 'a', 'cyc', 'a');
SELECT gaps.add_stripe('cyc', 'b', 'cyc', 'b');
SELECT gaps.add_factor('cyc', 'ab', 'a', ARRAY['b']);
SELECT gaps.add_factor('cyc', 'ba', 'b', ARRAY['a']);
SELECT gaps.map_factor('cyc', 'ab', 1, 'id', 'id');
SELECT gaps.map_factor('cyc', 'ba', 1, 'id', 'id');
SELECT * FROM gaps.learn_cpts('cyc', 0, 'em');                   -- fail: cycle, no restart
SELECT gaps.drop_model('cyc');
DROP TABLE cyc;
-- P(c = 3) under the learned model: the share of rows with c = 3, 40 / 60,
-- since EM's fixed points match the observed marginal of a child that is
-- never missing.
SELECT round((pa.cpt[1] * pc.cpt[3] + pa.cpt[2] * pc.cpt[6])::numeric, 6) AS p_c3,
	   round(40 / 60.0, 6) AS observed
FROM gaps.factor pa, gaps.factor pc
WHERE pa.model_id = gaps._model_id('lc') AND pa.name = 'pa'
  AND pc.model_id = pa.model_id AND pc.name = 'pc';
DROP TABLE lc;
SELECT gaps.drop_model('lc');

-- On complete data the E-step returns the counts, so EM stops after one
-- iteration with the counting answer.
CREATE TABLE c (id int PRIMARY KEY, a text, b text);
INSERT INTO c VALUES (1, 'x', 'u'), (2, 'x', 'v'), (3, 'y', 'v');
SELECT gaps.create_model('c');
SELECT gaps.declare_attribute('c', 'c', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('c', 'c', 'b', ARRAY['u', 'v']);
SELECT gaps.add_stripe('c', 'a', 'c', 'a');
SELECT gaps.add_stripe('c', 'b', 'c', 'b');
SELECT gaps.add_factor('c', 'pa', 'a');
SELECT gaps.add_factor('c', 'pb', 'b', ARRAY['a']);
SELECT * FROM gaps.learn_cpts('c', 1) ORDER BY factor;
CREATE TEMP TABLE counted AS SELECT name, cpt FROM gaps.factor
WHERE model_id = gaps._model_id('c');
SELECT factor, method, iterations, converged FROM gaps.learn_cpts('c', 1, 'em') ORDER BY 1;
SELECT bool_and(abs(x - y) < 1e-12) AS same
FROM gaps.factor f JOIN counted USING (name), unnest(f.cpt, counted.cpt) u(x, y)
WHERE f.model_id = gaps._model_id('c');
DROP TABLE c;
SELECT gaps.drop_model('c');
