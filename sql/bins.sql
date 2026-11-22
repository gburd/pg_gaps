-- Binned attributes: a numeric column declared with bin edges, which
-- pg_gaps discretises on read.  Edges e1 < ... < en make n + 1 bins,
-- (-inf, e1), [e1, e2), ..., [en, inf); the domain labels them in order.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse
\set SHOW_CONTEXT never

-- Every boundary case, in each numeric type.  x = 1 and 2.5 sit on the
-- edges and go to the bin above; NaN is in no bin, so it is missing like
-- NULL; the infinities go to the outer bins.
CREATE TABLE s (id int PRIMARY KEY, x float8, n numeric, i int, r real, b bigint, sm smallint);
INSERT INTO s VALUES
	(1, -1, -1, 1, 0.7, 1, 1),
	(2, 1, 1, 2, 0.7, 2, 2),
	(3, 1.5, 1.5, 2, 0.69, 2, 3),
	(4, 2.5, 2.5, 3, 0.71, 3, 3),
	(5, 3, 3, 10, 'Infinity', 9, 9),
	(6, 'Infinity', 'Infinity', -5, '-Infinity', -9, -9),
	(7, '-Infinity', '-Infinity', NULL, 'NaN', NULL, NULL),
	(8, 'NaN', 'NaN', 2, NULL, 2, 2),
	(9, NULL, NULL, 2, 1, 2, 2);
SELECT gaps.create_model('b');
SELECT gaps.declare_attribute('b', 's', 'x', ARRAY['lo', 'mid', 'hi'], edges => ARRAY[1, 2.5]);
SELECT gaps.declare_attribute('b', 's', 'n', ARRAY['lo', 'mid', 'hi'], edges => ARRAY[1, 2.5]);
SELECT gaps.declare_attribute('b', 's', 'i', ARRAY['<2', '[2,3)', '>=3'], edges => ARRAY[2, 3]);
SELECT gaps.declare_attribute('b', 's', 'r', ARRAY['lo', 'hi'], edges => ARRAY[0.7]);
SELECT gaps.declare_attribute('b', 's', 'b', ARRAY['lo', 'hi'], edges => ARRAY[2]);
SELECT gaps.declare_attribute('b', 's', 'sm', ARRAY['lo', 'hi'], edges => ARRAY[2.5]);
SELECT attname, domain, edges FROM gaps.attribute ORDER BY attribute_id;
SELECT gaps.add_stripe('b', a, 's', a) FROM unnest(ARRAY['x', 'n', 'i', 'r', 'b', 'sm']) a;
SELECT gaps.add_factor('b', a, a) FROM unnest(ARRAY['x', 'n', 'i', 'r', 'b', 'sm']) a;

-- learn_cpts with alpha 0 is the bin counts, which width_bucket gives by
-- hand (NaN and NULL left out).
SELECT factor, complete_entities FROM gaps.learn_cpts('b', 0) ORDER BY 1;
WITH hand AS (
	SELECT 'x' AS a, width_bucket(x, ARRAY[1, 2.5]::float8[]) AS bin FROM s
		WHERE x IS NOT NULL AND x <> 'NaN'
	UNION ALL SELECT 'n', width_bucket(n, ARRAY[1, 2.5]::numeric[]) FROM s
		WHERE n IS NOT NULL AND n <> 'NaN'
	UNION ALL SELECT 'i', width_bucket(i, ARRAY[2, 3]) FROM s WHERE i IS NOT NULL
	UNION ALL SELECT 'r', width_bucket(r, ARRAY[0.7]::real[]) FROM s
		WHERE r IS NOT NULL AND r <> 'NaN'
	UNION ALL SELECT 'b', width_bucket(b, ARRAY[2]::bigint[]) FROM s WHERE b IS NOT NULL
	UNION ALL SELECT 'sm', width_bucket(sm, ARRAY[2.5]) FROM s WHERE sm IS NOT NULL),
