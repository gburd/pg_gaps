-- Learning CPTs from complete data, checked against counts computed
-- directly in SQL.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

CREATE TABLE p (id int PRIMARY KEY, a text, b text);
-- a: 6 x, 2 y observed; b given a: x -> (u,u,u,v), y -> (v), one row with
-- b but no a, one with a but no b, one with neither.
INSERT INTO p VALUES
	(1, 'x', 'u'), (2, 'x', 'u'), (3, 'x', 'u'), (4, 'x', 'v'),
	(5, 'x', NULL), (6, 'x', NULL), (7, 'y', 'v'), (8, 'y', NULL),
	(9, NULL, 'u'), (10, NULL, NULL);
SELECT gaps.create_model('lm');
SELECT gaps.declare_attribute('lm', 'p', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('lm', 'p', 'b', ARRAY['u', 'v', 'w']);
SELECT gaps.add_stripe('lm', 'a', 'p', 'a');
SELECT gaps.add_stripe('lm', 'b', 'p', 'b');
SELECT gaps.add_factor('lm', 'pa', 'a');
SELECT gaps.add_factor('lm', 'pb', 'b', ARRAY['a']);

-- Unsmoothed: exact frequencies; P(b | a = y) has one observation.
SELECT * FROM gaps.learn_cpts('lm', 0) ORDER BY factor;
SELECT name, cpt FROM gaps.factor ORDER BY name;
-- pa: 6/8, 2/8.  pb: a=x: 3/4, 1/4, 0; a=y: 0, 1, 0.
SELECT bool_and(abs(c - w) < 1e-12) AS exact
FROM gaps.factor f,
	 unnest(f.cpt, CASE f.name WHEN 'pa' THEN ARRAY[0.75, 0.25]
				   ELSE ARRAY[0.75, 0.25, 0, 0, 1, 0] END) AS u(c, w);

-- Smoothed with alpha = 1: (count + 1) / (total + |domain|).
SELECT * FROM gaps.learn_cpts('lm') ORDER BY factor;
SELECT bool_and(abs(c - w) < 1e-12) AS smoothed
FROM gaps.factor f,
	 unnest(f.cpt, CASE f.name WHEN 'pa' THEN ARRAY[7.0 / 10, 3.0 / 10]
				   ELSE ARRAY[4.0 / 7, 2.0 / 7, 1.0 / 7, 1.0 / 4, 2.0 / 4, 1.0 / 4] END) AS u(c, w);

-- A learned model infers.
SELECT entity, value, round(probability::numeric, 6) AS p
FROM gaps.marginals('lm') WHERE attname = 'b' AND entity = '{"id": 10}' ORDER BY value;

SELECT * FROM gaps.learn_cpts('lm', -1);           -- fail: negative alpha
SELECT * FROM gaps.learn_cpts('lm', 'NaN');        -- fail: NaN alpha

-- A parent combination never seen complete, with alpha 0: uniform.
DELETE FROM p WHERE a = 'y';
SELECT * FROM gaps.learn_cpts('lm', 0) ORDER BY factor;
SELECT cpt FROM gaps.factor WHERE name = 'pb';

-- Priors.  A prior replaces alpha for its factor: cell = (count + prior) /
-- (row total + sum(prior row)).  pb's rows: a=x has counts (3,1,0), a=y (0,0,0)
-- now that the y rows are gone.
SELECT gaps.set_prior('lm', 'pb', ARRAY[1, 2, 3, 0.5, 0.5, 1]);
SELECT * FROM gaps.learn_cpts('lm', 5) ORDER BY factor;
SELECT bool_and(abs(c - w) < 1e-12) AS prior_rows
FROM gaps.factor f,
	 unnest(f.cpt, ARRAY[4.0 / 10, 3.0 / 10, 3.0 / 10, 0.25, 0.25, 0.5]) AS u(c, w)
WHERE f.name = 'pb';
-- pa has no prior, so alpha = 5 applies to it: (6 + 5) / 16, (0 + 5) / 16.
SELECT bool_and(abs(c - w) < 1e-12) AS alpha_elsewhere
FROM gaps.factor f, unnest(f.cpt, ARRAY[11.0 / 16, 5.0 / 16]) AS u(c, w)
WHERE f.name = 'pa';
-- Rule 14 on every learned CPT.
SELECT f.name, r, abs(sum(c) - 1) < 1e-12 AS sums_to_1, min(c) >= 0 AS nonneg
FROM gaps.factor f, unnest(f.cpt) WITH ORDINALITY u(c, i),
	 LATERAL (SELECT (i - 1) / gaps._card(f.child_stripe) AS r) x
GROUP BY f.name, r ORDER BY 1, 2;
-- A zero prior row with no counts stays uniform.
SELECT gaps.set_prior('lm', 'pb', ARRAY[1, 1, 1, 0, 0, 0]);
SELECT * FROM gaps.learn_cpts('lm') ORDER BY factor;
SELECT cpt[4:6] AS uniform FROM gaps.factor WHERE name = 'pb';
-- NULL clears the prior: alpha again, (count + 1) / (total + 3).
SELECT gaps.set_prior('lm', 'pb', NULL);
SELECT * FROM gaps.learn_cpts('lm') ORDER BY factor;
SELECT bool_and(abs(c - w) < 1e-12) AS alpha_again
FROM gaps.factor f,
	 unnest(f.cpt, ARRAY[4.0 / 7, 2.0 / 7, 1.0 / 7, 1.0 / 3, 1.0 / 3, 1.0 / 3]) AS u(c, w)
WHERE f.name = 'pb';

-- Rejected priors.
SELECT gaps.set_prior('lm', 'pb', ARRAY[1, 1, 1, 1, 1, -1]);       -- fail: negative
SELECT gaps.set_prior('lm', 'pb', ARRAY[1, 1, 1, 1, 1, 'NaN'::float8]); -- fail: NaN
SELECT gaps.set_prior('lm', 'pb', ARRAY[1, 1, 1, 1, 1, 'Infinity'::float8]); -- fail: infinite
SELECT gaps.set_prior('lm', 'pb', ARRAY[1, 1, 1]);                 -- fail: one parent row
SELECT gaps.set_prior('lm', 'nope', ARRAY[1, 1]);                  -- fail: no factor
UPDATE gaps.factor SET prior = ARRAY[1, -1, 1] WHERE name = 'pb';  -- fail: trigger
UPDATE gaps.factor SET prior = ARRAY[1, 1] WHERE name = 'pb';      -- fail: not rows of 3
-- Direct DML that passes the trigger but has the wrong length for the
-- parents is refused when the model is next learned or grounded.
BEGIN;
UPDATE gaps.factor SET prior = ARRAY[1, 1, 1] WHERE name = 'pb';
SELECT * FROM gaps.learn_cpts('lm');                                -- fail: length
ROLLBACK;
BEGIN;
UPDATE gaps.factor SET prior = ARRAY[1, 1, 1] WHERE name = 'pb';
SELECT count(*) FROM gaps.marginals('lm');                          -- fail: length
ROLLBACK;

DROP TABLE p;
SELECT gaps.drop_model('lm');

-- pg.ddx.io's hazard cell: a Beta(8.70, 191.30) prior, n = 200 exposures
-- with d = 9 events.  Posterior mean (a + d) / (a + b + n) = 17.7 / 400.
CREATE TABLE hz (id int PRIMARY KEY, ev text);
INSERT INTO hz SELECT i, CASE WHEN i <= 9 THEN 'yes' ELSE 'no' END
FROM generate_series(1, 200) i;
SELECT gaps.create_model('hz');
SELECT gaps.declare_attribute('hz', 'hz', 'ev', ARRAY['yes', 'no']);
SELECT gaps.add_stripe('hz', 'ev', 'hz', 'ev');
SELECT gaps.add_factor('hz', 'ev', 'ev');
SELECT gaps.set_prior('hz', 'ev', ARRAY[8.70, 191.30]);
SELECT * FROM gaps.learn_cpts('hz', 100);
SELECT abs(cpt[1] - (8.70 + 9) / (8.70 + 191.30 + 200)) < 1e-12 AS beta_mean,
	   round(cpt[1]::numeric, 5) AS p_yes
FROM gaps.factor WHERE name = 'ev';
-- Arrays indexed from 0 would shift every cell, so both are refused.
SELECT gaps.set_prior('hz', 'ev', '[0:1]={8.70,191.30}');          -- fail: lower bound
SELECT gaps.set_cpt('hz', 'ev', '[0:1]={0.5,0.5}');                -- fail: lower bound
DROP TABLE hz;
SELECT gaps.drop_model('hz');

-- cpt_rows on a factor with two parents, one of whose domains is in an
-- order sorting would change: rows in the CPT's order, values in
-- declaration order.
CREATE TABLE cr (id int PRIMARY KEY, a text, b text, c text);
SELECT gaps.create_model('cr');
SELECT gaps.declare_attribute('cr', 'cr', 'a', ARRAY['1', '2', '10']);
SELECT gaps.declare_attribute('cr', 'cr', 'b', ARRAY['n', 'y']);
SELECT gaps.declare_attribute('cr', 'cr', 'c', ARRAY['lo', 'hi']);
SELECT gaps.add_stripe('cr', 'sa', 'cr', 'a');
SELECT gaps.add_stripe('cr', 'sb', 'cr', 'b');
SELECT gaps.add_stripe('cr', 'sc', 'cr', 'c');
SELECT gaps.add_factor('cr', 'pc', 'sc', ARRAY['sa', 'sb'],
	ARRAY[0.1, 0.9, 0.2, 0.8, 0.3, 0.7, 0.4, 0.6, 0.5, 0.5, 0.6, 0.4]);
SELECT gaps.set_prior('cr', 'pc', ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
SELECT * FROM gaps.cpt_rows('cr', 'pc') WITH ORDINALITY;
SELECT count(*) AS parent_rows, sum(n) AS cells,
	   bool_and(abs(s - 1) < 1e-12) AS rows_sum_to_1
FROM (SELECT parents, count(*) n, sum(probability) s FROM gaps.cpt_rows('cr', 'pc') GROUP BY parents) x;
-- A stripe twice among the parents keeps both, in order.
SELECT gaps.add_factor('cr', 'pb', 'sb', ARRAY['sa', 'sa']);
SELECT parents, value, probability, prior FROM gaps.cpt_rows('cr', 'pb') LIMIT 3;
SELECT * FROM gaps.cpt_rows('cr', 'nope');                         -- fail: no factor
DROP TABLE cr;


-- A cell that should be 0 stays 0 after the row-sum slack fix: with
-- counts (6, 23, 1, 0) and alpha 0 the slack used to come out -2.2e-16.
CREATE TABLE s4 (id int PRIMARY KEY, v text);
INSERT INTO s4 SELECT i, CASE WHEN i <= 6 THEN 'a' WHEN i <= 29 THEN 'b' ELSE 'c' END
FROM generate_series(1, 30) i;
SELECT gaps.create_model('s4');
SELECT gaps.declare_attribute('s4', 's4', 'v', ARRAY['a', 'b', 'c', 'd']);
SELECT gaps.add_stripe('s4', 'v', 's4', 'v');
SELECT gaps.add_factor('s4', 'pv', 'v');
SELECT * FROM gaps.learn_cpts('s4', 0);
SELECT cpt[4] = 0 AS last_is_zero FROM gaps.factor WHERE name = 'pv';
SELECT gaps.drop_model('s4');
DROP TABLE s4;
SELECT gaps.drop_model('cr');
