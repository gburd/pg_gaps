-- Parameterised factors (doc/DESIGN.md, "Parameterised factors"): a
-- logistic or noisy-OR factor's tables are computed from its params at
-- grounding, over each instance's missing parents only.  The oracle is the
-- same model with the factor declared as a dense CPT, its entries computed
-- here from the same formula by code that shares nothing with the
-- extension's.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- The formula, written out: P(child = k) (k 1-based) given one value per
-- parent (vals, 1-based), for a child of kc values, parents with domain
-- sizes pc.  Logistic: class 1 scores 0, class k > 1 scores its bias plus
-- one weight per parent value; softmax.  Noisy-OR: P(off) = (1 - leak)
-- times (1 - p_i) for each parent not at its first value.
CREATE FUNCTION formula(kind text, params float8[], kc int, pc int[], vals int[], k int)
RETURNS float8 LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	w int := 1 + coalesce((SELECT sum(x) FROM unnest(pc) x), 0);
	sc float8[];
	q float8;
	b int;
BEGIN
	IF kind = 'noisy_or' THEN
		q := 1 - params[1];
		FOR i IN 1 .. coalesce(cardinality(vals), 0) LOOP
			IF vals[i] > 1 THEN
				q := q * (1 - params[1 + i]);
			END IF;
		END LOOP;
		RETURN CASE WHEN k = 1 THEN q ELSE 1 - q END;
	END IF;
	sc := ARRAY[0::float8];
	FOR c IN 2 .. kc LOOP
		sc := sc || params[(c - 2) * w + 1];
		b := 1;
		FOR i IN 1 .. coalesce(cardinality(vals), 0) LOOP
			sc[c] := sc[c] + params[(c - 2) * w + 1 + b + vals[i] - 1];
			b := b + pc[i];
		END LOOP;
	END LOOP;
	RETURN exp(sc[k]) / (SELECT sum(exp(x)) FROM unnest(sc) x);
END
$$;

-- The dense CPT of a parameterised factor, in the CPT layout (parents
-- then child, the last parent and then the child fastest).
CREATE FUNCTION dense(kind text, params float8[], kc int, pc int[]) RETURNS float8[]
LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	n int := coalesce((SELECT exp(sum(ln(x)))::int FROM unnest(pc) x), 1);
	out float8[] := '{}';
	vals int[];
	r int;
BEGIN
	FOR row IN 0 .. n - 1 LOOP
		vals := '{}';
		r := row;
		FOR i IN REVERSE coalesce(cardinality(pc), 0) .. 1 LOOP
			vals := (r % pc[i] + 1) || vals;
			r := r / pc[i];
		END LOOP;
		FOR k IN 1 .. kc LOOP
			out := out || formula(kind, params, kc, pc, vals, k);
		END LOOP;
	END LOOP;
	RETURN out;
END
$$;

