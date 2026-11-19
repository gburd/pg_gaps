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

DROP TABLE p;
SELECT gaps.drop_model('lm');
