-- bench/em_convergence.sql: how far EM's stopping rule leaves the CPTs from
-- EM's fixed point, on pg.ddx.io's shape (a 2-state latent A with three
-- 3-state children, n rows drawn from planted CPTs, A observed in the first
-- lab).
--
-- Usage: psql -X -v n=20000 -v lab=300 -v seed=0.42 -f bench/em_convergence.sql
--
-- Prints an md5 of the gaps function bodies (which code ran), then takes
-- the fixed point as a run to tol 1e-15, and for the default and two
-- other tols prints iterations, last_change and the largest distance of
-- any CPT cell from the fixed point.  \timing inside one session (rule 4).
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS pg_gaps CASCADE;
CREATE EXTENSION pg_gaps;
SELECT md5(string_agg(p.prosrc, '' ORDER BY p.proname, p.oid)) AS code
FROM pg_proc p WHERE p.pronamespace = 'gaps'::regnamespace;
DROP TABLE IF EXISTS latent;
SELECT setseed(:seed);
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

SELECT DISTINCT iterations, loglik, last_change FROM gaps.learn_cpts('latent', 1, 'em', 5000, 1e-15);
CREATE TEMP TABLE fp AS SELECT name, cpt FROM gaps.factor
WHERE model_id = gaps._model_id('latent');
CREATE VIEW pg_temp.dist AS
SELECT max(abs(x - y)) AS dist FROM gaps.factor f JOIN fp USING (name), unnest(f.cpt, fp.cpt) u(x, y)
WHERE f.model_id = gaps._model_id('latent');
\timing on
SELECT DISTINCT iterations, converged, last_change FROM gaps.learn_cpts('latent', 1, 'em');
SELECT DISTINCT iterations, converged, last_change FROM gaps.learn_cpts('latent', 1, 'em');
\timing off
SELECT * FROM pg_temp.dist;
SELECT DISTINCT iterations, converged, last_change FROM gaps.learn_cpts('latent', 1, 'em', tol => 1e-4);
SELECT * FROM pg_temp.dist;
SELECT DISTINCT iterations, converged, last_change FROM gaps.learn_cpts('latent', 1, 'em', tol => 1e-8);
SELECT * FROM pg_temp.dist;
