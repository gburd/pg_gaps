-- bench/learn.sql: gaps.learn_cpts at scale, on two models.
--
-- chain: a -> b over n rows of one table, a third of a missing (the
-- reviewer's model for the counting query's cost).  latent: pg.ddx.io's
-- shape, a 2-state latent A with three 3-state children, n rows of which
-- the first lab have A observed, drawn from planted CPTs.
--
-- Usage: psql -X -v n=10200 -v lab=200 -v chain=1 -v latent=1 -v em=1 \
--			-f bench/learn.sql
--
-- Prints an md5 of the gaps function bodies (CREATE EXTENSION copies them
-- into the catalog, so this says which code ran), the learned CPTs, and
-- the time of each learn_cpts call, twice per model in one session (rule
-- 4).  chain=1 and latent=1 time method 'counts' on each model; em=1
-- times method 'em' on the latent model.
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS pg_gaps CASCADE;
CREATE EXTENSION pg_gaps;
SELECT md5(string_agg(p.prosrc, '' ORDER BY p.proname, p.oid)) AS code
FROM pg_proc p WHERE p.pronamespace = 'gaps'::regnamespace;
DROP TABLE IF EXISTS chain, latent;

CREATE TABLE chain (id int PRIMARY KEY, a text, b text);
INSERT INTO chain
SELECT g, CASE WHEN g % 3 = 0 THEN NULL WHEN g % 2 = 0 THEN 'x' ELSE 'y' END,
	   CASE WHEN g % 5 = 0 THEN 'u' ELSE 'v' END
FROM generate_series(1, :n) g;
ANALYZE chain;
SELECT gaps.create_model('chain');
SELECT gaps.declare_attribute('chain', 'chain', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('chain', 'chain', 'b', ARRAY['u', 'v']);
SELECT gaps.add_stripe('chain', 'a', 'chain', 'a');
SELECT gaps.add_stripe('chain', 'b', 'chain', 'b');
SELECT gaps.add_factor('chain', 'pa', 'a');
SELECT gaps.add_factor('chain', 'pb', 'b', ARRAY['a']);

-- The planted CPTs: P(A) = (0.3, 0.7), and P(child | A) per child.
SELECT setseed(0.42);
CREATE TEMP TABLE planted (f text, cpt float8[]);
INSERT INTO planted VALUES ('pa', '{0.3,0.7}'),
	('pb', '{0.7,0.2,0.1, 0.1,0.3,0.6}'),
	('pc', '{0.2,0.6,0.2, 0.5,0.1,0.4}'),
	('pd', '{0.1,0.1,0.8, 0.6,0.3,0.1}');
CREATE FUNCTION pg_temp.draw(p float8[], off int, u float8) RETURNS int
LANGUAGE sql AS $$
	SELECT CASE WHEN u < p[off + 1] THEN 1 WHEN u < p[off + 1] + p[off + 2] THEN 2 ELSE 3 END
$$;
CREATE TABLE latent (id int PRIMARY KEY, a text, b text, c text, d text);
INSERT INTO latent
SELECT g, CASE WHEN g <= :lab THEN (ARRAY['s', 't'])[a] END,
	   (ARRAY['1', '2', '3'])[pg_temp.draw((SELECT cpt FROM planted WHERE f = 'pb'),
										   (a - 1) * 3, random())],
	   (ARRAY['1', '2', '3'])[pg_temp.draw((SELECT cpt FROM planted WHERE f = 'pc'),
										   (a - 1) * 3, random())],
	   (ARRAY['1', '2', '3'])[pg_temp.draw((SELECT cpt FROM planted WHERE f = 'pd'),
										   (a - 1) * 3, random())]
FROM (SELECT g, CASE WHEN random() < 0.3 THEN 1 ELSE 2 END AS a
	  FROM generate_series(1, :n) g) x;
ANALYZE latent;
SELECT gaps.create_model('latent');
SELECT gaps.declare_attribute('latent', 'latent', 'a', ARRAY['s', 't']);
SELECT gaps.declare_attribute('latent', 'latent', x, ARRAY['1', '2', '3'])
FROM unnest(ARRAY['b', 'c', 'd']) x;
SELECT gaps.add_stripe('latent', x, 'latent', x) FROM unnest(ARRAY['a', 'b', 'c', 'd']) x;
SELECT gaps.add_factor('latent', 'pa', 'a');
SELECT gaps.add_factor('latent', 'p' || x, x, ARRAY['a']) FROM unnest(ARRAY['b', 'c', 'd']) x;

\if :chain
\timing on
SELECT factor, complete_entities FROM gaps.learn_cpts('chain', 1);
SELECT factor, complete_entities FROM gaps.learn_cpts('chain', 1);
\timing off
\endif
\if :latent
\timing on
SELECT factor, complete_entities FROM gaps.learn_cpts('latent', 1);
SELECT factor, complete_entities FROM gaps.learn_cpts('latent', 1);
\timing off
\endif
SELECT m.name AS model, f.name, f.cpt
FROM gaps.factor f JOIN gaps.model m USING (model_id) ORDER BY 1, 2;

\if :em
\timing on
SELECT factor, method, iterations, loglik, converged FROM gaps.learn_cpts('latent', 1, 'em');
SELECT factor, method, iterations, loglik, converged FROM gaps.learn_cpts('latent', 1, 'em');
\timing off
SELECT f.name, max(abs(x - y)) AS planted_err
FROM gaps.factor f JOIN planted p ON p.f = f.name, unnest(f.cpt, p.cpt) u(x, y)
WHERE f.model_id = gaps._model_id('latent') GROUP BY 1 ORDER BY 1;
\endif
