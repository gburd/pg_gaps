-- Changing a learned model: frozen factors, adding and dropping factors,
-- and lineage.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- A latent class a with children b and c, 600 rows of which 60 are
-- labelled.
CREATE TABLE lt (id int PRIMARY KEY, a text, b text, c text, d text);
INSERT INTO lt
SELECT g, CASE WHEN g <= 60 THEN CASE WHEN g % 3 = 0 THEN 's' ELSE 't' END END,
	   CASE WHEN g % 3 = 0 THEN CASE WHEN g % 5 = 0 THEN '2' ELSE '1' END
			ELSE CASE WHEN g % 7 = 0 THEN '1' ELSE '2' END END,
	   CASE WHEN g % 3 = 0 THEN CASE WHEN g % 4 = 0 THEN '2' ELSE '1' END
			ELSE CASE WHEN g % 11 = 0 THEN '1' ELSE '2' END END,
	   CASE WHEN g % 3 = 0 THEN 'y' ELSE CASE WHEN g % 2 = 0 THEN 'y' ELSE 'n' END END
FROM generate_series(1, 600) g;
SELECT gaps.create_model('lt');
SELECT gaps.declare_attribute('lt', 'lt', 'a', ARRAY['s', 't']);
SELECT gaps.declare_attribute('lt', 'lt', x, ARRAY['1', '2']) FROM unnest(ARRAY['b', 'c']) x;
SELECT gaps.add_stripe('lt', x, 'lt', x) FROM unnest(ARRAY['a', 'b', 'c']) x;
SELECT gaps.add_factor('lt', 'pa', 'a');
SELECT gaps.add_factor('lt', 'p' || x, x, ARRAY['a']) FROM unnest(ARRAY['b', 'c']) x;
SELECT factor, method, converged FROM gaps.learn_cpts('lt', 1, 'em') ORDER BY factor;

-- Frozen, by counts: pa keeps its CPT bit for bit and reports so, and the
-- others learn what they learn without the freeze (counting is per
-- factor).
SELECT gaps.set_cpt('lt', 'pa', ARRAY[0.04, 0.96]);
SELECT gaps.set_frozen('lt', 'pa', true);
SELECT * FROM gaps.learn_cpts('lt', 1) ORDER BY method, factor;
SELECT cpt = ARRAY[0.04, 0.96]::float8[] AS kept FROM gaps.factor WHERE name = 'pa';
CREATE TEMP TABLE frozen_counts AS SELECT name, cpt FROM gaps.factor
WHERE model_id = gaps._model_id('lt') AND name <> 'pa';
SELECT gaps.set_frozen('lt', 'pa', false);
SELECT count(*) FROM gaps.learn_cpts('lt', 1);
SELECT bool_and(f.cpt = x.cpt) AS others_as_unfrozen, count(*)
FROM gaps.factor f JOIN frozen_counts x USING (name) WHERE f.model_id = gaps._model_id('lt');

-- Frozen, by EM: pg.ddx.io's use, the prior row pinned while the children
-- refit.  pa is unchanged, EM still converges, and its objective, over the
-- unfrozen factors, never decreases: the t-th iterate at tol 0.
SELECT gaps.set_cpt('lt', 'pa', ARRAY[0.04, 0.96]);
SELECT gaps.set_frozen('lt', 'pa', true);
SELECT factor, complete_entities, method, converged FROM gaps.learn_cpts('lt', 1, 'em')
ORDER BY method, factor;
SELECT cpt = ARRAY[0.04, 0.96]::float8[] AS kept FROM gaps.factor WHERE name = 'pa';
CREATE TEMP TABLE path (t int, obj float8);
DO $$
DECLARE
	ll float8;
BEGIN
	FOR t IN 1 .. 8 LOOP
		SELECT max(l.loglik) INTO ll FROM gaps.learn_cpts('lt', 1, 'em', t, 0) l;
		INSERT INTO path SELECT t, ll + sum(ln(x))
		FROM gaps.factor f, unnest(f.cpt) x
		WHERE f.model_id = gaps._model_id('lt') AND NOT f.frozen;
	END LOOP;
