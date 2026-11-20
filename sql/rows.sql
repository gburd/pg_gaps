-- Row estimates of the set-returning functions (doc/DESIGN.md, "Row
-- estimates").  With a constant model name the support function estimates
-- from reltuples, each attribute's null fraction and its domain size;
-- otherwise the planner uses the declared ROWS (1000 for marginals and
-- impute, 10 for learn_cpts).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- The planner's row estimate for a query, from EXPLAIN (FORMAT JSON), so
-- the test does not depend on cost numbers that differ across versions.
CREATE FUNCTION plan_rows(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE
	j json;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (j -> 0 -> 'Plan' ->> 'Plan Rows')::float8;
END
$$;

-- 1000 rows; a is missing in 250 of them (domain of 2), b in 100 (domain
-- of 3).  ANALYZE samples every row of a table this small, so reltuples and
-- the null fractions are exact, and the estimate equals the real count:
-- marginals 250 * 2 + 100 * 3 = 800 rows, impute 250 + 100 = 350.
CREATE TABLE t (id int PRIMARY KEY, a text, b text);
INSERT INTO t
SELECT i, CASE WHEN i % 4 = 0 THEN NULL WHEN i % 3 = 0 THEN 'x' ELSE 'y' END,
	   CASE WHEN i % 10 = 0 THEN NULL ELSE (ARRAY['p', 'q', 'r'])[i % 3 + 1] END
FROM generate_series(1, 1000) i;
ANALYZE t;

SELECT gaps.create_model('rm');
SELECT gaps.declare_attribute('rm', 't', 'a', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('rm', 't', 'b', ARRAY['p', 'q', 'r']);
SELECT gaps.add_stripe('rm', 'sa', 't', 'a');
SELECT gaps.add_stripe('rm', 'sb', 't', 'b');
SELECT gaps.add_factor('rm', 'pa', 'sa');
SELECT gaps.add_factor('rm', 'pb', 'sb', ARRAY['sa']);
SELECT factor, complete_entities FROM gaps.learn_cpts('rm') ORDER BY factor;

-- The estimates, and the actual counts beside them.
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS marginals_est,
	   (SELECT count(*) FROM gaps.marginals('rm')) AS marginals_actual,
	   plan_rows($$SELECT * FROM gaps.impute('rm')$$) AS impute_est,
	   (SELECT count(*) FROM gaps.impute('rm')) AS impute_actual,
	   plan_rows($$SELECT * FROM gaps.learn_cpts('rm')$$) AS learn_est;

-- A parameter: a custom plan sees the bound value, a generic plan does not
-- and falls back to ROWS.
PREPARE pm(text) AS SELECT * FROM gaps.marginals($1);
SET plan_cache_mode = force_custom_plan;
SELECT plan_rows($$EXECUTE pm('rm')$$) AS custom;
SET plan_cache_mode = force_generic_plan;
SELECT plan_rows($$EXECUTE pm('rm')$$) AS generic;
RESET plan_cache_mode;
DEALLOCATE pm;

-- No such model, or a NULL name: no error at plan time, the default
-- estimate.  Executing it fails as it always did.
SELECT plan_rows($$SELECT * FROM gaps.marginals('nosuch')$$) AS nosuch,
	   plan_rows($$SELECT * FROM gaps.marginals(NULL)$$) AS null_name;
SELECT * FROM gaps.marginals('nosuch');                      -- fail

-- A role that is not a member of the model's owner cannot see the model,
-- and gets the default, not the model's size.
CREATE ROLE gaps_rows_other;
GRANT SELECT ON t TO gaps_rows_other;
GRANT EXECUTE ON FUNCTION plan_rows(text) TO gaps_rows_other;
SET ROLE gaps_rows_other;
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS hidden_model;
RESET ROLE;

-- A cached plan is not reused across roles: the estimate is replanned.
PREPARE pq AS SELECT * FROM gaps.marginals('rm');
SELECT plan_rows('EXECUTE pq') AS owner_plan;
SET ROLE gaps_rows_other;
SELECT plan_rows('EXECUTE pq') AS other_plan;
RESET ROLE;
DEALLOCATE pq;

-- A member of the owner role who may not read the table's columns may not
-- read their statistics either: the default.  With SELECT on one column
-- only, the other column's statistics stay hidden, so still the default.
UPDATE gaps.model SET owner = 'gaps_rows_other' WHERE name = 'rm';
GRANT gaps_rows_other TO current_user;
REVOKE SELECT ON t FROM gaps_rows_other;
GRANT SELECT (id, a) ON t TO gaps_rows_other;
SET ROLE gaps_rows_other;
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS one_column;
RESET ROLE;
GRANT SELECT ON t TO gaps_rows_other;
SET ROLE gaps_rows_other;
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS readable;
RESET ROLE;

-- Row-level security on the table hides its statistics from the role, as
-- pg_stats does.
ALTER TABLE t ENABLE ROW LEVEL SECURITY;
CREATE POLICY few ON t TO gaps_rows_other USING (id < 10);
SET ROLE gaps_rows_other;
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS rls_table;
RESET ROLE;
ALTER TABLE t DISABLE ROW LEVEL SECURITY;

-- An error inside the estimate (here, no SELECT on the catalog) is not
-- an error of the query being planned.
REVOKE SELECT ON gaps.attribute FROM PUBLIC;
SET ROLE gaps_rows_other;
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS catalog_error;
RESET ROLE;
GRANT SELECT ON gaps.attribute TO PUBLIC;

-- Under SERIALIZABLE there is no estimate, so that planning takes no
-- predicate locks on the catalog.
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS serializable;
COMMIT;

-- A cached plan depends on the data table: ANALYZE replans it, and the
-- estimate follows the new null fraction (a missing in 625 rows now, so
-- 625 * 2 + 100 * 3 = 1550).  Ordinary inserts into the catalog send no
-- invalidation, so declaring another attribute (c, missing in all 1000
-- rows) leaves the cached estimate as it was until something replans it;
-- a fresh plan sees 1550 + 1000.
ALTER TABLE t ADD COLUMN c text;
PREPARE pq AS SELECT * FROM gaps.marginals('rm');
SELECT plan_rows('EXECUTE pq') AS before_analyze;
UPDATE t SET a = NULL WHERE id <= 500;
ANALYZE t;
SELECT plan_rows('EXECUTE pq') AS after_analyze;
SELECT gaps.declare_attribute('rm', 't', 'c', ARRAY['1']);
SELECT plan_rows('EXECUTE pq') AS cached_after_model_edit,
	   plan_rows($$SELECT * FROM gaps.marginals('rm')$$) AS fresh;
DEALLOCATE pq;

-- A table with inheritance children: grounding reads the children, which
-- the parent's reltuples does not count, so no estimate.
CREATE TABLE par (id int PRIMARY KEY, a text);
CREATE TABLE ch () INHERITS (par);
INSERT INTO par SELECT i, CASE WHEN i % 2 = 0 THEN 'x' END
FROM generate_series(1, 100) i;
ANALYZE par;
SELECT gaps.create_model('pm');
SELECT gaps.declare_attribute('pm', 'par', 'a', ARRAY['x']);
SELECT plan_rows($$SELECT * FROM gaps.marginals('pm')$$) AS inherited;

-- A coarse entity key: 1000 rows share 10 entities and a is missing in the
-- 100 rows of entity 0, so 100 * 2 = 200 estimated against one variable,
-- 2 rows, actual.  The estimate counts rows, not entities, as documented.
CREATE TABLE ce (id int, k int, a text);
INSERT INTO ce SELECT i, i % 10, CASE WHEN i % 10 = 0 THEN NULL ELSE 'x' END
FROM generate_series(1, 1000) i;
ANALYZE ce;
SELECT gaps.create_model('cm');
SELECT gaps.declare_attribute('cm', 'ce', 'a', ARRAY['x', 'y'], ARRAY['k']);
SELECT gaps.add_stripe('cm', 's', 'ce', 'a');
SELECT gaps.add_factor('cm', 'f', 's');
SELECT gaps.set_cpt('cm', 'f', ARRAY[0.5, 0.5]);
SELECT plan_rows($$SELECT * FROM gaps.marginals('cm')$$) AS coarse_est,
	   (SELECT count(*) FROM gaps.marginals('cm')) AS coarse_actual;

-- A table never analyzed has no statistics: the default.
CREATE TABLE u (id int PRIMARY KEY, c text);
SELECT gaps.create_model('um');
SELECT gaps.declare_attribute('um', 'u', 'c', ARRAY['x']);
SELECT plan_rows($$SELECT * FROM gaps.marginals('um')$$) AS unanalyzed;

DROP TABLE t, u, par, ch, ce;
SELECT gaps.drop_model('rm');
SELECT gaps.drop_model('um');
SELECT gaps.drop_model('pm');
SELECT gaps.drop_model('cm');
DROP FUNCTION plan_rows(text);
DROP ROLE gaps_rows_other;