-- A random model over table rt (built by build()): attributes a1 .. a4
-- with priors, child c with parents a random subset of them, as a dense
-- CPT ('d') or as a parameterised factor ('p').  The seed drives
-- setseed(); the same seed gives the same model twice.
CREATE TABLE rt (id int PRIMARY KEY, a1 text, a2 text, a3 text, a4 text, c text);
CREATE FUNCTION build(seed float8, form text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	kind text;
	kc int;
	np int;
	pc int[] := '{}';
	ps text[] := '{}';
	params float8[] := '{}';
	card int[];
	pr float8[];
	m text := 'r' || form;
BEGIN
	PERFORM setseed(seed);
	kind := CASE WHEN random() < 0.5 THEN 'logistic' ELSE 'noisy_or' END;
	kc := CASE WHEN kind = 'noisy_or' THEN 2 ELSE 2 + floor(random() * 2)::int END;
	card := ARRAY[2 + floor(random() * 3)::int, 2 + floor(random() * 3)::int,
				  2 + floor(random() * 3)::int, 2 + floor(random() * 3)::int];
	TRUNCATE rt;
	INSERT INTO rt
	SELECT i, CASE WHEN random() < 0.5 THEN NULL ELSE 'v' || floor(random() * card[1]) END,
		   CASE WHEN random() < 0.5 THEN NULL ELSE 'v' || floor(random() * card[2]) END,
		   CASE WHEN random() < 0.5 THEN NULL ELSE 'v' || floor(random() * card[3]) END,
		   CASE WHEN random() < 0.5 THEN NULL ELSE 'v' || floor(random() * card[4]) END,
		   CASE WHEN random() < 0.5 THEN NULL ELSE 'v' || floor(random() * kc) END
	FROM generate_series(1, 6) i;
	PERFORM gaps.create_model(m);
	FOR i IN 1 .. 4 LOOP
		PERFORM gaps.declare_attribute(m, 'rt', ('a' || i)::name,
			ARRAY(SELECT 'v' || x FROM generate_series(0, card[i] - 1) x));
		PERFORM gaps.add_stripe(m, 's' || i, 'rt', ('a' || i)::name);
		pr := ARRAY(SELECT 0.1 + random() FROM generate_series(1, card[i]));
		PERFORM gaps.add_factor(m, 'f' || i, 's' || i, '{}',
			ARRAY(SELECT x / (SELECT sum(y) FROM unnest(pr) y) FROM unnest(pr) x));
		IF random() < 0.6 THEN
			ps := ps || ('s' || i);
			pc := pc || card[i];
		END IF;
	END LOOP;
	PERFORM gaps.declare_attribute(m, 'rt', 'c',
		ARRAY(SELECT 'v' || x FROM generate_series(0, kc - 1) x));
	PERFORM gaps.add_stripe(m, 'sc', 'rt', 'c');
	np := cardinality(ps);
	IF kind = 'noisy_or' THEN
		params := ARRAY(SELECT random() FROM generate_series(0, np));
	ELSE
		params := ARRAY(SELECT random() * 6 - 3
						FROM generate_series(1, (kc - 1) * (1 + coalesce((SELECT sum(x)
							FROM unnest(pc) x), 0))::int));
	END IF;
	IF form = 'p' THEN
		PERFORM gaps.add_factor(m, 'fc', 'sc', ps, kind => kind, params => params);
	ELSE
		PERFORM gaps.add_factor(m, 'fc', 'sc', ps, dense(kind, params, kc, pc));
	END IF;
	RETURN kind;
END
$$;

-- 200 random models, each solved both ways.  Every marginal must agree to
-- 1e-12, every distribution must sum to 1 with no negative entry, and
-- there must be marginals to compare.
CREATE TABLE cmp (seed int, kind text, attname name, entity jsonb, value text,
				  pd float8, pp float8);
DO $$
DECLARE
	kind text;
BEGIN
	FOR s IN 1 .. 200 LOOP
		kind := build(s / 1000.0, 'd');
		CREATE TEMP TABLE md AS SELECT * FROM gaps.marginals('rd');
		PERFORM build(s / 1000.0, 'p');
		INSERT INTO cmp
		SELECT s, kind, d.attname, d.entity, d.value, d.probability, p.probability
		FROM md d FULL JOIN gaps.marginals('rp') p USING (attname, entity, value);
		DROP TABLE md;
		PERFORM gaps.drop_model('rd');
		PERFORM gaps.drop_model('rp');
	END LOOP;
END
$$;
SELECT kind, count(DISTINCT seed) AS models, count(*) > 1000 AS many_marginals,
	   count(*) FILTER (WHERE pd IS NULL OR pp IS NULL) AS unmatched,
	   count(*) FILTER (WHERE abs(pd - pp) > 1e-12) AS disagree
FROM cmp GROUP BY kind ORDER BY kind;
SELECT count(*) AS bad_distributions FROM (
	SELECT seed, attname, entity FROM cmp GROUP BY seed, attname, entity
	HAVING abs(sum(pp) - 1) > 1e-12 OR min(pp) < 0) x;
SELECT count(DISTINCT seed) FILTER (WHERE attname = 'c') AS models_with_missing_child
FROM cmp;

-- pg.ddx.io's shape: a binary latent child with 15 observed five-valued
-- inputs.  As a CPT it would have 5^15 rows; each instance's table here
-- is one row, and inference is exact.
CREATE TABLE em15 (id int PRIMARY KEY, x1 int, x2 int, x3 int, x4 int, x5 int, x6 int,
				   x7 int, x8 int, x9 int, x10 int, x11 int, x12 int, x13 int, x14 int,
				   x15 int, llm text);
INSERT INTO em15
SELECT i, i % 5, (i * 3) % 5, (i * 7) % 5, i % 5, (i + 1) % 5, (i + 2) % 5, (i + 3) % 5,
	   (i + 4) % 5, (i * 2) % 5, i % 5, (i * 4) % 5, (i + 1) % 5, i % 5, (i * 3) % 5, i % 5,
	   CASE WHEN i <= 2 THEN 'no' END
FROM generate_series(1, 6) i;
SELECT gaps.create_model('em15');
SELECT count(*) AS inputs FROM (
	SELECT gaps.declare_attribute('em15', 'em15', ('x' || i)::name, ARRAY['0', '1', '2', '3', '4']),
		   gaps.add_stripe('em15', 's' || i, 'em15', ('x' || i)::name),
		   gaps.add_factor('em15', 'p' || i, 's' || i, '{}', ARRAY[0.2, 0.2, 0.2, 0.2, 0.2])
	FROM generate_series(1, 15) i) x;
SELECT gaps.declare_attribute('em15', 'em15', 'llm', ARRAY['no', 'yes']);
SELECT gaps.add_stripe('em15', 'llm', 'em15', 'llm');
-- Weights: a bias of -1, then w(input i, value v) = (v - 2) * i / 30.
SELECT gaps.add_factor('em15', 'llm', 'llm',
	ARRAY(SELECT 's' || i FROM generate_series(1, 15) i),
	kind => 'logistic',
	params => ARRAY[-1.0::float8] || ARRAY(SELECT (v - 2) * i / 30.0
										   FROM generate_series(1, 15) i,
												generate_series(0, 4) v ORDER BY i, v));
SELECT cardinality(g.cpts) AS cells FROM gaps._ground(gaps._model_id('em15')) g;
WITH hand AS (
	SELECT e.id, 1 / (1 + exp(-(-1 + (SELECT sum((v - 2) * i / 30.0)
									  FROM unnest(ARRAY[e.x1, e.x2, e.x3, e.x4, e.x5, e.x6,
														e.x7, e.x8, e.x9, e.x10, e.x11, e.x12,
														e.x13, e.x14, e.x15])
										   WITH ORDINALITY u(v, i))))) AS p
	FROM em15 e WHERE e.llm IS NULL)
SELECT h.id, round(m.probability::numeric, 9) AS p_yes,
	   abs(m.probability - h.p) < 1e-12 AS agrees
FROM hand h JOIN gaps.marginals('em15') m ON m.entity = jsonb_build_object('id', h.id)
WHERE m.value = 'yes' ORDER BY h.id;
-- With every input missing the same factor is 5^15 x 2 cells.
UPDATE em15 SET x1 = NULL, x2 = NULL, x3 = NULL, x4 = NULL, x5 = NULL, x6 = NULL, x7 = NULL,
	x8 = NULL, x9 = NULL, x10 = NULL, x11 = NULL, x12 = NULL, x13 = NULL, x14 = NULL,
	x15 = NULL WHERE id = 3;
SELECT count(*) FROM gaps.marginals('em15');                        -- fail: too big
SELECT gaps.drop_model('em15');
DROP TABLE em15;

-- Validation at the trust boundary.
CREATE TABLE v (id int PRIMARY KEY, a text, b text, c text);
SELECT gaps.create_model('v');
SELECT gaps.declare_attribute('v', 'v', 'a', ARRAY['x', 'y', 'z']);
SELECT gaps.declare_attribute('v', 'v', 'b', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('v', 'v', 'c', ARRAY['off', 'on', 'maybe']);
SELECT gaps.add_stripe('v', 'a', 'v', 'a');
SELECT gaps.add_stripe('v', 'b', 'v', 'b');
SELECT gaps.add_stripe('v', 'c', 'v', 'c');
SELECT gaps.add_factor('v', 'f1', 'b', ARRAY['a'], kind => 'noisy_or',
	params => ARRAY[0.1]);                                           -- fail: length
SELECT gaps.add_factor('v', 'f1', 'b', ARRAY['a'], kind => 'noisy_or',
	params => ARRAY[0.1, 1.5]);                                      -- fail: not a probability
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'noisy_or',
	params => ARRAY[0.1, 0.5]);                                      -- fail: child not binary
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'logistic',
	params => ARRAY[1, 2, 3, 4, 5, 6, 7]);                           -- fail: length (needs 8)
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'logistic',
	params => ARRAY[1, 2, 3, 'Infinity', 5, 6, 7, 8]::float8[]);     -- fail: infinite
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'logistic',
	params => '[0:7]={1,2,3,4,5,6,7,8}');                            -- fail: lower bound
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'logistic');  -- fail: no params
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], ARRAY[1, 0, 0, 1, 0, 0, 1, 0, 0],
	kind => 'logistic', params => ARRAY[1, 2, 3, 4, 5, 6, 7, 8]);    -- fail: and a CPT
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'table');     -- fail: kind
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], params => ARRAY[1]);  -- fail: cpt with params
SELECT gaps.add_factor('v', 'f1', 'c', ARRAY['a'], kind => 'logistic',
	params => ARRAY[1, 2, 3, 4, 5, 6, 7, 8]);
