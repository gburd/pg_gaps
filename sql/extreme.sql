-- Parameterised factors with many observed parents, against closed forms.
-- A person's state is a noisy-OR or a logistic of an aggregating slot over
-- the person's e-mails and of one missing parent y.  With the state
-- observed and n e-mails observed active ('llm'), y's posterior does not
-- depend on n, though each e-mail multiplies the probability that the
-- state is off by (1 - p) (noisy-OR) or adds its weight to the score
-- (logistic): with a thousand e-mails that is far below the smallest
-- double.  The posterior must still match the closed form to 1e-12, and
-- so must log P(evidence), which EM climbs (gaps._expected_counts).
CREATE EXTENSION IF NOT EXISTS pg_gaps;
SET client_min_messages = warning;
\set SHOW_CONTEXT never
CREATE TABLE person (pid int PRIMARY KEY, state text, y text);
CREATE TABLE email (eid int PRIMARY KEY, pid int, a text);
INSERT INTO person VALUES (1, 'human', NULL);

-- The model over n active e-mails, rebuilt per case.
CREATE FUNCTION build(n int, kind text, params float8[]) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	TRUNCATE email;
	INSERT INTO email SELECT g, 1, 'llm' FROM generate_series(1, n) g;
	PERFORM gaps.drop_model(name) FROM gaps.model WHERE name = 'x';
	PERFORM gaps.create_model('x');
	PERFORM gaps.declare_attribute('x', 'email', 'a', ARRAY['human', 'llm']);
	PERFORM gaps.add_stripe('x', 'a', 'email', 'a');
	PERFORM gaps.add_factor('x', 'pa', 'a', '{}', ARRAY[0.7, 0.3]);
	PERFORM gaps.declare_attribute('x', 'person', 'state', ARRAY['human', 'llm']);
	PERFORM gaps.declare_attribute('x', 'person', 'y', ARRAY['off', 'on']);
	PERFORM gaps.add_stripe('x', 'state', 'person', 'state');
	PERFORM gaps.add_stripe('x', 'y', 'person', 'y');
	PERFORM gaps.add_factor('x', 'py', 'y', '{}', ARRAY[0.5, 0.5]);
	PERFORM gaps.add_factor('x', 'comb', 'state', ARRAY['a', 'y'], kind => kind,
							params => params, aggregate => ARRAY[true, false]);
	PERFORM gaps.map_factor('x', 'comb', 1, 'pid', 'pid');
	PERFORM gaps.map_factor('x', 'comb', 2, 'pid', 'pid');
END
$$;

-- y's posterior P(on) and log P(evidence) of the model, each as its
-- relative error against want_y and want_ll, and the method.
CREATE FUNCTION errors(want_y float8, want_ll float8,
	OUT y_err float8, OUT ll_err float8, OUT method text)
LANGUAGE plpgsql AS $$
DECLARE
	g record;
BEGIN
	SELECT abs(m.probability - want_y) / want_y, m.method INTO y_err, method
	FROM gaps.marginals('x') m WHERE m.attname = 'y' AND m.value = 'on';
	g := gaps._ground(gaps._model_id('x'));
	SELECT abs(e.loglik - want_ll) / abs(want_ll) INTO ll_err
	FROM gaps._expected_counts(g.card, g.evidence, g.child, g.parent_off, g.parents,
							   g.cpt_off, g.cpts, g.cpt_lscale) e LIMIT 1;
END
$$;

-- Noisy-OR, leak 0.05, p_y 0.3, p per e-mail: P(state off) = 0.95 (1 -
-- p)^n (0.7 if y is on), so P(y on | off) = 0.7 / 1.7, and P(evidence) =
-- 0.3^n x 0.95 (1 - p)^n x 0.5 x 1.7.
SELECT n, p, e.y_err < 1e-12 AS y_exact, e.ll_err < 1e-12 AS loglik_exact, e.method
FROM (VALUES (1000, 0.6), (10000, 0.6), (1000, 0.99), (10000, 0.99)) v(n, p),
	LATERAL (SELECT build(n, 'noisy_or', ARRAY[0.05, p, 0.3])) b,
	LATERAL errors(0.7 / 1.7,
				   n * ln(0.3) + ln(0.95) + n * ln(1 - p) + ln(0.5) + ln(1.7)) e
ORDER BY n, p;