counts AS (
	SELECT h.a, k, count(h.bin) FILTER (WHERE h.bin = k)::float8 / count(*) AS p
	FROM hand h CROSS JOIN generate_series(0, 2) k
	GROUP BY h.a, k)
SELECT f.name, f.cpt,
	   f.cpt = (SELECT array_agg(c.p ORDER BY c.k) FROM counts c
				WHERE c.a = f.name AND c.k < cardinality(f.cpt)) AS equals_hand
FROM gaps.factor f ORDER BY 1;
SELECT value, probability FROM gaps.cpt_rows('b', 'i');

-- The missing values (NULL and NaN) get the learned distribution over the
-- labels.
SELECT attname, entity, value, round(probability::numeric, 6) AS p
FROM gaps.marginals('b') ORDER BY 1, 2, 3;

-- A latent class with a binned child: evidence is the bin, so the
-- posterior of a given x is the hand value for x's bin.  P(a = 1) = .3;
-- P(bin | a): a = 0 {.7, .2, .1}, a = 1 {.1, .3, .6}.  x = 2.5 is in bin 3:
-- .3 * .6 / (.7 * .1 + .3 * .6) = .72.
CREATE TABLE lc (id int PRIMARY KEY, a text, x numeric);
INSERT INTO lc VALUES (1, NULL, 0.99), (2, NULL, 1), (3, NULL, 2.5), (4, NULL, 2.4999), (5, '1', 7);
SELECT gaps.create_model('lc');
SELECT gaps.declare_attribute('lc', 'lc', 'a', ARRAY['0', '1']);
SELECT gaps.declare_attribute('lc', 'lc', 'x', ARRAY['lo', 'mid', 'hi'], edges => ARRAY[1, 2.5]);
SELECT gaps.add_stripe('lc', 'a', 'lc', 'a'), gaps.add_stripe('lc', 'x', 'lc', 'x');
SELECT gaps.add_factor('lc', 'pa', 'a', '{}', ARRAY[0.7, 0.3]);
SELECT gaps.add_factor('lc', 'px', 'x', ARRAY['a'], ARRAY[0.7, 0.2, 0.1, 0.1, 0.3, 0.6]);
SELECT entity, round(probability::numeric, 6) AS p_a1
FROM gaps.marginals('lc') WHERE value = '1' ORDER BY 1;
SELECT entity, round(probability::numeric, 6) AS p_a1
FROM gaps.marginals('lc', 'lc', '[{"id": 3}]') WHERE value = '1';
SELECT round((0.3 * 0.1 / (0.7 * 0.7 + 0.3 * 0.1))::numeric, 6) AS lo,
	   round((0.3 * 0.3 / (0.7 * 0.2 + 0.3 * 0.3))::numeric, 6) AS mid,
	   round((0.3 * 0.6 / (0.7 * 0.1 + 0.3 * 0.6))::numeric, 6) AS hi;

-- A logistic factor with a binned parent: its weights are per bin, and a
-- row's score uses the weight of its value's bin.  bias -1, weights lo 0,
-- mid 1, hi 3: P(y = on) = 1 / (1 + exp(-(-1 + w(bin)))); x NULL averages
-- over the bins with x's prior (.5, .3, .2).
CREATE TABLE lg (id int PRIMARY KEY, x numeric, y text);
INSERT INTO lg VALUES (1, 0.5, NULL), (2, 1, NULL), (3, 2.5, NULL), (4, NULL, NULL),
	(5, NULL, 'on');
SELECT gaps.create_model('lg');
SELECT gaps.declare_attribute('lg', 'lg', 'x', ARRAY['lo', 'mid', 'hi'], edges => ARRAY[1, 2.5]);
SELECT gaps.declare_attribute('lg', 'lg', 'y', ARRAY['off', 'on']);
SELECT gaps.add_stripe('lg', 'x', 'lg', 'x'), gaps.add_stripe('lg', 'y', 'lg', 'y');
SELECT gaps.add_factor('lg', 'px', 'x', '{}', ARRAY[0.5, 0.3, 0.2]);
SELECT gaps.add_factor('lg', 'py', 'y', ARRAY['x'], kind => 'logistic',
					   params => ARRAY[-1, 0, 1, 3]);
