/*-------------------------------------------------------------------------
 *
 * pg_gaps.c
 *	  Module entry point for pg_gaps.
 *
 * pg_gaps brings the BayesStore probabilistic data model to PostgreSQL: a
 * table whose probabilistic columns may be missing (an "incomplete
 * relation"), plus a separately stored probabilistic graphical model that
 * describes how those values are distributed and correlated.  See
 * doc/DESIGN.md for the data model and ROADMAP.md for what is built.
 *
 * This file holds only the module magic block for now.  The inference
 * engine and the SQL interface arrive in the phases ROADMAP.md lists.
 *
 * Copyright (c) 2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"

/*
 * PostgreSQL 18 added PG_MODULE_MAGIC_EXT, which lets pg_get_loaded_modules()
 * report the library's name and version.  PG_GAPS_VERSION comes from the
 * control file through the Makefile, so the two cannot disagree.
 */
#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "pg_gaps",.version = PG_GAPS_VERSION);
#else
PG_MODULE_MAGIC;
#endif