SELECT gaps.set_cpt('v', 'f1', ARRAY[1, 0, 0, 1, 0, 0, 1, 0, 0]);    -- fail: CPT on a logistic
SELECT gaps.set_prior('v', 'f1', ARRAY[1, 1, 1, 1, 1, 1, 1, 1, 1]);  -- fail: prior on it
UPDATE gaps.factor SET params = ARRAY[0.1, 'NaN'] WHERE name = 'f1'; -- fail: NaN, direct DML
SELECT name, kind, params FROM gaps.factor WHERE name = 'f1';
SELECT gaps.drop_model('v');
DROP TABLE v;

-- learn_cpts leaves a parameterised factor as it is, says so, and EM
-- learns the other factors with it fixed.  x -> y is a CPT to learn and y
-- -> z a noisy-OR.  With P(z = on | y) known, the rows with z observed and
-- y missing carry information about P(y | x) that only EM uses.
CREATE TABLE lx (id int PRIMARY KEY, x text, y text, z text);
INSERT INTO lx SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END,
	CASE WHEN i <= 20 THEN CASE WHEN i % 3 = 0 THEN 'on' ELSE 'off' END END,
	CASE WHEN i > 20 THEN CASE WHEN i % 4 = 0 THEN 'off' ELSE 'on' END END