END
$$;
SELECT count(*) AS steps, bool_and(d >= -1e-9) AS monotone, sum(d) > 1e-6 AS moved
FROM (SELECT obj - lag(obj) OVER (ORDER BY t) AS d FROM path) x WHERE d IS NOT NULL;
-- The oracle: at the fixed point with pa held, pb is the M-step of its
-- own E-step, (expected count + 1) / (rows + 2) per value of a, with the
-- expected counts from the posteriors under the learned CPTs.
SELECT count(*) FROM gaps.learn_cpts('lt', 1, 'em', 5000, 1e-12);
WITH post AS (
	SELECT (m.entity ->> 'id')::int AS id, m.probability AS p_s
	FROM gaps.marginals('lt') m WHERE m.attname = 'a' AND m.value = 's'),
w AS (
	SELECT l.b, CASE WHEN l.a = 's' THEN 1 WHEN l.a = 't' THEN 0 ELSE p.p_s END AS ws
	FROM lt l LEFT JOIN post p USING (id)),
cnt AS (
	SELECT sum(ws) FILTER (WHERE b = '1') AS s1, sum(ws) AS s,
		   sum(1 - ws) FILTER (WHERE b = '1') AS t1, sum(1 - ws) AS t
	FROM w)
SELECT abs(f.cpt[1] - (c.s1 + 1) / (c.s + 2)) < 1e-9 AS given_s,
	   abs(f.cpt[3] - (c.t1 + 1) / (c.t + 2)) < 1e-9 AS given_t
FROM cnt c, gaps.factor f WHERE f.name = 'pb';
-- Every factor frozen: nothing to learn, EM stops at once.
SELECT gaps.set_frozen('lt', x, true) FROM unnest(ARRAY['pb', 'pc']) x;
SELECT factor, method, iterations, converged FROM gaps.learn_cpts('lt', 1, 'em')
ORDER BY factor;
SELECT gaps.set_frozen('lt', x, false) FROM unnest(ARRAY['pa', 'pb', 'pc']) x;

-- Rejections.
SELECT gaps.set_frozen('lt', 'nosuch', true);                   -- fail
SELECT gaps.set_frozen('lt', 'pa', NULL);                       -- fail
SELECT gaps.add_factor('lt', 'pd0', 'b');
SELECT gaps.set_frozen('lt', 'pd0', true);                      -- fail: no CPT
DELETE FROM gaps.factor WHERE name = 'pd0';
-- A frozen CPT that rules out observed values: EM's add-one start cannot
-- help, and the error names the frozen factor.
SELECT cpt AS pb_cpt FROM gaps.factor WHERE name = 'pb' \gset
SELECT gaps.set_cpt('lt', 'pb', ARRAY[1, 0, 1, 0]);
SELECT gaps.set_frozen('lt', 'pb', true);
SELECT * FROM gaps.learn_cpts('lt', 0, 'em');                   -- fail
SELECT gaps.set_frozen('lt', 'pb', false);
SELECT gaps.set_cpt('lt', 'pb', :'pb_cpt');

-- Adding a child to a learned model and fitting only it: freeze the rest,
-- add d, learn.  The frozen factors are unchanged bit for bit.
SELECT gaps.learn_cpts('lt', 1, 'em') IS NOT NULL AS relearned;
CREATE TEMP TABLE learned AS SELECT name, cpt FROM gaps.factor
WHERE model_id = gaps._model_id('lt');
SELECT gaps.set_frozen('lt', x, true) FROM unnest(ARRAY['pa', 'pb', 'pc']) x;
SELECT gaps.declare_attribute('lt', 'lt', 'd', ARRAY['n', 'y']);
SELECT gaps.add_stripe('lt', 'd', 'lt', 'd');
SELECT gaps.add_factor('lt', 'pd', 'd', ARRAY['a']);
SELECT factor, complete_entities, method, converged FROM gaps.learn_cpts('lt', 1, 'em')
ORDER BY method, factor;
SELECT bool_and(f.cpt = l.cpt) AS unchanged, count(*)
FROM gaps.factor f JOIN learned l USING (name) WHERE f.model_id = gaps._model_id('lt');
-- d is y whenever a is s, so P(d = y | a = s) is near 1.
SELECT cpt[2] > 0.9 AS learned_d FROM gaps.factor WHERE name = 'pd';

