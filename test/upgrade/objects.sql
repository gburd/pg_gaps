-- Every object of extension pg_gaps, described without OIDs, one per line
-- and sorted, for test/upgrade/run.sh to compare an updated database with a
-- fresh install.  Covers what the extension owns (pg_depend 'e'), the
-- functions' definitions and attributes, the tables' columns, defaults,
-- constraints, indexes, triggers, policies and row-level security, the
-- composite types, the configuration tables pg_dump keeps, and the
-- privileges on the schema and on gaps.model_member.
SELECT x FROM (
	SELECT 'member ' || pg_describe_object(d.classid, d.objid, d.objsubid) AS x
	FROM pg_depend d
	WHERE d.refclassid = 'pg_extension'::regclass AND d.deptype = 'e'
	  AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'pg_gaps')
	UNION ALL
	SELECT format('function %s returns %s %s strict=%s secdef=%s leakproof=%s parallel=%s '
				  'rows=%s cost=%s support=%s config=%s lang=%s bin=%s src=%s',
				  p.oid::regprocedure, pg_get_function_result(p.oid),
				  p.provolatile, p.proisstrict, p.prosecdef, p.proleakproof, p.proparallel,
				  p.prorows, p.procost, p.prosupport::regproc, p.proconfig,
				  (SELECT l.lanname FROM pg_language l WHERE l.oid = p.prolang),
				  p.probin, md5(p.prosrc))
		|| ' args=' || pg_get_function_arguments(p.oid)
	FROM pg_proc p WHERE p.pronamespace = 'gaps'::regnamespace
	UNION ALL
	SELECT format('column %s.%s %s %s notnull=%s default=%s', a.attrelid::regclass,
				  a.attnum, a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull,
				  pg_get_expr(ad.adbin, ad.adrelid))
	FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid
		LEFT JOIN pg_attrdef ad ON ad.adrelid = a.attrelid AND ad.adnum = a.attnum
	WHERE c.relnamespace = 'gaps'::regnamespace AND a.attnum > 0 AND NOT a.attisdropped
	UNION ALL
	SELECT format('constraint %s %s %s %s', conrelid::regclass, conname,
				  pg_get_constraintdef(oid), condeferrable::text || condeferred::text)
	FROM pg_constraint WHERE connamespace = 'gaps'::regnamespace
	UNION ALL
	SELECT 'index ' || pg_get_indexdef(indexrelid)
	FROM pg_index i JOIN pg_class c ON c.oid = i.indrelid
	WHERE c.relnamespace = 'gaps'::regnamespace
	UNION ALL
	SELECT 'trigger ' || pg_get_triggerdef(t.oid)
	FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid
	WHERE c.relnamespace = 'gaps'::regnamespace AND NOT t.tgisinternal
	UNION ALL
	SELECT format('policy %s.%s %s %s roles=%s using=%s check=%s', schemaname, tablename,
				  policyname, cmd, roles, qual, with_check)
	FROM pg_policies WHERE schemaname = 'gaps'
	UNION ALL
	SELECT format('relation %s kind=%s rls=%s forcerls=%s', c.oid::regclass, c.relkind,
				  c.relrowsecurity, c.relforcerowsecurity)
	FROM pg_class c WHERE c.relnamespace = 'gaps'::regnamespace
	UNION ALL
	SELECT 'config ' || unnest(extconfig)::regclass::text
	FROM pg_extension WHERE extname = 'pg_gaps'
	UNION ALL
	SELECT format('acl schema %s', n.nspacl::text)
	FROM pg_namespace n WHERE n.nspname = 'gaps'
	UNION ALL
	SELECT format('acl %s %s', c.oid::regclass, c.relacl::text)
	FROM pg_class c
	WHERE c.oid = 'gaps.model_member'::regclass
) o ORDER BY x COLLATE "C";