FROM generate_series(1, 60) i;
SELECT gaps.create_model('lx');
SELECT gaps.declare_attribute('lx', 'lx', 'x', ARRAY['a', 'b']);
SELECT gaps.declare_attribute('lx', 'lx', 'y', ARRAY['off', 'on']);
SELECT gaps.declare_attribute('lx', 'lx', 'z', ARRAY['off', 'on']);
SELECT gaps.add_stripe('lx', 'x', 'lx', 'x');
SELECT gaps.add_stripe('lx', 'y', 'lx', 'y');
SELECT gaps.add_stripe('lx', 'z', 'lx', 'z');
SELECT gaps.add_factor('lx', 'px', 'x');
SELECT gaps.add_factor('lx', 'py', 'y', ARRAY['x']);
SELECT gaps.add_factor('lx', 'pz', 'z', ARRAY['y'], kind => 'noisy_or',
	params => ARRAY[0.2, 0.9]);
SELECT * FROM gaps.learn_cpts('lx');
SELECT factor, complete_entities, method, iterations > 1 AS iterated, converged
FROM gaps.learn_cpts('lx', method => 'em', tol => 1e-12, max_iterations => 500)
ORDER BY factor;
SELECT name, kind, cpt IS NULL AS no_cpt, params FROM gaps.factor
WHERE name = 'pz';
-- The oracle: at EM's fixed point with pz held, P(y | x) equals the M-step
-- of its own E-step, (expected count of y = on, given the evidence under
-- the learned CPTs, + alpha 1) / (rows + 2) per value of x.
WITH post AS (
	SELECT (m.entity ->> 'id')::int AS id, m.probability AS p_on
	FROM gaps.marginals('lx') m WHERE m.attname = 'y' AND m.value = 'on'),