-- Replacing a factor: drop_factor removes the factor only, d and its
-- stripe stay, grounding reports d's values uncovered until a new factor
-- covers them.
SELECT gaps.drop_factor('lt', 'pd');
SELECT count(*) AS d_attribute, (SELECT count(*) FROM gaps.stripe WHERE name = 'd') AS d_stripe
FROM gaps.attribute WHERE attname = 'd';
SELECT count(*) FROM gaps.marginals('lt');                      -- fail: uncovered
SELECT gaps.add_factor('lt', 'pd', 'd', ARRAY['a'], ARRAY[0.5, 0.5, 0.1, 0.9]);
SELECT count(DISTINCT attname) FROM gaps.marginals('lt');
-- Retiring a feature: with cascade, d goes too (nothing else covers it),
-- and the model still answers.
SELECT gaps.drop_factor('lt', 'pd', cascade => true);
SELECT count(*) AS d_attribute FROM gaps.attribute WHERE attname = 'd';
SELECT count(DISTINCT attname) FROM gaps.marginals('lt');
-- With cascade, a factor whose child is another factor's parent cannot go.
SELECT gaps.drop_factor('lt', 'pa', cascade => true);           -- fail
SELECT gaps.drop_factor('lt', 'nosuch');                        -- fail
SELECT gaps.drop_factor('lt', 'pa', NULL);                      -- fail

