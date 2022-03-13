/*-------------------------------------------------------------------------
 *  
 * pg_log is a PostgreSQL extension which allows to display 
 * instance log using SQL. 
 * 
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2022, Pierre Forstmann.
 *            
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "utils/guc.h"
#include "utils/fmgrprotos.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

PG_FUNCTION_INFO_V1(pg_log);
static Datum pg_log_internal(FunctionCallInfo fcinfo);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(DEBUG5, "pg_log:_PG_init():entry");

	elog(DEBUG5, "pg_log:_PG_init():exit");
}

/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	elog(DEBUG5, "pg_log:_PG_fini():entry");

	elog(DEBUG5, "pg_log:_PG_fini():exit");
}

Datum pg_log(PG_FUNCTION_ARGS)
{
   return (pg_log_internal(fcinfo));
}

static Datum pg_log_internal(FunctionCallInfo fcinfo)
{

	ReturnSetInfo 	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		randomAccess;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	AttInMetadata	 *attinmeta;
	MemoryContext 	oldcontext;
	int 		i;
	int		line_count;
	text		lfn;	
	PGFunction	func;
	const char	*log_filename;	
	const char	*log_directory;
	StringInfoData	full_log_filename;
	Datum		result;

	
	log_directory = GetConfigOption("log_directory", true, false);
	log_filename = GetConfigOption("log_filename", true, false);

	initStringInfo(&full_log_filename);
	appendStringInfo(&full_log_filename, "./");
	appendStringInfoString(&full_log_filename, log_directory);
	appendStringInfo(&full_log_filename, "/");
	appendStringInfoString(&full_log_filename, log_filename);

	func = pg_read_file_v2;
	memcpy(VARDATA(&lfn), full_log_filename.data, full_log_filename.len);
	SET_VARSIZE(&lfn, sizeof(text) + full_log_filename.len);
	result =  DirectFunctionCall1Coll(func, 100, (Datum)&lfn);

	line_count = 0;

	/* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
#if PG_VERSION_NUM <= 120000
	tupdesc = CreateTemplateTupleDesc(1, false);
#else
	tupdesc = CreateTemplateTupleDesc(1);
#endif
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "message",
					   TEXTOID, -1, 0);

	randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	for (i=0; i < line_count; i++)
	{
		char 		*values[2];
		HeapTuple	tuple;
		char		buf_v1[10];
		char		buf_v2[30];


		values[0] = buf_v1;
		tuple = BuildTupleFromCStrings(attinmeta, values);
		tuplestore_puttuple(tupstore, tuple);

	}

	return (Datum)0;

}
