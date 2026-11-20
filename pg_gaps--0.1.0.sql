/* pg_gaps--0.1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_gaps" to load this file. \quit

--
-- The model catalog.  doc/DESIGN.md "Model catalog" describes it.  Every
-- table is registered with pg_extension_config_dump so pg_dump keeps user
-- models, and every table has row-level security so a role sees and
-- changes only the models its role owns.
--

CREATE TABLE gaps.model (
	model_id	serial PRIMARY KEY,
	name		text NOT NULL UNIQUE,
	owner		regrole NOT NULL DEFAULT current_user::regrole
);

CREATE TABLE gaps.attribute (
	attribute_id serial PRIMARY KEY,
	model_id	int NOT NULL REFERENCES gaps.model ON DELETE CASCADE,
	rel			regclass NOT NULL,
	relname		text NOT NULL,		-- qualified name when declared
	attname		name NOT NULL,
	typname		text NOT NULL,		-- format_type() when declared
	entity_keys name[] NOT NULL,
	domain		text[] NOT NULL,
	UNIQUE (model_id, rel, attname)
);

CREATE TABLE gaps.stripe (
	stripe_id	serial PRIMARY KEY,
	model_id	int NOT NULL REFERENCES gaps.model ON DELETE CASCADE,
	name		text NOT NULL,
	attribute_id int NOT NULL REFERENCES gaps.attribute ON DELETE CASCADE,
	UNIQUE (model_id, name)
);

CREATE TABLE gaps.stripe_qual (
	stripe_id	int NOT NULL REFERENCES gaps.stripe ON DELETE CASCADE,
	ord			int NOT NULL,
	attname		name NOT NULL,
	op			text NOT NULL CHECK (op IN ('=', '<>', '<', '<=', '>', '>=')),
	value		text NOT NULL,
	PRIMARY KEY (stripe_id, ord)
);

CREATE TABLE gaps.factor (
	factor_id	serial PRIMARY KEY,
	model_id	int NOT NULL REFERENCES gaps.model ON DELETE CASCADE,
	name		text NOT NULL,
	child_stripe int NOT NULL REFERENCES gaps.stripe ON DELETE CASCADE,
	cpt			float8[],			-- NULL until set or learned
	UNIQUE (model_id, name)
);

CREATE TABLE gaps.factor_parent (
	factor_id	int NOT NULL REFERENCES gaps.factor ON DELETE CASCADE,
	ord			int NOT NULL,
	parent_stripe int NOT NULL REFERENCES gaps.stripe ON DELETE CASCADE,
	PRIMARY KEY (factor_id, ord)
);

CREATE TABLE gaps.factor_map (
	factor_id	int NOT NULL,
	ord			int NOT NULL,		-- which parent
	child_att	name NOT NULL,
	parent_att	name NOT NULL,
	PRIMARY KEY (factor_id, ord, child_att),
	FOREIGN KEY (factor_id, ord) REFERENCES gaps.factor_parent ON DELETE CASCADE
);

SELECT pg_catalog.pg_extension_config_dump('gaps.model', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.model_model_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.attribute', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.attribute_attribute_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.stripe', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.stripe_stripe_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.stripe_qual', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.factor', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.factor_factor_id_seq', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.factor_parent', '');
SELECT pg_catalog.pg_extension_config_dump('gaps.factor_map', '');

--
-- Row-level security.  A model belongs to its owner role; a role sees and
-- changes a model when it is a member of that role.  The child tables
-- follow their model through a lookup in gaps.model, which is itself
-- filtered, so they need no policy logic of their own.
--

CREATE FUNCTION gaps._may(m int) RETURNS boolean
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
	SELECT EXISTS (SELECT 1 FROM gaps.model WHERE model_id = m)
$$;

ALTER TABLE gaps.model ENABLE ROW LEVEL SECURITY;
CREATE POLICY model_owner ON gaps.model
	USING (pg_catalog.pg_has_role(owner, 'MEMBER'))
	WITH CHECK (pg_catalog.pg_has_role(owner, 'MEMBER'));

ALTER TABLE gaps.attribute ENABLE ROW LEVEL SECURITY;
CREATE POLICY attribute_model ON gaps.attribute USING (gaps._may(model_id));
ALTER TABLE gaps.stripe ENABLE ROW LEVEL SECURITY;
CREATE POLICY stripe_model ON gaps.stripe USING (gaps._may(model_id));
ALTER TABLE gaps.factor ENABLE ROW LEVEL SECURITY;
CREATE POLICY factor_model ON gaps.factor USING (gaps._may(model_id));

ALTER TABLE gaps.stripe_qual ENABLE ROW LEVEL SECURITY;
CREATE POLICY stripe_qual_model ON gaps.stripe_qual
	USING (EXISTS (SELECT 1 FROM gaps.stripe s WHERE s.stripe_id = stripe_qual.stripe_id));
ALTER TABLE gaps.factor_parent ENABLE ROW LEVEL SECURITY;
CREATE POLICY factor_parent_model ON gaps.factor_parent
	USING (EXISTS (SELECT 1 FROM gaps.factor f WHERE f.factor_id = factor_parent.factor_id));
ALTER TABLE gaps.factor_map ENABLE ROW LEVEL SECURITY;
CREATE POLICY factor_map_model ON gaps.factor_map
	USING (EXISTS (SELECT 1 FROM gaps.factor f WHERE f.factor_id = factor_map.factor_id));

GRANT USAGE ON SCHEMA gaps TO PUBLIC;
GRANT SELECT, INSERT, UPDATE, DELETE ON gaps.model, gaps.attribute, gaps.stripe,
	gaps.stripe_qual, gaps.factor, gaps.factor_parent, gaps.factor_map TO PUBLIC;
GRANT USAGE ON SEQUENCE gaps.model_model_id_seq, gaps.attribute_attribute_id_seq,
	gaps.stripe_stripe_id_seq, gaps.factor_factor_id_seq TO PUBLIC;

--
-- Validation triggers.  These run on every write, through the functions
-- below or by direct DML, so no row can skip them.
--

-- Resolve an attribute row against the live catalogs, failing with a
-- message naming what changed since it was declared: the relation, the
-- column, its type, an entity key, or a column an entity set selects on.
-- Returns the column's current type.
CREATE FUNCTION gaps._resolve(a gaps.attribute) RETURNS text
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	cur text;
	typ text;
	qstripe text;
	qcol name;
BEGIN
	SELECT c.oid::regclass::text INTO cur FROM pg_class c WHERE c.oid = a.rel;
	IF cur IS NULL OR cur <> a.relname THEN
		RAISE EXCEPTION 'relation "%" of model attribute "%" no longer exists',
			a.relname, a.attname
			USING HINT = CASE WHEN cur IS NULL THEN 'Drop the model or its attribute.'
				ELSE format('The OID now belongs to "%s".', cur) END;
	END IF;
	SELECT format_type(t.atttypid, t.atttypmod) INTO typ
	FROM pg_attribute t
	WHERE t.attrelid = a.rel AND t.attname = a.attname AND t.attnum > 0
		AND NOT t.attisdropped;
	IF typ IS NULL THEN
		RAISE EXCEPTION 'column "%" of relation "%" no longer exists', a.attname, a.relname;
	END IF;
	IF typ <> a.typname THEN
		RAISE EXCEPTION 'column "%" of relation "%" changed type from % to %',
			a.attname, a.relname, a.typname, typ;
	END IF;
	FOR i IN 1 .. coalesce(array_length(a.entity_keys, 1), 0) LOOP
		IF NOT EXISTS (SELECT 1 FROM pg_attribute t
					   WHERE t.attrelid = a.rel AND t.attname = a.entity_keys[i]
						 AND t.attnum > 0 AND NOT t.attisdropped) THEN
			RAISE EXCEPTION 'entity key column "%" of relation "%" no longer exists',
				a.entity_keys[i], a.relname;
		END IF;
	END LOOP;
	SELECT s.name, q.attname INTO qstripe, qcol
	FROM gaps.stripe s JOIN gaps.stripe_qual q USING (stripe_id)
	WHERE s.attribute_id = a.attribute_id
	  AND NOT EXISTS (SELECT 1 FROM pg_attribute t
					  WHERE t.attrelid = a.rel AND t.attname = q.attname
						AND t.attnum > 0 AND NOT t.attisdropped)
	ORDER BY s.name, q.ord LIMIT 1;
	IF FOUND THEN
		RAISE EXCEPTION 'column "%" of relation "%" in the entity set of stripe "%" no longer exists',
			qcol, a.relname, qstripe;
	END IF;
	RETURN typ;
END
$$;

CREATE FUNCTION gaps._attribute_check() RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	t record;
BEGIN
	SELECT a.atttypid, a.atttypmod INTO t
	FROM pg_attribute a
	WHERE a.attrelid = NEW.rel AND a.attname = NEW.attname AND a.attnum > 0
		AND NOT a.attisdropped;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'column "%" does not exist in relation %', NEW.attname, NEW.rel;
	END IF;
	NEW.relname := NEW.rel::regclass::text;
	NEW.typname := format_type(t.atttypid, t.atttypmod);

	IF coalesce(array_length(NEW.domain, 1), 0) = 0 THEN
		RAISE EXCEPTION 'the domain of %.% is empty', NEW.relname, NEW.attname;
	END IF;
	IF array_position(NEW.domain, NULL) IS NOT NULL THEN
		RAISE EXCEPTION 'the domain of %.% contains NULL', NEW.relname, NEW.attname
			USING HINT = 'NULL marks a missing value; it cannot also be a value.';
	END IF;
	IF (SELECT count(DISTINCT v) FROM unnest(NEW.domain) v) <> array_length(NEW.domain, 1) THEN
		RAISE EXCEPTION 'the domain of %.% has duplicate values', NEW.relname, NEW.attname;
	END IF;

	IF coalesce(array_length(NEW.entity_keys, 1), 0) = 0 THEN
		RAISE EXCEPTION 'attribute %.% has no entity keys', NEW.relname, NEW.attname
			USING HINT = 'Give the table a primary key or pass entity_keys.';
	END IF;
	IF NEW.attname = ANY (NEW.entity_keys) THEN
		RAISE EXCEPTION 'probabilistic attribute %.% cannot be its own entity key',
			NEW.relname, NEW.attname;
	END IF;
	FOR i IN 1 .. array_length(NEW.entity_keys, 1) LOOP
		IF NOT EXISTS (SELECT 1 FROM pg_attribute a
					   WHERE a.attrelid = NEW.rel AND a.attname = NEW.entity_keys[i]
						 AND a.attnum > 0 AND NOT a.attisdropped) THEN
			RAISE EXCEPTION 'entity key column "%" does not exist in relation %',
				NEW.entity_keys[i], NEW.relname;
		END IF;
	END LOOP;
	RETURN NEW;
END
$$;
CREATE TRIGGER attribute_check BEFORE INSERT OR UPDATE ON gaps.attribute
	FOR EACH ROW EXECUTE FUNCTION gaps._attribute_check();

-- A stripe qual names a deterministic column of the stripe's relation:
-- one that no attribute of the model declares probabilistic.
CREATE FUNCTION gaps._stripe_qual_check() RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	a gaps.attribute;
BEGIN
	SELECT at.* INTO a FROM gaps.stripe s JOIN gaps.attribute at USING (attribute_id)
	WHERE s.stripe_id = NEW.stripe_id;
	IF NOT EXISTS (SELECT 1 FROM pg_attribute t
				   WHERE t.attrelid = a.rel AND t.attname = NEW.attname
					 AND t.attnum > 0 AND NOT t.attisdropped) THEN
		RAISE EXCEPTION 'column "%" does not exist in relation %', NEW.attname, a.relname;
	END IF;
	IF EXISTS (SELECT 1 FROM gaps.attribute x
			   WHERE x.model_id = a.model_id AND x.rel = a.rel AND x.attname = NEW.attname) THEN
		RAISE EXCEPTION 'stripe predicate column %.% is probabilistic', a.relname, NEW.attname
			USING HINT = 'Entity sets select on deterministic columns only.';
	END IF;
	RETURN NEW;
END
$$;
CREATE TRIGGER stripe_qual_check BEFORE INSERT OR UPDATE ON gaps.stripe_qual
	FOR EACH ROW EXECUTE FUNCTION gaps._stripe_qual_check();

-- Domain size of a stripe's attribute.
CREATE FUNCTION gaps._card(stripe int) RETURNS int
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
	SELECT array_length(a.domain, 1)
	FROM gaps.stripe s JOIN gaps.attribute a USING (attribute_id)
	WHERE s.stripe_id = stripe
$$;

-- Check a factor's CPT.  Every entry finite and non-negative, every run of
-- |child domain| entries a distribution: that much the factor row alone
-- determines, and the trigger on gaps.factor checks it on every write.
-- With with_parents, also check the length against the parents' domains; that
-- needs the factor's gaps.factor_parent rows, which pg_restore loads in a
-- later transaction than gaps.factor, so it is checked by the declaring
-- functions and at every grounding, not by the trigger.
CREATE FUNCTION gaps._cpt_check(f int, with_parents boolean) RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	fr gaps.factor;
	k int;
	rows bigint := 1;
	want bigint;
	s float8;
BEGIN
	SELECT * INTO fr FROM gaps.factor WHERE factor_id = f;
	IF fr.cpt IS NULL THEN
		RETURN;
	END IF;
	k := gaps._card(fr.child_stripe);
	SELECT rows * coalesce(exp(sum(ln(gaps._card(parent_stripe))))::bigint, 1) INTO rows
	FROM gaps.factor_parent WHERE factor_id = f;
	want := rows * k;
	IF array_ndims(fr.cpt) <> 1 OR array_length(fr.cpt, 1) % k <> 0
	   OR (with_parents AND array_length(fr.cpt, 1) <> want) THEN
		RAISE EXCEPTION 'CPT of factor "%" has % entries, needs %', fr.name,
			coalesce(array_length(fr.cpt, 1), 0),
			CASE WHEN with_parents THEN want::text ELSE format('a multiple of %s', k) END
			USING DETAIL = format('%s parent combinations x %s child values', rows, k);
	END IF;
	rows := array_length(fr.cpt, 1) / k;
	IF EXISTS (SELECT 1 FROM unnest(fr.cpt) v
			   WHERE v IS NULL OR v < 0 OR v = 'Infinity'::float8 OR v = 'NaN'::float8) THEN
		RAISE EXCEPTION 'CPT of factor "%" has a NULL, negative, infinite or NaN entry', fr.name;
	END IF;
	FOR r IN 0 .. rows - 1 LOOP
		SELECT sum(fr.cpt[r * k + i]) INTO s FROM generate_series(1, k) i;
		IF abs(s - 1.0) > 1e-9 THEN
			RAISE EXCEPTION 'row % of the CPT of factor "%" sums to %, not 1', r + 1, fr.name, s
				USING HINT = 'Each run of child-domain-size consecutive entries is one '
					'conditional distribution.';
		END IF;
	END LOOP;
END
$$;

CREATE FUNCTION gaps._factor_check() RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
	IF TG_TABLE_NAME = 'factor' THEN
		PERFORM gaps._cpt_check(NEW.factor_id, false);
	ELSIF EXISTS (SELECT 1 FROM gaps.factor f WHERE f.factor_id = NEW.factor_id
				  AND f.child_stripe = NEW.parent_stripe) THEN
		RAISE EXCEPTION 'a stripe cannot be a parent of itself';
	END IF;
	RETURN NULL;
END
$$;
CREATE CONSTRAINT TRIGGER factor_check AFTER INSERT OR UPDATE ON gaps.factor
	DEFERRABLE INITIALLY DEFERRED
	FOR EACH ROW EXECUTE FUNCTION gaps._factor_check();
CREATE CONSTRAINT TRIGGER factor_parent_check AFTER INSERT OR UPDATE ON gaps.factor_parent
	DEFERRABLE INITIALLY DEFERRED
	FOR EACH ROW EXECUTE FUNCTION gaps._factor_check();

-- A factor_map equality names an existing column on each side.
CREATE FUNCTION gaps._factor_map_check() RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	crel regclass;
	prel regclass;
BEGIN
	SELECT a.rel INTO crel FROM gaps.factor f JOIN gaps.stripe s ON s.stripe_id = f.child_stripe
		JOIN gaps.attribute a USING (attribute_id) WHERE f.factor_id = NEW.factor_id;
	SELECT a.rel INTO prel FROM gaps.factor_parent p JOIN gaps.stripe s ON s.stripe_id = p.parent_stripe
		JOIN gaps.attribute a USING (attribute_id)
	WHERE p.factor_id = NEW.factor_id AND p.ord = NEW.ord;
	IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = crel AND attname = NEW.child_att
				   AND attnum > 0 AND NOT attisdropped) THEN
		RAISE EXCEPTION 'column "%" does not exist in relation %', NEW.child_att, crel;
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = prel AND attname = NEW.parent_att
				   AND attnum > 0 AND NOT attisdropped) THEN
		RAISE EXCEPTION 'column "%" does not exist in relation %', NEW.parent_att, prel;
	END IF;
	RETURN NEW;
END
$$;
CREATE TRIGGER factor_map_check BEFORE INSERT OR UPDATE ON gaps.factor_map
	FOR EACH ROW EXECUTE FUNCTION gaps._factor_map_check();

--
-- Declaring a model.
--

CREATE FUNCTION gaps._model_id(p_model text) RETURNS int
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	id int;
BEGIN
	SELECT m.model_id INTO id FROM gaps.model m WHERE m.name = p_model;
	IF id IS NULL THEN
		RAISE EXCEPTION 'model "%" does not exist', p_model;
	END IF;
	RETURN id;
END
$$;

CREATE FUNCTION gaps._stripe_id(p_model text, p_stripe text) RETURNS int
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	id int;
BEGIN
	SELECT s.stripe_id INTO id FROM gaps.stripe s
	WHERE s.model_id = gaps._model_id(p_model) AND s.name = p_stripe;
	IF id IS NULL THEN
		RAISE EXCEPTION 'stripe "%" does not exist in model "%"', p_stripe, p_model;
	END IF;
	RETURN id;
END
$$;

-- Note that SQL-function parameters are qualified with the function name
-- wherever a column of the same name is in scope: an unqualified "name"
-- inside a query on gaps.model is the column, not the parameter.
CREATE FUNCTION gaps.create_model(name text) RETURNS void
LANGUAGE sql
SET search_path = pg_catalog, pg_temp
AS $$
	INSERT INTO gaps.model (name) VALUES (create_model.name)
$$;

CREATE FUNCTION gaps.drop_model(name text) RETURNS void
LANGUAGE sql
SET search_path = pg_catalog, pg_temp
AS $$
	DELETE FROM gaps.model m WHERE m.model_id = gaps._model_id(drop_model.name)
$$;

-- Declare rel.att probabilistic with the given domain.  entity_keys
-- defaults to rel's primary key.
CREATE FUNCTION gaps.declare_attribute(model text, rel regclass, att name,
									   domain text[], entity_keys name[] DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	keys name[] := entity_keys;
BEGIN
	IF keys IS NULL THEN
		SELECT array_agg(a.attname ORDER BY k.ord) INTO keys
		FROM pg_index i
			CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS k(attnum, ord)
			JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum
		WHERE i.indrelid = rel AND i.indisprimary;
	END IF;
	INSERT INTO gaps.attribute (model_id, rel, relname, attname, typname, entity_keys, domain)
	VALUES (gaps._model_id(model), rel, '', att, '', keys, domain);
END
$$;

-- A stripe over a declared attribute.  Its entity set is every entity of
-- the attribute, narrowed by add_stripe_qual.
CREATE FUNCTION gaps.add_stripe(model text, stripe text, rel regclass, att name)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	m int := gaps._model_id(model);
	a int;
BEGIN
	SELECT x.attribute_id INTO a FROM gaps.attribute x
	WHERE x.model_id = m AND x.rel = add_stripe.rel AND x.attname = att;
	IF a IS NULL THEN
		RAISE EXCEPTION '%.% is not a probabilistic attribute of model "%"', rel, att, model
			USING HINT = 'Declare it with gaps.declare_attribute first.';
	END IF;
	INSERT INTO gaps.stripe (model_id, name, attribute_id) VALUES (m, stripe, a);
END
$$;

CREATE FUNCTION gaps.add_stripe_qual(model text, stripe text, att name, op text, value text)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	s int := gaps._stripe_id(model, stripe);
	n int;
BEGIN
	SELECT coalesce(max(q.ord), 0) + 1 INTO n FROM gaps.stripe_qual q WHERE q.stripe_id = s;
	INSERT INTO gaps.stripe_qual (stripe_id, ord, attname, op, value)
	VALUES (s, n, att, add_stripe_qual.op, add_stripe_qual.value);
END
$$;

-- A first-order factor: child stripe given parent stripes, with a shared
-- CPT (NULL until set or learned).
CREATE FUNCTION gaps.add_factor(model text, factor text, child text,
								parents text[] DEFAULT '{}', cpt float8[] DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	m int := gaps._model_id(model);
	f int;
BEGIN
	INSERT INTO gaps.factor (model_id, name, child_stripe, cpt)
	VALUES (m, factor, gaps._stripe_id(model, child), cpt)
	RETURNING factor_id INTO f;
	FOR i IN 1 .. coalesce(array_length(parents, 1), 0) LOOP
		INSERT INTO gaps.factor_parent (factor_id, ord, parent_stripe)
		VALUES (f, i, gaps._stripe_id(model, parents[i]));
	END LOOP;
	PERFORM gaps._cpt_check(f, true);
END
$$;

-- Map parent number ord (1-based) of a factor by the equality
-- child.child_att = parent.parent_att.
CREATE FUNCTION gaps.map_factor(model text, factor text, ord int,
								child_att name, parent_att name)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	f int;
BEGIN
	SELECT x.factor_id INTO f FROM gaps.factor x
	WHERE x.model_id = gaps._model_id(model) AND x.name = map_factor.factor;
	IF f IS NULL THEN
		RAISE EXCEPTION 'factor "%" does not exist in model "%"', factor, model;
	END IF;
	INSERT INTO gaps.factor_map VALUES (f, ord, child_att, parent_att);
END
$$;

CREATE FUNCTION gaps.set_cpt(model text, factor text, cpt float8[]) RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
	UPDATE gaps.factor x SET cpt = set_cpt.cpt
	WHERE x.model_id = gaps._model_id(model) AND x.name = set_cpt.factor;
	IF NOT FOUND THEN
		RAISE EXCEPTION 'factor "%" does not exist in model "%"', factor, model;
	END IF;
	PERFORM gaps._cpt_check(x.factor_id, true) FROM gaps.factor x
	WHERE x.model_id = gaps._model_id(model) AND x.name = set_cpt.factor;
END
$$;

--
-- Structural validity [W11 3.3.2] (doc/DESIGN.md, "Validity").
--

-- Whether "v1 op v2" holds for two constants of a column, compared as
-- grounding compares them: cast to the column's type, in the column's
-- collation.  op is one of the operators gaps.stripe_qual allows.
CREATE FUNCTION gaps._qual_holds(typ text, coll oid, v1 text, op text, v2 text)
RETURNS boolean
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	coll_clause text := '';
	r boolean;
BEGIN
	IF coll <> 0 THEN
		SELECT format(' COLLATE %I.%I', n.nspname, c.collname) INTO coll_clause
		FROM pg_collation c JOIN pg_namespace n ON n.oid = c.collnamespace
		WHERE c.oid = coll;
	END IF;
	EXECUTE format('SELECT ($1)::%s%s %s ($2)::%s', typ, coll_clause, op, typ) INTO r USING v1, v2;
	RETURN r;
END
$$;

-- Whether the quals on a column of type typ can be reasoned about as a
-- total order.  True when the operators grounding applies to two values of
-- typ (gaps._stripe_where writes "col op const::typ" under search_path
-- pg_catalog, so these are the pg_catalog operators taking typ on both
-- sides) are <, <=, =, >= and > of one btree operator family, at their
-- btree strategies, and <> is the negator of that =.  Then = is an
-- equivalence and the order is total and consistent with it, which is what
-- the contradiction rules of gaps._may_share_rvs assume.  Box, for one, has
-- all six operators and = compares areas, so it fails here.
--
-- Also requires the type's input function to be immutable.  A stable one
-- (date, timestamp, timestamptz, time, timetz, interval, money, the reg*
-- types) parses a constant by TimeZone, DateStyle, IntervalStyle or
-- lc_monetary, so a proof over its constants would hold only for the
-- session's settings.
--
-- shortcut: a type with no operators of its own (varchar, a domain) fails,
-- although grounding resolves its quals to the base type's operators;
-- resolve through binary coercion if such proofs are wanted.
CREATE FUNCTION gaps._total_order(typ oid) RETURNS boolean
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
	SELECT (SELECT p.provolatile = 'i' FROM pg_type t JOIN pg_proc p ON p.oid = t.typinput
			WHERE t.oid = typ)
	   AND EXISTS (
		SELECT 1
		FROM pg_operator o
			JOIN pg_amop am ON am.amopopr = o.oid
			JOIN pg_am ON pg_am.oid = am.amopmethod AND pg_am.amname = 'btree'
		WHERE o.oprnamespace = 'pg_catalog'::regnamespace
		  AND o.oprleft = typ AND o.oprright = typ
		  AND (o.oprname, am.amopstrategy) IN (('<', 1), ('<=', 2), ('=', 3),
											   ('>=', 4), ('>', 5))
		  AND (o.oprname <> '=' OR EXISTS (
				SELECT 1 FROM pg_operator ne
				WHERE ne.oprnamespace = 'pg_catalog'::regnamespace AND ne.oprname = '<>'
				  AND ne.oprleft = typ AND ne.oprright = typ AND ne.oprnegate = o.oid))
		GROUP BY am.amopfamily
		HAVING count(DISTINCT o.oprname) = 5)
$$;

-- Whether each entity of attribute att is one row, so that a qual on any
-- column of the relation is fixed per entity.  That holds when the entity
-- keys include the primary key, the key is checked on every statement (not
-- deferrable), and the relation has no inheritance children or partitions,
-- which grounding reads too and the key does not cover.  Otherwise only
-- quals on the entity keys are fixed per entity: rows of one coarse entity
-- may fall on both sides of a qual on another column (AGENTS.md rule 19).
CREATE FUNCTION gaps._whole_rows(att int) RETURNS boolean
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
	SELECT EXISTS (
		SELECT 1 FROM pg_index i
		WHERE i.indrelid = a.rel AND i.indisprimary AND i.indimmediate
		  AND NOT EXISTS (SELECT 1 FROM unnest(i.indkey) k(attnum)
							JOIN pg_attribute t ON t.attrelid = a.rel AND t.attnum = k.attnum
						  WHERE t.attname <> ALL (a.entity_keys)))
		AND NOT EXISTS (SELECT 1 FROM pg_inherits h WHERE h.inhparent = a.rel)
	FROM gaps.attribute a WHERE a.attribute_id = att
$$;

-- Whether stripes s1 and s2 may share a random variable, decided from the
-- catalog alone, for any table contents.  They share one when they are over
-- the same attribute and their entity sets meet.  The answer is false only
-- when that is impossible: the attributes differ, or two quals of the
-- stripes (from either one, so a stripe whose own quals contradict is
-- empty) contradict each other on a column that is fixed per entity and
-- whose type is a total order (gaps._total_order).  A column is fixed per
-- entity when it is an entity key, or on any column when whole_rows, which
-- the caller passes as gaps._whole_rows of s1's attribute.
--
-- Two quals contradict when one is "= c" and c fails the other, or one is a
-- lower bound (>, >=) above the other's upper bound (<, <=), or both meet
-- at one constant and either is strict.  These hold when the operators are
-- a btree total order, which the column test requires, so the test is
-- sound.  It is not complete: "<>" never contradicts anything but "=", and
-- a discrete type's gaps (s > 1 AND s < 2 over int) are not seen, so such
-- pairs count as may share.
--
-- Examines only the columns the quals name, so for stripes with n1 and n2
-- quals it makes at most n1 + n2 column tests and (n1 + n2)^2 constant
-- comparisons, whatever the width of the relation; gaps.validate bounds the
-- total.
CREATE FUNCTION gaps._may_share_rvs(s1 int, s2 int, whole_rows boolean) RETURNS boolean
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	att int;
	rel regclass;
	keys name[];
	q record;
BEGIN
	SELECT s.attribute_id, a.rel, a.entity_keys INTO att, rel, keys
	FROM gaps.stripe s JOIN gaps.attribute a USING (attribute_id)
	WHERE s.stripe_id = s1;
	IF att IS DISTINCT FROM (SELECT s.attribute_id FROM gaps.stripe s WHERE s.stripe_id = s2) THEN
		RETURN false;
	END IF;
	IF NOT EXISTS (SELECT 1 FROM gaps.stripe_qual sq WHERE sq.stripe_id IN (s1, s2)) THEN
		RETURN true;
	END IF;

	-- The columns the quals name that the rules may use, then every ordered
	-- pair of quals on one of them, so each rule below needs to look at one
	-- order only.  The first CTE is materialized so that gaps._total_order
	-- runs once per named column, not over every column of rel.
	FOR q IN WITH named AS MATERIALIZED (
				SELECT t.attname, t.atttypid, format_type(t.atttypid, t.atttypmod) AS typ,
					   t.attcollation AS coll
				FROM pg_attribute t
				WHERE t.attrelid = rel AND t.attnum > 0 AND NOT t.attisdropped
				  AND t.attname IN (SELECT sq.attname FROM gaps.stripe_qual sq
									WHERE sq.stripe_id IN (s1, s2))
				  AND (whole_rows OR t.attname = ANY (keys))),
			 fixed AS MATERIALIZED (
				SELECT n.attname, n.typ, n.coll FROM named n WHERE gaps._total_order(n.atttypid))
			 SELECT x.op AS op1, x.value AS v1, y.op AS op2, y.value AS v2, c.typ, c.coll
			 FROM gaps.stripe_qual x
				JOIN gaps.stripe_qual y ON y.attname = x.attname
				JOIN fixed c ON c.attname = x.attname
			 WHERE x.stripe_id IN (s1, s2) AND y.stripe_id IN (s1, s2) LOOP
		IF q.op1 = '=' AND NOT gaps._qual_holds(q.typ, q.coll, q.v1, q.op2, q.v2) THEN
			RETURN false;
		END IF;
		-- A lower bound v1 and an upper bound v2 leave nothing between them
		-- when v1 > v2, or when v1 = v2 and either bound excludes it.
		IF q.op1 IN ('>', '>=') AND q.op2 IN ('<', '<=')
		   AND gaps._qual_holds(q.typ, q.coll, q.v1,
								CASE WHEN q.op1 = '>=' AND q.op2 = '<=' THEN '>' ELSE '>=' END,
								q.v2) THEN
			RETURN false;
		END IF;
	END LOOP;
	RETURN true;
END
$$;

-- Render a cycle of the factor graph built by gaps.validate, given in edge
-- order (cyc[i] -> cyc[i + 1], and the last back to the first), as its
-- stripes: "p -> c" for a factor's parent and child, "c ~ p" where the
-- child of one factor may share RVs with a parent of the next, and a single
-- name where they are the same stripe.  ef, eg and ep are the graph's
-- edges and each edge's witness parent stripe.
CREATE FUNCTION gaps._render_cycle(cyc int[], ef int[], eg int[], ep text[]) RETURNS text
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	k int := cardinality(cyc);
	out text;
	first text;
	pin text;
	prev text;
	cname text;
BEGIN
	FOR i IN 1 .. k LOOP
		SELECT e.p INTO pin FROM unnest(ef, eg, ep) e(f, g, p)
		WHERE e.f = cyc[CASE WHEN i = 1 THEN k ELSE i - 1 END] AND e.g = cyc[i];
		SELECT s.name INTO cname FROM gaps.factor fa JOIN gaps.stripe s
			ON s.stripe_id = fa.child_stripe WHERE fa.factor_id = cyc[i];
		IF i = 1 THEN
			out := pin;
			first := pin;
		ELSIF pin <> prev THEN
			out := out || ' ~ ' || pin;
		END IF;
		out := out || ' -> ' || cname;
		prev := cname;
	END LOOP;
	IF prev <> first THEN
		out := out || ' ~ ' || first;
	END IF;
	RETURN (SELECT format('factors %s: ', string_agg(fa.name, ', ' ORDER BY u.i))
			FROM unnest(cyc) WITH ORDINALITY u(id, i) JOIN gaps.factor fa ON fa.factor_id = u.id)
		|| out;
END
$$;

-- The dependency-graph check of [W11 3.3.2]: whether the model's ground
-- network is acyclic for every table contents, decided from the catalog
-- alone, without reading or grounding any data.
--
-- The graph has a node per factor and an edge f -> g when f's child stripe
-- may share an RV with a parent stripe of g (gaps._may_share_rvs).  A ground
-- edge runs from an RV of a parent stripe of f to an RV of f's child
-- stripe; for a path to continue, that RV must also be in a parent stripe
-- of the next factor.  So every ground cycle follows a cycle of this graph,
-- and an acyclic graph proves the ground network acyclic.  A self-loop is
-- the thesis's within-factor precondition failing (T2423-2424); a longer
-- cycle is its cycle of alternating directed and undirected edges.
--
-- Returns two rows.  'within-factor': no factor's child may share an RV
-- with one of its own parents.  'dependency graph': no cycle through two or
-- more factors.  When both are ok the model grounds to an acyclic network
-- whatever the data.  A row that is not ok proves nothing: the cycle may
-- not exist in the ground network (Fig. 3.5's phi3 style is fine, a
-- one-to-many walk through time is fine), and grounding checks the ground
-- network exactly (gx_check) in any case.  The detail names what was found.
--
-- The proof is for the catalogs and session as they are when it runs.  It
-- does not survive DDL on the tables (a key dropped or made deferrable, a
-- child table added).  Constants are text cast at use, so grounding parses
-- them under the caller's TimeZone and DateStyle and the model's meaning
-- already depends on the session; validate reasons only about types whose
-- input is immutable (gaps._total_order), so its verdict does not, and any
-- other type's quals count as may share.
--
-- Cost: the attributes are resolved first, as grounding resolves them.
-- Then each distinct pair of a child stripe and a parent stripe on one
-- attribute is tested once (gaps._may_share_rvs, which looks only at the
-- columns the pair's quals name); a pair whose stripes have n1 and n2 quals
-- counts 10 + (n1 + n2)^2 (the test's fixed cost, then a bound on its
-- column tests and qual comparisons), and the sum over the pairs is limited
-- to 200000 before any test runs.  So validate makes at most 20000 pair
-- tests and 200000 qual comparisons, a few seconds either way (6.9 s for
-- 19900 pairs, 2.0 s for one pair of 444 quals, on the development host).
-- The cycle search makes at most F passes over at most F^2 edges for F
-- factors, so models are limited to 100 factors.  Grounding does not call
-- this: gx_check finds ground cycles exactly in time linear in the ground
-- network, so a structural proof saves it nothing.
CREATE FUNCTION gaps.validate(model text, OUT test text, OUT ok boolean, OUT detail text)
RETURNS SETOF record
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	m int := gaps._model_id(model);
	nf int;
	ncmp bigint;
	pc int[];
	pp int[];
	ef int[];
	eg int[];
	ep text[];
	selfloops text;
	alive int[];
	removed int[];
	path int[];
	pred int;
BEGIN
	PERFORM gaps._resolve(a) FROM gaps.attribute a WHERE a.model_id = m ORDER BY a.attribute_id;
	SELECT count(*) INTO nf FROM gaps.factor fa WHERE fa.model_id = m;
	IF nf > 100 THEN
		RAISE EXCEPTION 'model "%" has % factors; gaps.validate handles at most 100', model, nf
			USING HINT = 'Grounding (gaps.marginals) still checks the ground network for cycles exactly.';
	END IF;

	-- The distinct (child stripe, parent stripe) pairs on one attribute, and
	-- what testing them all costs, in the units of the header comment.
	SELECT coalesce(array_agg(x.c), '{}'), coalesce(array_agg(x.p), '{}') INTO pc, pp
	FROM (SELECT DISTINCT f.child_stripe AS c, gp.parent_stripe AS p
		  FROM gaps.factor f
			JOIN gaps.stripe cs ON cs.stripe_id = f.child_stripe
			JOIN gaps.factor g ON g.model_id = f.model_id
			JOIN gaps.factor_parent gp ON gp.factor_id = g.factor_id
			JOIN gaps.stripe ps ON ps.stripe_id = gp.parent_stripe
		  WHERE f.model_id = m AND ps.attribute_id = cs.attribute_id) x;
	SELECT coalesce(sum(10 + ((SELECT count(*) FROM gaps.stripe_qual q WHERE q.stripe_id = x.c)
							 + (SELECT count(*) FROM gaps.stripe_qual q WHERE q.stripe_id = x.p))
							^ 2), 0)
	INTO ncmp FROM unnest(pc, pp) x(c, p);
	IF ncmp > 200000 THEN
		RAISE EXCEPTION 'model "%" needs % units of stripe tests; gaps.validate allows 200000',
			model, ncmp
			USING HINT = 'Grounding (gaps.marginals) still checks the ground network for cycles exactly.';
	END IF;

	-- The edges, each with one witness parent of g, and the self-loops.
	SELECT coalesce(array_agg(x.f ORDER BY x.f, x.g), '{}'),
		   coalesce(array_agg(x.g ORDER BY x.f, x.g), '{}'),
		   coalesce(array_agg(x.p ORDER BY x.f, x.g), '{}'),
		   string_agg(format('factor "%s": child "%s" may share RVs with parent "%s"',
							 x.fname, x.c, x.p), '; ' ORDER BY x.fname) FILTER (WHERE x.f = x.g)
	INTO ef, eg, ep, selfloops
	FROM (WITH entity AS MATERIALIZED (
			SELECT a.attribute_id, gaps._whole_rows(a.attribute_id) AS whole_rows
			FROM gaps.attribute a
			WHERE a.attribute_id IN (SELECT s.attribute_id FROM unnest(pc) u(c)
										JOIN gaps.stripe s ON s.stripe_id = u.c)),
		  share AS MATERIALIZED (
			SELECT u.c, u.p FROM unnest(pc, pp) u(c, p)
				JOIN gaps.stripe s ON s.stripe_id = u.c
				JOIN entity e ON e.attribute_id = s.attribute_id
			WHERE gaps._may_share_rvs(u.c, u.p, e.whole_rows))
		  SELECT f.factor_id AS f, g.factor_id AS g, f.name AS fname, cs.name AS c,
				 min(ps.name) AS p
		  FROM gaps.factor f
			JOIN gaps.stripe cs ON cs.stripe_id = f.child_stripe
			JOIN share x ON x.c = f.child_stripe
			JOIN (SELECT DISTINCT fp.factor_id, fp.parent_stripe
				  FROM gaps.factor_parent fp JOIN gaps.factor g USING (factor_id)
				  WHERE g.model_id = m) gp ON gp.parent_stripe = x.p
			JOIN gaps.factor g ON g.factor_id = gp.factor_id
			JOIN gaps.stripe ps ON ps.stripe_id = gp.parent_stripe
		  WHERE f.model_id = m
		  GROUP BY f.factor_id, g.factor_id, f.name, cs.name) x;
	test := 'within-factor';
	ok := selfloops IS NULL;
	detail := coalesce(selfloops, 'no child stripe may share an RV with its own parents');
	RETURN NEXT;

	-- Kahn's algorithm, ignoring self-loops: repeatedly remove the factors
	-- with no edge from a remaining factor.  What remains lies on or behind
	-- a cycle.
	alive := ARRAY(SELECT fa.factor_id FROM gaps.factor fa WHERE fa.model_id = m ORDER BY 1);
	LOOP
		removed := ARRAY(SELECT u.id FROM unnest(alive) u(id)
						 EXCEPT
						 SELECT e.g FROM unnest(ef, eg) e(f, g)
							JOIN unnest(alive) u(id) ON u.id = e.f
						 WHERE e.f <> e.g);
		EXIT WHEN cardinality(removed) = 0;
		alive := ARRAY(SELECT u.id FROM unnest(alive) u(id) WHERE u.id <> ALL (removed));
	END LOOP;
	test := 'dependency graph';
	ok := cardinality(alive) = 0;
	IF ok THEN
		detail := 'no cycle through two or more factors';
		RETURN NEXT;
		RETURN;
	END IF;

	-- Every remaining factor has a predecessor that remains, so walking
	-- predecessors from one of them repeats a factor within cardinality(alive)
	-- steps.  The walk runs against the edges; reversed, its tail from the
	-- repeated factor is a cycle in edge order.
	path := ARRAY[alive[1]];
	LOOP
		SELECT min(e.f) INTO pred FROM unnest(ef, eg) e(f, g)
		WHERE e.g = path[cardinality(path)] AND e.f <> e.g AND e.f = ANY (alive);
		EXIT WHEN pred = ANY (path);
		path := path || pred;
	END LOOP;
	path := ARRAY(SELECT path[i] FROM generate_series(cardinality(path),
													  array_position(path, pred), -1) i);
	detail := 'may have a cycle through ' || gaps._render_cycle(path, ef, eg, ep);
	RETURN NEXT;
END
$$;

--
-- Inference.
--

CREATE FUNCTION gaps._infer(card int[], evidence int[], child int[], parent_off int[],
							parents int[], cpt_off int[], cpts float8[],
							OUT var int, OUT value int, OUT probability float8,
							OUT component int, OUT map_value int, OUT map_probability float8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gaps_infer'
LANGUAGE C STRICT VOLATILE;

-- The SQL to select one attribute's entities and their evidence:
-- SELECT entity jsonb, ev text[] (the distinct non-NULL values of the
-- attribute among the entity's rows) FROM rel GROUP BY the entity keys.
-- Identifiers come from the catalog and are quoted; nothing from callers
-- is interpolated.
CREATE FUNCTION gaps._entities_sql(a gaps.attribute) RETURNS text
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	keyobj text := '';
	keylist text := '';
BEGIN
	FOR i IN 1 .. array_length(a.entity_keys, 1) LOOP
		keyobj := keyobj || CASE WHEN i > 1 THEN ', ' ELSE '' END
			|| format('%L, %I', a.entity_keys[i], a.entity_keys[i]);
		keylist := keylist || CASE WHEN i > 1 THEN ', ' ELSE '' END
			|| format('%I', a.entity_keys[i]);
	END LOOP;
	RETURN format('SELECT jsonb_build_object(%s) AS entity, '
				  'array_agg(DISTINCT %I::text) FILTER (WHERE %I IS NOT NULL) AS ev '
				  'FROM %s GROUP BY %s',
				  keyobj, a.attname, a.attname, a.rel, keylist);
END
$$;

-- The WHERE clause of a stripe's entity set.  Its constants are elements
-- of one text[] parameter, $1, starting at element firstparam, each cast to
-- its column's type; the caller appends them to the array it passes.
-- Returns the clause (empty for no quals) and the constants in order.
CREATE FUNCTION gaps._stripe_where(stripe int, alias text, firstparam int,
								   OUT clause text, OUT params text[])
LANGUAGE plpgsql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	q record;
	n int := firstparam;
	rel regclass;
BEGIN
	clause := '';
	params := '{}';
	SELECT a.rel INTO rel FROM gaps.stripe s JOIN gaps.attribute a USING (attribute_id)
	WHERE s.stripe_id = stripe;
	FOR q IN SELECT * FROM gaps.stripe_qual WHERE stripe_id = stripe ORDER BY ord LOOP
		clause := clause || format(' AND %I.%I %s ($1[%s])::%s', alias, q.attname, q.op, n,
			(SELECT format_type(atttypid, atttypmod) FROM pg_attribute
			 WHERE attrelid = rel AND attname = q.attname AND attnum > 0 AND NOT attisdropped));
		params := params || q.value;
		n := n + 1;
	END LOOP;
END
$$;

-- Ground a model.  Returns one row per random variable, numbered 1..n,
-- with what gaps._infer needs and what is needed to name the variable in
-- results.  All reads of user tables happen here, through dynamic SQL run
-- as the caller.
--
-- Validity checks (doc/DESIGN.md, "Validity" and "Mappings"):
--   - every RV of every declared attribute is the child of exactly one CPT;
--   - every CPT is set;
--   - every child entity maps to exactly one parent entity per parent;
--   - the rows of one entity agree on its observed value, which is in the
--     attribute's domain.
-- Directed cycles are found by the engine (gx_check).
CREATE FUNCTION gaps._ground(m int,
	OUT card int[], OUT evidence int[], OUT child int[], OUT parent_off int[],
	OUT parents int[], OUT cpt_off int[], OUT cpts float8[],
	OUT var_attr int[], OUT var_entity jsonb[])
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	a gaps.attribute;
	f record;
	p record;
	w record;
	wp record;
	sql text;
	bad record;
BEGIN
	-- Entities and evidence, per attribute, numbered in a stable order.
	CREATE TEMP TABLE IF NOT EXISTS pg_gaps_rv (
		v int, attribute_id int, entity jsonb, card int, ev int)
		ON COMMIT DROP;
	TRUNCATE pg_gaps_rv;
	-- One pass per attribute reads the table once: every entity is stored,
	-- numbered in a stable order, with its evidence, and the two kinds of
	-- bad evidence (rows that disagree, a value outside the domain) are
	-- stored as ev -1 and -2 and reported afterwards, so the common, clean
	-- case does not aggregate the table three times.
	FOR a IN SELECT * FROM gaps.attribute WHERE model_id = m ORDER BY attribute_id LOOP
		PERFORM gaps._resolve(a);
		sql := gaps._entities_sql(a);
		EXECUTE format('INSERT INTO pg_gaps_rv (v, attribute_id, entity, card, ev) '
					   'SELECT $4 + row_number() OVER (ORDER BY e.entity), $1, e.entity, $2, '
					   'CASE WHEN cardinality(e.ev) > 1 THEN -1 '
					   'WHEN e.ev IS NULL THEN 0 '
					   'ELSE coalesce(array_position($3, e.ev[1]), -2) END '
					   'FROM (%s) e', sql)
			USING a.attribute_id, array_length(a.domain, 1), a.domain,
				  (SELECT coalesce(max(r.v), 0) FROM pg_gaps_rv r);
		SELECT r.entity, r.ev INTO bad FROM pg_gaps_rv r
		WHERE r.attribute_id = a.attribute_id AND r.ev < 0 ORDER BY r.v LIMIT 1;
		IF bad.entity IS NOT NULL THEN
			EXECUTE format('SELECT e.ev FROM (%s) e WHERE e.entity = $1', sql)
				INTO sql USING bad.entity;
			IF bad.ev = -1 THEN
				RAISE EXCEPTION 'rows of entity % of %.% disagree: %', bad.entity, a.relname,
					a.attname, sql;
			END IF;
			RAISE EXCEPTION 'value "%" of %.% for entity % is not in its domain %',
				trim(both '{}' from sql), a.relname, a.attname, bad.entity, a.domain;
		END IF;
	END LOOP;
	CREATE INDEX IF NOT EXISTS pg_gaps_rv_key ON pg_gaps_rv (attribute_id, entity);
	ANALYZE pg_gaps_rv;

	-- Each factor: its child entities, each with its parent entities.
	CREATE TEMP TABLE IF NOT EXISTS pg_gaps_inst (
		factor_id int, v int, ord int, pv int) ON COMMIT DROP;
	TRUNCATE pg_gaps_inst;
	FOR f IN SELECT fa.*, s.attribute_id AS cattr
			 FROM gaps.factor fa JOIN gaps.stripe s ON s.stripe_id = fa.child_stripe
			 WHERE fa.model_id = m ORDER BY fa.factor_id LOOP
		IF f.cpt IS NULL THEN
			RAISE EXCEPTION 'factor "%" has no CPT', f.name
				USING HINT = 'Set one with gaps.set_cpt or learn one with gaps.learn_cpts.';
		END IF;
		PERFORM gaps._cpt_check(f.factor_id, true);
		SELECT * INTO a FROM gaps.attribute WHERE attribute_id = f.cattr;
		w := gaps._stripe_where(f.child_stripe, 'c', 1);

		-- The child entities (ord 0 marks the child itself): the distinct
		-- entities of the stripe's rows, joined to their variables.
		EXECUTE format('INSERT INTO pg_gaps_inst (factor_id, v, ord, pv) '
					   'SELECT $2, r.v, 0, r.v FROM '
					   '(SELECT DISTINCT jsonb_build_object(%s) AS entity FROM %s c WHERE TRUE%s) x '
					   'JOIN pg_gaps_rv r ON r.attribute_id = $3 AND r.entity = x.entity',
					   (SELECT string_agg(format('%L, c.%I', k, k), ', ')
						FROM unnest(a.entity_keys) k), a.rel, w.clause)
			USING w.params, f.factor_id, f.cattr;

		-- Each parent: join child rows to parent rows on the mapping and the
		-- parent stripe's predicate; each child entity must reach exactly one
		-- parent entity.
		FOR p IN SELECT fp.ord, fp.parent_stripe, ps.attribute_id
				 FROM gaps.factor_parent fp
					JOIN gaps.stripe ps ON ps.stripe_id = fp.parent_stripe
				 WHERE fp.factor_id = f.factor_id ORDER BY fp.ord LOOP
			DECLARE
				pa gaps.attribute;
				on_clause text;
				nmap int;
			BEGIN
				SELECT * INTO pa FROM gaps.attribute WHERE attribute_id = p.attribute_id;
				SELECT count(*), string_agg(format(' AND c.%I = p.%I', child_att, parent_att), '')
				INTO nmap, on_clause
				FROM gaps.factor_map WHERE factor_id = f.factor_id AND ord = p.ord;
				IF nmap = 0 THEN
					IF pa.rel <> a.rel OR pa.entity_keys <> a.entity_keys THEN
						RAISE EXCEPTION 'factor "%" needs a mapping to parent % (gaps.map_factor)',
							f.name, p.ord
							USING DETAIL = 'Only a parent on the same relation and entity keys '
								'maps to the child entity by default.';
					END IF;
					SELECT string_agg(format(' AND c.%I = p.%I', k, k), '') INTO on_clause
					FROM unnest(a.entity_keys) k;
				END IF;
				wp := gaps._stripe_where(p.parent_stripe, 'p', cardinality(w.params) + 1);

				-- One pass: each child entity with the distinct parent
				-- entities its rows reach (the LEFT JOIN keeps a child that
				-- reaches none, with pes NULL).  Exactly one, which must be a
				-- parent RV, gives its v; anything else is stored with a
				-- non-positive pv (minus the number of parents reached, or -1
				-- for one parent that is not in the parent stripe) and
				-- reported below.
				sql := format(
					'SELECT jsonb_build_object(%s) AS ce, '
					'array_agg(DISTINCT jsonb_build_object(%s)) '
					'FILTER (WHERE p.ctid IS NOT NULL) AS pes '
					'FROM %s c LEFT JOIN %s p ON TRUE%s%s WHERE TRUE%s '
					'GROUP BY 1',
					(SELECT string_agg(format('%L, c.%I', k, k), ', ') FROM unnest(a.entity_keys) k),
					(SELECT string_agg(format('%L, p.%I', k, k), ', ') FROM unnest(pa.entity_keys) k),
					a.rel, pa.rel, on_clause, wp.clause, w.clause);
				EXECUTE format('INSERT INTO pg_gaps_inst (factor_id, v, ord, pv) '
							   'SELECT $2, rc.v, $3, CASE WHEN cardinality(x.pes) = 1 '
							   'THEN coalesce(rp.v, -1) ELSE -coalesce(cardinality(x.pes), 0) END '
							   'FROM (%s) x '
							   'JOIN pg_gaps_rv rc ON rc.attribute_id = $4 AND rc.entity = x.ce '
							   'LEFT JOIN pg_gaps_rv rp ON rp.attribute_id = $5 '
							   'AND rp.entity = x.pes[1] AND cardinality(x.pes) = 1',
							   sql)
					USING w.params || wp.params, f.factor_id, p.ord, f.cattr, pa.attribute_id;
				SELECT r.entity AS ce, i.pv INTO bad
				FROM pg_gaps_inst i JOIN pg_gaps_rv r ON r.v = i.v
				WHERE i.factor_id = f.factor_id AND i.ord = p.ord AND i.pv <= 0
				ORDER BY i.v LIMIT 1;
				IF bad.ce IS NOT NULL THEN
					RAISE EXCEPTION 'factor "%": child entity % maps to % parent entities in parent %',
						f.name, bad.ce, CASE WHEN bad.pv = -1 THEN 0 ELSE -bad.pv END, p.ord
						USING HINT = 'Each child must have exactly one parent per parent stripe; '
							'check the factor''s mapping (gaps.map_factor) and the parent stripe''s '
							'predicate.';
				END IF;

			END;
		END LOOP;
	END LOOP;

	-- Coverage: every RV is the child of exactly one factor.
	SELECT r.v, r.entity, at.relname, at.attname, count(DISTINCT i.factor_id) AS n
	INTO bad
	FROM pg_gaps_rv r JOIN gaps.attribute at USING (attribute_id)
		LEFT JOIN pg_gaps_inst i ON i.v = r.v AND i.ord = 0
	GROUP BY r.v, r.entity, at.relname, at.attname
	HAVING count(DISTINCT i.factor_id) <> 1
	ORDER BY r.v LIMIT 1;
	IF bad.v IS NOT NULL THEN
		RAISE EXCEPTION '%.% of entity % is the child of % factors, not exactly one',
			bad.relname, bad.attname, bad.entity, bad.n
			USING HINT = 'Every probabilistic value, present or missing, needs one CPT.';
	END IF;

	-- Assemble the arrays, in variable order; each CPT appears once in cpts
	-- and every instance of its factor points at it.
	SELECT array_agg(r.card ORDER BY r.v), array_agg(r.ev ORDER BY r.v),
		   array_agg(r.attribute_id ORDER BY r.v), array_agg(r.entity ORDER BY r.v)
	INTO card, evidence, var_attr, var_entity
	FROM pg_gaps_rv r;

	-- One ordered pass over the instances: every child variable (ord 0,
	-- the coverage check above guarantees exactly one per variable) is
	-- followed by its parents in order, so child[], parent_off[] and
	-- parents[] all come out of a single sort.
	WITH fc AS (
		SELECT fa.factor_id,
			   1 + coalesce(sum(cardinality(fa.cpt)) OVER (ORDER BY fa.factor_id
				   ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), 0) AS off
		FROM gaps.factor fa WHERE fa.model_id = m),
	seq AS (
		SELECT i.v, i.ord, i.pv, fc.off,
			   count(*) FILTER (WHERE i.ord > 0)
				   OVER (ORDER BY i.v, i.ord ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING)
				   AS nparents_before
		FROM pg_gaps_inst i JOIN fc USING (factor_id))
	SELECT array_agg(s.v ORDER BY s.v) FILTER (WHERE s.ord = 0),
		   array_agg((1 + coalesce(s.nparents_before, 0))::int ORDER BY s.v)
			   FILTER (WHERE s.ord = 0)
			   || (1 + count(*) FILTER (WHERE s.ord > 0))::int,
		   array_agg(s.off::int ORDER BY s.v) FILTER (WHERE s.ord = 0),
		   coalesce(array_agg(s.pv ORDER BY s.v, s.ord) FILTER (WHERE s.ord > 0), '{}')
	INTO child, parent_off, cpt_off, parents
	FROM seq s;
	SELECT coalesce(array_agg(x ORDER BY fa.factor_id, k), '{}') INTO cpts
	FROM gaps.factor fa, unnest(fa.cpt) WITH ORDINALITY u(x, k) WHERE fa.model_id = m;
END
$$;

-- The posterior distribution of every missing value of the model, given
-- every value that is present.  One row per (missing value, domain value).
-- Values in one component are correlated: their probabilities are
-- marginals and must not be multiplied together (doc/DESIGN.md,
-- Principle 1).  Values in different components are independent.
CREATE FUNCTION gaps.marginals(model text,
	OUT rel regclass, OUT attname name, OUT entity jsonb, OUT value text,
	OUT probability float8, OUT component int, OUT method text)
RETURNS SETOF record
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	g record;
BEGIN
	g := gaps._ground(gaps._model_id(model));
	-- Variables are named by joining pg_gaps_rv, which _ground leaves
	-- behind for the transaction, not by subscripting g.var_entity: a
	-- subscript into an array of jsonb scans from its start, which is
	-- quadratic over millions of variables.
	RETURN QUERY
		SELECT a.rel, a.attname, v.entity, a.domain[r.value],
			   r.probability, r.component, 'exact'::text
		FROM gaps._infer(g.card, g.evidence, g.child, g.parent_off, g.parents,
						 g.cpt_off, g.cpts) r
			JOIN pg_gaps_rv v ON v.v = r.var
			JOIN gaps.attribute a ON a.attribute_id = v.attribute_id;
END
$$;

-- The jointly most probable completion of each component: one row per
-- missing value, with the posterior probability of its component's whole
-- assignment (the same for every row of the component).
CREATE FUNCTION gaps.impute(model text,
	OUT rel regclass, OUT attname name, OUT entity jsonb, OUT value text,
	OUT probability float8, OUT component int, OUT method text)
RETURNS SETOF record
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	g record;
BEGIN
	g := gaps._ground(gaps._model_id(model));
	RETURN QUERY
		SELECT a.rel, a.attname, v.entity, a.domain[r.map_value],
			   r.map_probability, r.component, 'exact'::text
		FROM gaps._infer(g.card, g.evidence, g.child, g.parent_off, g.parents,
						 g.cpt_off, g.cpts) r
			JOIN pg_gaps_rv v ON v.v = r.var
			JOIN gaps.attribute a ON a.attribute_id = v.attribute_id
		WHERE r.value = 1;
END
$$;

-- Learn every factor's CPT from the data: for each combination of parent
-- values, the observed frequencies of the child's values among child
-- entities whose child and parents are all observed, plus alpha in every
-- cell (additive smoothing; alpha > 0 keeps every cell positive, so a value
-- never seen is improbable rather than impossible).  A parent combination
-- with no complete entity gets the uniform distribution, which is also
-- what alpha alone gives it.  Counts are per entity, not per row, so a
-- coarse entity counts once.  Returns the number of complete entities used
-- per factor.
--
-- The CPTs are statistics of the rows the caller can read, and anyone who
-- can read the model can read them (doc/DESIGN.md, "Security").
CREATE FUNCTION gaps.learn_cpts(model text, alpha float8 DEFAULT 1.0,
	OUT factor text, OUT complete_entities bigint)
RETURNS SETOF record
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
	m int := gaps._model_id(model);
	f record;
	g record;
	k int;
	nrows bigint;
	newcpt float8[];
	used bigint;
BEGIN
	IF alpha IS NULL OR alpha < 0 OR alpha = 'Infinity'::float8 OR alpha = 'NaN'::float8 THEN
		RAISE EXCEPTION 'alpha must be a finite non-negative number, not %', alpha;
	END IF;

	-- Grounding needs a CPT on every factor, but only uses them to build
	-- arrays this function ignores; give the unset ones a uniform
	-- placeholder inside this transaction, replaced below.
	UPDATE gaps.factor fa
	SET cpt = array_fill(1.0::float8 / gaps._card(fa.child_stripe),
						 ARRAY[gaps._card(fa.child_stripe)
							   * coalesce((SELECT exp(sum(ln(gaps._card(fp.parent_stripe))))::int
										  FROM gaps.factor_parent fp
										  WHERE fp.factor_id = fa.factor_id), 1)])
	WHERE fa.model_id = m AND fa.cpt IS NULL;
	g := gaps._ground(m);

	FOR f IN SELECT fa.factor_id, fa.name, fa.child_stripe FROM gaps.factor fa
			 WHERE fa.model_id = m ORDER BY fa.factor_id LOOP
		k := gaps._card(f.child_stripe);
		SELECT k * coalesce(exp(sum(ln(gaps._card(fp.parent_stripe))))::bigint, 1) INTO nrows
		FROM gaps.factor_parent fp WHERE fp.factor_id = f.factor_id;

		-- Index of each complete instance's cell: parents then child, child
		-- fastest, all values 1-based in g.evidence.
		WITH inst AS (
			SELECT i.v,
				   array_agg(i2.pv ORDER BY i2.ord) FILTER (WHERE i2.ord > 0) AS ps
			FROM pg_gaps_inst i
				LEFT JOIN pg_gaps_inst i2 ON i2.factor_id = i.factor_id AND i2.v = i.v
			WHERE i.factor_id = f.factor_id AND i.ord = 0
			GROUP BY i.v),
		cells AS (
			SELECT (SELECT coalesce(sum((g.evidence[p] - 1)
						* coalesce((SELECT exp(sum(ln(g.card[q])))::bigint
									FROM unnest(inst.ps[o + 1:]) q), 1)), 0)
					FROM unnest(inst.ps) WITH ORDINALITY u(p, o)) * k
				   + g.evidence[inst.v] - 1 AS cell
			FROM inst
			WHERE g.evidence[inst.v] > 0
			  AND NOT EXISTS (SELECT 1 FROM unnest(inst.ps) p WHERE g.evidence[p] = 0)),
		counts AS (
			SELECT c AS cell, alpha + count(cells.cell) AS n
			FROM generate_series(0, nrows - 1) c LEFT JOIN cells ON cells.cell = c
			GROUP BY c),
		normed AS (
			SELECT cell, CASE WHEN sum(n) OVER w = 0 THEN 1.0 / k
							  ELSE n / sum(n) OVER w END AS p
			FROM counts
			WINDOW w AS (PARTITION BY cell / k))
		SELECT array_agg(p ORDER BY cell), (SELECT count(*) FROM cells)
		INTO newcpt, used FROM normed;

		-- Rows must sum to 1 within the trigger's 1e-9; division can leave a
		-- last-bit error, so the last entry of each row takes up the slack.
		FOR r IN 0 .. nrows / k - 1 LOOP
			newcpt[r * k + k] := 1.0 - (SELECT coalesce(sum(newcpt[r * k + i]), 0)
										FROM generate_series(1, k - 1) i);
		END LOOP;
		UPDATE gaps.factor x SET cpt = newcpt WHERE x.factor_id = f.factor_id;
		factor := f.name;
		complete_entities := used;
		RETURN NEXT;
	END LOOP;
END
$$;