WITH w(bin, wt, pr) AS (VALUES (1, 0.0, 0.5), (2, 1.0, 0.3), (3, 3.0, 0.2)),
hand AS (
	SELECT l.id, CASE WHEN l.x IS NULL THEN NULL ELSE width_bucket(l.x, ARRAY[1, 2.5]) + 1 END
		AS bin FROM lg l),
want AS (
	SELECT h.id,
		   CASE WHEN h.bin IS NULL
				THEN (SELECT sum(w.pr / (1 + exp(1 - w.wt))) FROM w)
				ELSE (SELECT 1 / (1 + exp(1 - w.wt)) FROM w WHERE w.bin = h.bin) END AS p
	FROM hand h WHERE h.id < 5)
SELECT m.entity, round(m.probability::numeric, 12) AS p_on,
	   abs(m.probability - want.p) < 1e-12 AS equals_hand
FROM gaps.marginals('lg') m JOIN want ON want.id = (m.entity ->> 'id')::int
WHERE m.attname = 'y' AND m.value = 'on' ORDER BY 1;
-- Row 5's observed y = on is evidence about its missing x: P(bin | on) is
-- the prior times P(on | bin), normalized.
WITH w(bin, label, wt, pr) AS (VALUES (1, 'lo', 0.0, 0.5), (2, 'mid', 1.0, 0.3),
								  (3, 'hi', 3.0, 0.2)),
z AS (SELECT sum(pr / (1 + exp(1 - wt))) AS z FROM w)
SELECT m.value, round(m.probability::numeric, 12) AS p,
	   abs(m.probability - w.pr / (1 + exp(1 - w.wt)) / z.z) < 1e-12 AS equals_hand
FROM gaps.marginals('lg', 'lg', '[{"id": 5}]') m JOIN w ON w.label = m.value, z
WHERE m.attname = 'x' ORDER BY w.bin;

-- Rejections.
CREATE TABLE t (id int PRIMARY KEY, x float8, c text, r real);
SELECT gaps.create_model('bad');
SELECT gaps.declare_attribute('bad', 't', 'c', ARRAY['lo', 'hi'], edges => ARRAY[1]);       -- fail: text
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['lo', 'hi'], edges => ARRAY[2, 1]);    -- fail: order
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['lo', 'hi'], edges => ARRAY[1, 1]);    -- fail: equal
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['a', 'b', 'c'],
							  edges => ARRAY[1, 'Infinity']::float8[]);                            -- fail: inf
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['a', 'b'], edges => '{1,NaN}');            -- fail: NaN
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['a', 'b'], edges => ARRAY[1, NULL]);   -- fail: NULL
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['a'], edges => '{}');                  -- fail: empty
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['a', 'b'], edges => ARRAY[[1], [2]]);  -- fail: 2-D
SELECT gaps.declare_attribute('bad', 't', 'x', ARRAY['lo', 'hi'], edges => ARRAY[1, 2]);    -- fail: labels
SELECT gaps.declare_attribute('bad', 't', 'r', ARRAY['a', 'b', 'c'],
							  edges => ARRAY[0.1, 0.10000000001]);                         -- fail: real
SELECT count(*) FROM gaps.attribute a JOIN gaps.model m USING (model_id) WHERE m.name = 'bad';
-- A binned column whose type changes is refused at grounding, as any.
ALTER TABLE lc ALTER COLUMN x TYPE text;
SELECT count(*) FROM gaps.marginals('lc');                                                  -- fail

SELECT gaps.drop_model(name) FROM gaps.model WHERE name IN ('b', 'lc', 'lg', 'bad');