cnt AS (
	SELECT l.x,
		   sum(CASE WHEN l.y = 'on' THEN 1 WHEN l.y = 'off' THEN 0 ELSE p.p_on END) AS n_on,
		   count(*) AS n
	FROM lx l LEFT JOIN post p USING (id) GROUP BY l.x)
SELECT c.x, abs(f.cpt[CASE c.x WHEN 'a' THEN 2 ELSE 4 END] - (c.n_on + 1) / (c.n + 2)) < 1e-6
		AS m_step_fixed_point
FROM cnt c, gaps.factor f WHERE f.name = 'py' ORDER BY c.x;
SELECT gaps.drop_model('lx');
DROP TABLE lx;

-- cpt_rows computes a parameterised factor's table over few parents, and
-- refuses it over many, naming the size.
CREATE TABLE cr (id int PRIMARY KEY, a text, b text, c text);
SELECT gaps.create_model('cr');
SELECT gaps.declare_attribute('cr', 'cr', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('cr', 'cr', 'b', ARRAY['x', 'y', 'z']);
SELECT gaps.declare_attribute('cr', 'cr', 'c', ARRAY['off', 'on']);
SELECT gaps.add_stripe('cr', 'a', 'cr', 'a');
SELECT gaps.add_stripe('cr', 'b', 'cr', 'b');
SELECT gaps.add_stripe('cr', 'c', 'cr', 'c');
SELECT gaps.add_factor('cr', 'nor', 'c', ARRAY['a', 'b'], kind => 'noisy_or',
	params => ARRAY[0.1, 0.5, 0.25]);
SELECT parents, value, round(probability::numeric, 6) AS p, prior FROM gaps.cpt_rows('cr', 'nor');
SELECT bool_and(abs(r.probability - formula('noisy_or', ARRAY[0.1, 0.5, 0.25], 2, ARRAY[2, 3],
			ARRAY[(o - 1) / 6 + 1, ((o - 1) / 2) % 3 + 1]::int[], ((o - 1) % 2)::int + 1)) < 1e-15)
		AS formula_agrees
FROM gaps.cpt_rows('cr', 'nor') WITH ORDINALITY r(parents, value, probability, prior, o);
SET gaps.max_factor_cells = 8;
SELECT count(*) FROM gaps.cpt_rows('cr', 'nor');                    -- fail: 12 cells
RESET gaps.max_factor_cells;
SELECT gaps.drop_model('cr');
DROP TABLE cr;

DROP TABLE rt, cmp;
DROP FUNCTION build(float8, text), dense(text, float8[], int, int[]),
	formula(text, float8[], int, int[], int[], int);
