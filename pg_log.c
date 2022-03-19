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

PG_FUNCTION_INFO_V1(pg_read);
PG_FUNCTION_INFO_V1(pg_log);
static Datum pg_read_internal(char *filename);
static Datum pg_log_internal(FunctionCallInfo fcinfo);

static text *l_result;

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

Datum pg_read(PG_FUNCTION_ARGS)
{
   char *filename;

   filename = PG_GETARG_CSTRING(0); 
   return (pg_read_internal(filename));
}

static Datum pg_read_internal(char *filename)
{

	const char	*log_directory;
	PGFunction	func;
	StringInfoData	full_log_filename;
	text		*lfn;	
	text		*result;
	int		char_count;
	int		line_count;
	char		*p;
	char		c;
	int 		i;
	int		max_line_size;


	/*
	 *  read <filename> which must be in PG log directory
	 */	
	log_directory = GetConfigOption("log_directory", true, false);

	initStringInfo(&full_log_filename);
	appendStringInfo(&full_log_filename, "./");
	appendStringInfoString(&full_log_filename, log_directory);
	appendStringInfo(&full_log_filename, "/");
	appendStringInfoString(&full_log_filename, filename);

	lfn = (text *) palloc(full_log_filename.len + VARHDRSZ);
	func = pg_read_file_v2;
	memcpy(VARDATA(lfn), full_log_filename.data, full_log_filename.len);
	SET_VARSIZE(lfn, full_log_filename.len + VARHDRSZ);
	
	result =  (text *)DirectFunctionCall1Coll(func, /*C.uft8 */ 12546, (Datum)lfn);
	

	/*
	 * check returned data
	 */

	for (char_count = 0, p = VARDATA(result), line_count = 0, i = 0, max_line_size = 0;
	     char_count < VARSIZE(result); 
	     char_count++, i++)
	{
		c = *(p + char_count);
		if (c == '\n')
		{
			  line_count++;
			  if (i > max_line_size)
				  max_line_size = i;
			  i = 0;
		}
	}		

	elog(INFO, "pg_log: checked %d characters in %d lines (longest=%d)", char_count, line_count, max_line_size);

	l_result = result;

	return (Datum)0;

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

	char		*log_filename = NULL;
	int		line_count;
	int		char_count;
	char		*p;
	int		i;
	int		c;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	log_filename = GetConfigOption("log_filename", true, false);
	pg_read_internal(log_filename);

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

	for (char_count = 0, p = VARDATA(l_result), line_count = 0, i = 0;
             char_count < VARSIZE(l_result);
             char_count++, i++)
        {
		char		buf_v1[200];
		char 		*values[1];
		HeapTuple	tuple;

                c = *(p + char_count);
		buf_v1[i] = c;

                if (c == '\n')
                {
			buf_v1[i] = '\0';
			line_count++;
			i = 0;

			values[0] = buf_v1;
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);
                }
        }

	return (Datum)0;

}
