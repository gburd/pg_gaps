-- The model catalog: declaring a first-order Bayesian network over a
-- table, and every rejection the catalog and its triggers make.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- The paper's running example, simplified: per (time, room, sensor) a
-- temperature and a light reading, either of which may be missing.
CREATE TABLE sensor (t int, room int, sid int, temp text, light text,
					 PRIMARY KEY (t, room, sid));

SELECT gaps.create_model('sensors');
SELECT gaps.create_model('sensors');               -- fail: exists
SELECT gaps.declare_attribute('sensors', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('sensors', 'sensor', 'light', ARRAY['dark', 'bright']);
SELECT attname, relname, typname, entity_keys, domain FROM gaps.attribute ORDER BY attname;

-- Attribute rejections.
SELECT gaps.declare_attribute('nosuch', 'sensor', 'temp', ARRAY['a']);       -- fail: no model
SELECT gaps.declare_attribute('sensors', 'sensor', 'nosuch', ARRAY['a']);    -- fail: no column
SELECT gaps.declare_attribute('sensors', 'sensor', 'sid', ARRAY[]::text[]);  -- fail: empty
SELECT gaps.declare_attribute('sensors', 'sensor', 'sid', ARRAY['1', '1']);  -- fail: duplicate
SELECT gaps.declare_attribute('sensors', 'sensor', 'sid', ARRAY['1', NULL]); -- fail: NULL value
SELECT gaps.declare_attribute('sensors', 'sensor', 'sid', ARRAY['1'], ARRAY['sid']::name[]); -- fail: own key
SELECT gaps.declare_attribute('sensors', 'sensor', 'sid', ARRAY['1'], ARRAY['zz']::name[]);  -- fail: no key col
CREATE TABLE nokey (a int, b text);
SELECT gaps.declare_attribute('sensors', 'nokey', 'b', ARRAY['x']);          -- fail: no primary key

-- Stripes and their entity-set predicates.
SELECT gaps.add_stripe('sensors', 'tp', 'sensor', 'temp');
SELECT gaps.add_stripe('sensors', 'l', 'sensor', 'light');
SELECT gaps.add_stripe('sensors', 'tp1', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('sensors', 'tp1', 'sid', '=', '1');
SELECT gaps.add_stripe('sensors', 'tp2', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('sensors', 'tp2', 'sid', '=', '2');
SELECT gaps.add_stripe('sensors', 'x', 'sensor', 'sid');                     -- fail: not declared
SELECT gaps.add_stripe_qual('sensors', 'tp1', 'light', '=', 'dark');         -- fail: probabilistic
SELECT gaps.add_stripe_qual('sensors', 'tp1', 'sid', 'LIKE', '1');           -- fail: operator
SELECT gaps.add_stripe_qual('sensors', 'nosuch', 'sid', '=', '1');           -- fail: no stripe

-- Factors and CPT validation.  CPT order: parents, then child, child
-- varying fastest; each run of |child domain| entries sums to 1.
SELECT gaps.add_factor('sensors', 'prior', 'tp', '{}', ARRAY[0.4, 0.6]);
SELECT gaps.add_factor('sensors', 'l_tp', 'l', ARRAY['tp'], ARRAY[0.8, 0.2, 0.3, 0.7]);
SELECT gaps.add_factor('sensors', 'bad1', 'tp1', '{}', ARRAY[0.5]);            -- fail: length
SELECT gaps.add_factor('sensors', 'bad2', 'tp1', '{}', ARRAY[0.5, 0.6]);       -- fail: sum
SELECT gaps.add_factor('sensors', 'bad3', 'tp1', '{}', ARRAY[1.5, -0.5]);      -- fail: negative
SELECT gaps.add_factor('sensors', 'bad4', 'tp1', '{}', ARRAY['NaN', 1]::float8[]); -- fail: NaN
SELECT gaps.add_factor('sensors', 'bad5', 'tp1', ARRAY['tp1'], ARRAY[1, 0, 0, 1]); -- fail: self parent
SELECT gaps.add_factor('sensors', 'learnme', 'tp2', ARRAY['tp1']);             -- CPT left NULL
SELECT gaps.map_factor('sensors', 'learnme', 1, 't', 't');
SELECT gaps.map_factor('sensors', 'learnme', 1, 'room', 'room');
SELECT gaps.map_factor('sensors', 'learnme', 1, 'nosuch', 'room');             -- fail: no column
SELECT gaps.set_cpt('sensors', 'learnme', ARRAY[0.9, 0.1, 0.1]);               -- fail: length
SELECT gaps.set_cpt('sensors', 'learnme', ARRAY[0.9, 0.1, 0.1, 0.9]);
SELECT name, cpt FROM gaps.factor ORDER BY factor_id;

-- Direct DML meets the per-row checks at once.  The length against the
-- parents' domains needs gaps.factor_parent, which pg_restore loads in a
-- later transaction, so a direct write is checked for it at the next
-- grounding instead (sql/infer.sql); the functions check it at once.
UPDATE gaps.factor SET cpt = ARRAY[0.5, 0.6, 0.5, 0.5] WHERE name = 'learnme';  -- fail: sum
UPDATE gaps.factor SET cpt = ARRAY[0.5, 0.5, 0.5] WHERE name = 'learnme';       -- fail: not a multiple of 2

-- Row-level security: another role sees no models and cannot add to one.
CREATE ROLE gaps_other;
GRANT SELECT ON sensor TO gaps_other;
SET ROLE gaps_other;
SELECT count(*) AS visible_models FROM gaps.model;
SELECT gaps.declare_attribute('sensors', 'sensor', 'sid', ARRAY['1']);   -- fail: not visible
INSERT INTO gaps.factor (model_id, name, child_stripe)
	SELECT 1, 'sneak', 1;                                                  -- fail: RLS
RESET ROLE;

-- Dropping a model removes all of it, and nothing of any other model.
SELECT gaps.create_model('bystander');
SELECT gaps.drop_model('sensors');
SELECT name FROM gaps.model;
SELECT gaps.drop_model('bystander');
SELECT (SELECT count(*) FROM gaps.attribute) + (SELECT count(*) FROM gaps.stripe)
	+ (SELECT count(*) FROM gaps.factor) AS rows_left;
DROP TABLE sensor, nokey;
DROP ROLE gaps_other;