-- Logistic, bias -1, w(llm) = w per e-mail, w(y on) 0.5: the score of
-- 'llm' is s = -1 + n w (+ 0.5 if y is on), P(human | s) = 1 / (1 +
-- exp(s)).  With s = 1e4, P(human) is about exp(-1e4); the posterior of y
-- is the ratio of the two, (1 + exp(s)) / (1 + exp(s + 0.5)) =
-- exp(-0.5) to within exp(-s), and the closed forms below are written so
-- that they do not overflow.
SELECT n, w, e.y_err < 1e-12 AS y_exact, e.ll_err < 1e-12 AS loglik_exact, e.method
FROM (VALUES (1000, 10.0::float8), (10000, 1.0::float8)) v(n, w),
	LATERAL (SELECT build(n, 'logistic', ARRAY[-1, 0, w, 0, 0.5])) b,
	LATERAL (SELECT -1 + n * w AS s) sc,
	LATERAL errors(exp(-0.5) / (1 + exp(-0.5)),
				   n * ln(0.3) + ln(0.5) - sc.s + ln(1 + exp(-0.5))) e
ORDER BY n;

-- An entry whose exact value is positive but not a normal double is an
-- ERROR naming the factor, never a silent zero: a noisy-OR with leak
-- 1e-320 (a subnormal) gives the unlabelled person's state P(on) = 1e-320
-- when its one e-mail is inactive.
TRUNCATE email;
INSERT INTO email VALUES (1, 1, 'human');
UPDATE person SET state = NULL;
SELECT build(0, 'noisy_or', ARRAY[1e-320, 0.5, 0.3]);
INSERT INTO email VALUES (1, 1, 'human');
SELECT count(*) FROM gaps.marginals('x');                            -- fail: not representable

-- A factor's instances together are bounded before anything is built:
-- 30 people with 200 missing binary e-mails each need about 162M cells in
-- all, though each person's 5.4M pass the per-instance bounds.
TRUNCATE person;
INSERT INTO person SELECT g, NULL, 'off' FROM generate_series(1, 30) g;
SELECT build(0, 'logistic', ARRAY[-1, 0, 0.01, 0, 0.5]);
INSERT INTO email SELECT g, 1 + g % 30, NULL FROM generate_series(1, 6000) g;
SELECT count(*) FROM gaps.marginals('x');                            -- fail: too big
-- One instance is held to an estimate of what solving it holds, so one
-- the engine could not solve fails here, naming the factor, and not after
-- its tables are built.  A person with 50 missing e-mails: about 450k
-- cells estimated, 360k needed.
DELETE FROM person WHERE pid > 1;
TRUNCATE email;
INSERT INTO email SELECT g, 1, NULL FROM generate_series(1, 50) g;
SET gaps.max_total_cells = 300000;
SELECT count(*) FROM gaps.marginals('x');                            -- fail: too big
SET gaps.max_total_cells = 600000;
-- The closed form: c of the 50 are 'llm' with probability Binomial(50,
-- 0.3), and P(llm | c) = 1 / (1 + exp(1 - 0.01 c)).
SELECT abs(m.probability - (
		SELECT sum(exp(lgamma_c) * 0.3 ^ c * 0.7 ^ (50 - c) / (1 + exp(1 - 0.01 * c)))::float8
		FROM (SELECT c, coalesce((SELECT sum(ln(50 - i + 1) - ln(i))
								  FROM generate_series(1, c) i), 0) AS lgamma_c
			  FROM generate_series(0, 50) c) b)) < 1e-12 AS p_llm_exact
FROM gaps.marginals('x') m WHERE m.attname = 'state' AND m.value = 'llm';
RESET gaps.max_total_cells;
-- 300 missing members, which pass the per-table bound but are far more
-- than the engine could solve, fail up front.
INSERT INTO email SELECT g, 1, NULL FROM generate_series(51, 300) g;
SELECT count(*) FROM gaps.marginals('x');                            -- fail: too big

-- The SQL-callable step refuses one variable twice in an aggregating slot
-- (it would be two independent parents with one value).
SELECT * FROM gaps._param_cpts('f', 'noisy_or', 2, ARRAY[2], ARRAY[true], ARRAY[0.1, 0.5],
							   10, ARRAY[1], ARRAY[1, 3], ARRAY[2, 2], ARRAY[0, 0]);   -- fail

SELECT gaps.drop_model('x');
DROP TABLE person, email;
DROP FUNCTION build(int, text, float8[]), errors(float8, float8);
