/*-------------------------------------------------------------------------
 *
 * statscmds.c
 *	  Commands for creating and altering extended statistics
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/statscmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_statistic_ext.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "statistics/statistics.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* qsort comparator for the attnums in CreateStatistics */
static int
compare_int16(const void *a, const void *b)
{
	int			av = *(const int16 *) a;
	int			bv = *(const int16 *) b;

	/* this can't overflow if int is wider than int16 */
	return (av - bv);
}

/*
 *		CREATE STATISTICS
 */
ObjectAddress
CreateStatistics(CreateStatsStmt *stmt)
{
	int16		attnums[STATS_MAX_DIMENSIONS];
	int			numcols = 0;
	ObjectAddress address = InvalidObjectAddress;
	char	   *namestr;
	NameData	stxname;
	Oid			statoid;
	Oid			namespaceId;
	HeapTuple	htup;
	Datum		values[Natts_pg_statistic_ext];
	bool		nulls[Natts_pg_statistic_ext];
	int2vector *stxkeys;
	Relation	statrel;
	Relation	rel;
	Oid			relid;
	ObjectAddress parentobject,
				childobject;
	Datum		types[2];		/* one for each possible type of statistics */
	int			ntypes;
	ArrayType  *stxkind;
	bool		build_ndistinct;
	bool		build_dependencies;
	bool		requested_type = false;
	int			i;
	ListCell   *l;

	Assert(IsA(stmt, CreateStatsStmt));

	/* resolve the pieces of the name (namespace etc.) */
	namespaceId = QualifiedNameGetCreationNamespace(stmt->defnames, &namestr);
	namestrcpy(&stxname, namestr);

	/*
	 * Deal with the possibility that the named statistics already exist.
	 */
	if (SearchSysCacheExists2(STATEXTNAMENSP,
							  NameGetDatum(&stxname),
							  ObjectIdGetDatum(namespaceId)))
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("statistics \"%s\" already exist, skipping",
							namestr)));
			return InvalidObjectAddress;
		}

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("statistics \"%s\" already exist", namestr)));
	}

	/*
	 * CREATE STATISTICS will influence future execution plans but does not
	 * interfere with currently executing plans.  So it should be enough to
	 * take only ShareUpdateExclusiveLock on relation, conflicting with
	 * ANALYZE and other DDL that sets statistical information, but not with
	 * normal queries.
	 */
	rel = relation_openrv(stmt->relation, ShareUpdateExclusiveLock);
	relid = RelationGetRelid(rel);

	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW &&
		rel->rd_rel->relkind != RELKIND_FOREIGN_TABLE &&
		rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a table, foreign table, or materialized view",
						RelationGetRelationName(rel))));

	/*
	 * Transform column names to array of attnums. While at it, enforce some
	 * constraints.
	 */
	foreach(l, stmt->keys)
	{
		char	   *attname = strVal(lfirst(l));
		HeapTuple	atttuple;
		Form_pg_attribute attForm;
		TypeCacheEntry *type;

		atttuple = SearchSysCacheAttName(relid, attname);
		if (!HeapTupleIsValid(atttuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
			  errmsg("column \"%s\" referenced in statistics does not exist",
					 attname)));
		attForm = (Form_pg_attribute) GETSTRUCT(atttuple);

		/* Disallow use of system attributes in extended stats */
		if (attForm->attnum < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("statistics creation on system columns is not supported")));

		/* Disallow data types without a less-than operator */
		type = lookup_type_cache(attForm->atttypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("column \"%s\" cannot be used in statistics because its type has no default btree operator class",
							attname)));

		/* Make sure no more than STATS_MAX_DIMENSIONS columns are used */
		if (numcols >= STATS_MAX_DIMENSIONS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot have more than %d columns in statistics",
							STATS_MAX_DIMENSIONS)));

		attnums[numcols] = ((Form_pg_attribute) GETSTRUCT(atttuple))->attnum;
		numcols++;
		ReleaseSysCache(atttuple);
	}

	/*
	 * Check that at least two columns were specified in the statement. The
	 * upper bound was already checked in the loop above.
	 */
	if (numcols < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("extended statistics require at least 2 columns")));

	/*
	 * Sort the attnums, which makes detecting duplicates somewhat easier, and
	 * it does not hurt (it does not affect the efficiency, unlike for
	 * indexes, for example).
	 */
	qsort(attnums, numcols, sizeof(int16), compare_int16);

	/*
	 * Check for duplicates in the list of columns. The attnums are sorted so
	 * just check consecutive elements.
	 */
	for (i = 1; i < numcols; i++)
		if (attnums[i] == attnums[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_COLUMN),
				  errmsg("duplicate column name in statistics definition")));

	/* Form an int2vector representation of the sorted column list */
	stxkeys = buildint2vector(attnums, numcols);

	/*
	 * Parse the statistics options.  Currently only statistics types are
	 * recognized.
	 */
	build_ndistinct = false;
	build_dependencies = false;
	foreach(l, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(l);

		if (strcmp(opt->defname, "ndistinct") == 0)
		{
			build_ndistinct = defGetBoolean(opt);
			requested_type = true;
		}
		else if (strcmp(opt->defname, "dependencies") == 0)
		{
			build_dependencies = defGetBoolean(opt);
			requested_type = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized STATISTICS option \"%s\"",
							opt->defname)));
	}
	/* If no statistic type was specified, build them all. */
	if (!requested_type)
	{
		build_ndistinct = true;
		build_dependencies = true;
	}

	/* construct the char array of enabled statistic types */
	ntypes = 0;
	if (build_ndistinct)
		types[ntypes++] = CharGetDatum(STATS_EXT_NDISTINCT);
	if (build_dependencies)
		types[ntypes++] = CharGetDatum(STATS_EXT_DEPENDENCIES);
	Assert(ntypes > 0 && ntypes <= lengthof(types));
	stxkind = construct_array(types, ntypes, CHAROID, 1, true, 'c');

	/*
	 * Everything seems fine, so let's build the pg_statistic_ext tuple.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	values[Anum_pg_statistic_ext_stxrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_statistic_ext_stxname - 1] = NameGetDatum(&stxname);
	values[Anum_pg_statistic_ext_stxnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_statistic_ext_stxowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_statistic_ext_stxkeys - 1] = PointerGetDatum(stxkeys);
	values[Anum_pg_statistic_ext_stxkind - 1] = PointerGetDatum(stxkind);

	/* no statistics built yet */
	nulls[Anum_pg_statistic_ext_stxndistinct - 1] = true;
	nulls[Anum_pg_statistic_ext_stxdependencies - 1] = true;

	/* insert it into pg_statistic_ext */
	statrel = heap_open(StatisticExtRelationId, RowExclusiveLock);
	htup = heap_form_tuple(statrel->rd_att, values, nulls);
	CatalogTupleInsert(statrel, htup);
	statoid = HeapTupleGetOid(htup);
	heap_freetuple(htup);
	relation_close(statrel, RowExclusiveLock);

	/*
	 * Invalidate relcache so that others see the new statistics.
	 */
	CacheInvalidateRelcache(rel);

	relation_close(rel, NoLock);

	/*
	 * Add a dependency on the table, so that stats get dropped on DROP TABLE.
	 */
	ObjectAddressSet(parentobject, RelationRelationId, relid);
	ObjectAddressSet(childobject, StatisticExtRelationId, statoid);
	recordDependencyOn(&childobject, &parentobject, DEPENDENCY_AUTO);

	/*
	 * Also add dependency on the schema.  This is required to ensure that we
	 * drop the statistics on DROP SCHEMA.  This is not handled automatically
	 * by DROP TABLE because the statistics might be in a different schema
	 * from the table itself.  (This definition is a bit bizarre for the
	 * single-table case, but it will make more sense if/when we support
	 * extended stats across multiple tables.)
	 */
	ObjectAddressSet(parentobject, NamespaceRelationId, namespaceId);
	recordDependencyOn(&childobject, &parentobject, DEPENDENCY_AUTO);

	/* Return stats object's address */
	ObjectAddressSet(address, StatisticExtRelationId, statoid);

	return address;
}

/*
 * Guts of statistics deletion.
 */
void
RemoveStatisticsById(Oid statsOid)
{
	Relation	relation;
	HeapTuple	tup;
	Form_pg_statistic_ext statext;
	Oid			relid;

	/*
	 * Delete the pg_statistic_ext tuple.  Also send out a cache inval on the
	 * associated table, so that dependent plans will be rebuilt.
	 */
	relation = heap_open(StatisticExtRelationId, RowExclusiveLock);

	tup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statsOid));

	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for statistics %u", statsOid);

	statext = (Form_pg_statistic_ext) GETSTRUCT(tup);
	relid = statext->stxrelid;

	CacheInvalidateRelcacheByRelid(relid);

	simple_heap_delete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(relation, RowExclusiveLock);
}