-- Replacing a factor over part of an attribute: c splits into two stripes.
-- Dropping one stripe's factor leaves its values uncovered, which
-- grounding reports; a new factor over the stripe covers them again.
SELECT gaps.drop_factor('lt', 'pc', cascade => true);
SELECT gaps.declare_attribute('lt', 'lt', 'c', ARRAY['1', '2']);
SELECT gaps.add_stripe('lt', 'c_lo', 'lt', 'c');
SELECT gaps.add_stripe_qual('lt', 'c_lo', 'id', '<=', '300');
SELECT gaps.add_stripe('lt', 'c_hi', 'lt', 'c');
SELECT gaps.add_stripe_qual('lt', 'c_hi', 'id', '>', '300');
SELECT gaps.add_factor('lt', 'pc_lo', 'c_lo', ARRAY['a'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.add_factor('lt', 'pc_hi', 'c_hi', ARRAY['a'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.drop_factor('lt', 'pc_hi');
SELECT count(*) FROM gaps.marginals('lt');                      -- fail: uncovered
SELECT gaps.add_factor('lt', 'pc_hi2', 'c_hi', ARRAY['a'], ARRAY[0.3, 0.7, 0.6, 0.4]);
SELECT count(DISTINCT attname) FROM gaps.marginals('lt');
-- With cascade, a split attribute loses only the dropped factor's
-- stripe.  drop_factor leaves the caller's constraint mode alone: with
-- the foreign key IMMEDIATE before it, deleting a stripe still in use as
-- a parent fails at once after it.
BEGIN;
SET CONSTRAINTS ALL IMMEDIATE;
SELECT gaps.drop_factor('lt', 'pc_hi2', cascade => true);
SELECT array_agg(s.name ORDER BY s.name) AS c_stripes
FROM gaps.stripe s JOIN gaps.attribute a USING (attribute_id) WHERE a.attname = 'c';
DELETE FROM gaps.stripe WHERE name = 'a';                       -- fail
ROLLBACK;

-- A member's factor that a composite's factor uses as a parent cannot go.
CREATE TABLE grp (g int PRIMARY KEY, z text);
INSERT INTO grp VALUES (1, NULL);
ALTER TABLE lt ADD COLUMN g int DEFAULT 1;
SELECT gaps.create_model('top');
SELECT gaps.add_member('top', 'lt');
SELECT gaps.declare_attribute('top', 'grp', 'z', ARRAY['off', 'on']);
SELECT gaps.add_stripe('top', 'z', 'grp', 'z');
SELECT gaps.add_factor('top', 'pz', 'z', ARRAY['lt.b'], kind => 'noisy_or',
					   params => ARRAY[0.1, 0.5], aggregate => ARRAY[true]);
SELECT gaps.map_factor('top', 'pz', 1, 'g', 'g');
SELECT gaps.drop_factor('lt', 'pb', cascade => true);           -- fail: composite
SELECT gaps.drop_factor('top', 'pz');
SELECT gaps.drop_factor('lt', 'pb', cascade => true);
SELECT count(*) AS b_attribute FROM gaps.attribute WHERE attname = 'b';
SELECT gaps.drop_model('top');

-- The same through a composite the dropping role cannot see: the foreign
-- key on the parent stripe refuses it, without naming the hidden factor.
SET client_min_messages = warning;
CREATE ROLE gaps_a;
CREATE ROLE gaps_b;
GRANT gaps_a TO gaps_b;
GRANT SELECT ON lt, grp TO gaps_a, gaps_b;
SET ROLE gaps_a;
SELECT gaps.create_model('mine');
SELECT gaps.declare_attribute('mine', 'lt', 'b', ARRAY['1', '2']);
SELECT gaps.add_stripe('mine', 'b', 'lt', 'b');
SELECT gaps.add_factor('mine', 'pb', 'b', '{}', ARRAY[0.5, 0.5]);
SET ROLE gaps_b;
SELECT gaps.create_model('theirs');
SELECT gaps.add_member('theirs', 'mine');
SELECT gaps.declare_attribute('theirs', 'grp', 'z', ARRAY['off', 'on']);
SELECT gaps.add_stripe('theirs', 'z', 'grp', 'z');
SELECT gaps.add_factor('theirs', 'pz', 'z', ARRAY['mine.b'], kind => 'noisy_or',
					   params => ARRAY[0.1, 0.5], aggregate => ARRAY[true]);
SELECT gaps.map_factor('theirs', 'pz', 1, 'g', 'g');
SET ROLE gaps_a;
SELECT count(*) AS visible_models FROM gaps.model;
\set VERBOSITY sqlstate
SELECT gaps.drop_factor('mine', 'pb', cascade => true);         -- fail: 23503
\set VERBOSITY terse
SELECT count(*) AS still_there FROM gaps.factor WHERE name = 'pb';
SET ROLE gaps_b;
SELECT gaps.drop_model('theirs');
SET ROLE gaps_a;
SELECT gaps.drop_model('mine');
RESET ROLE;
REVOKE SELECT ON lt, grp FROM gaps_a, gaps_b;
DROP ROLE gaps_b;
DROP ROLE gaps_a;

-- Lineage: a parent's name and a note, which change nothing else.  The
-- parent is a name, so dropping it leaves the child alone.
SELECT gaps.create_model('lt2', parent => 'lt', note => 'round 2, refit from lt');
SELECT name, parent, note FROM gaps.model ORDER BY name;
SELECT gaps.set_lineage('lt', 'lt0', 'round 1');
SELECT gaps.set_lineage('lt2', NULL, NULL);
SELECT gaps.set_lineage('nosuch', NULL, NULL);                  -- fail
SELECT gaps.set_lineage('lt2', 'lt', 'round 2');
SELECT gaps.drop_model('lt');
SELECT name, parent, note FROM gaps.model ORDER BY name;
SELECT gaps.drop_model('lt2');
DROP TABLE lt, grp;
