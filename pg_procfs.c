/*-------------------------------------------------------------------------
 *  
 * pg_procfs is a PostgreSQL extension which allows to display 
 * /procfs data using SQL. 
 * 
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2022, Pierre Forstmann.
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group 
 *            
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "utils/guc.h"
#include "utils/fmgrprotos.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "utils/timestamp.h"
#include "executor/spi.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "pgstat.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/builtins.h"

#if PG_VERSION_NUM < 130000
#include "catalog/pg_type.h"
#endif

#define PG_PROCFS_MAX_LINE_SIZE	32768	

PG_MODULE_MAGIC;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

PG_FUNCTION_INFO_V1(pg_read);
PG_FUNCTION_INFO_V1(pg_procfs);
static Datum pg_read_internal(const char *filename);
static Datum pg_procfs_internal(FunctionCallInfo fcinfo);

/*---- Variable declarations ----*/

static text *g_result;

/*
 * structure to access /procfs data 
 */
struct	logdata {
	text	*data;
	int	char_count;
	int	line_count;
	int	i; 
	int	max_line_size;
	char	*p;
	int	first_newline_position;
} g_logdata;


/*
 * Module load callback
 */
void
_PG_init(void)
{


	elog(DEBUG5, "pg_procfs:_PG_init():entry");

	/* get the configuration */

	/* currently no GUC parameter */

	elog(DEBUG5, "pg_procfs:_PG_init():exit");
}

/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	elog(DEBUG5, "pg_procfs:_PG_fini():entry");

	elog(DEBUG5, "pg_procfs:_PG_fini():exit");
}
/* --- ---- */


static void logdata_start(text *p_result)
{
	g_logdata.data = p_result;	
	g_logdata.char_count = 0;
       	g_logdata.p = VARDATA(p_result);
	g_logdata.line_count = 0;
       	g_logdata.i = 0;
	g_logdata.max_line_size = 0;
	g_logdata.first_newline_position = -1;

}

static void logdata_start_from_newline(text *p_result)
{
	/*
	 * to start reading after first newline
	 */
	g_logdata.data = p_result;	
	g_logdata.char_count = g_logdata.first_newline_position + 1;
       	g_logdata.p = (char *)VARDATA(p_result);
	g_logdata.line_count = 0;
       	g_logdata.i = 0;

}

static bool logdata_has_more()
{
	return  g_logdata.char_count < VARSIZE(g_logdata.data);

}

static void logdata_next()
{
        g_logdata.char_count++;
	g_logdata.i++;
}

static	char logdata_get()
{
	return *(g_logdata.p + g_logdata.char_count);
}

static	void logdata_incr_line_count()
{
	g_logdata.line_count++;
}

static void logdata_set_max_line_size()
{
	g_logdata.max_line_size = g_logdata.i;
}

static bool logdata_index_gt_max_line_size()
{
	return g_logdata.i > g_logdata.max_line_size;
}

static int logdata_get_index()
{
	return g_logdata.i;
}

static void logdata_reset_index()
{
	g_logdata.i = 0;
}

static int logdata_get_char_count()
{
	return g_logdata.char_count;
}

static int logdata_get_line_count()
{
	return g_logdata.line_count;
}

static int logdata_get_max_line_size()
{
	return g_logdata.max_line_size;
}

static void logdata_set_first_newline_position (int pos)
{
	g_logdata.first_newline_position = pos;
}

/* --- ---- */

Datum pg_read(PG_FUNCTION_ARGS)
{
   char *filename;

   filename = PG_GETARG_CSTRING(0); 
   return (pg_read_internal(filename));
}

static Datum pg_read_internal(const char *filename)
{
	
	PGFunction	func;
	text		*lfn;	
	int64		offset;
	int64		length;
	struct stat	stat_buf;
	int		rc;
	text		*result;
	char		c;
	int		first_newline_position = 0;

	rc = stat(filename, &stat_buf);
	if (rc != 0)
		elog(ERROR, "pg_read_internal: stat failed on %s", filename);

	elog(DEBUG1, "pg_read_internal: %s has %ld bytes", filename, stat_buf.st_size); 

	lfn = (text *) palloc(strlen(filename) + VARHDRSZ);
	memcpy(VARDATA(lfn), filename, strlen(filename));
	SET_VARSIZE(lfn, strlen(filename) + VARHDRSZ);

	offset = 0; 
#if PG_VERSION_NUM > 110000
	func = pg_read_file_all;
#else
	func = pg_read_file;
#endif
	result =  (text *)DirectFunctionCall1(func, (Datum)lfn);

	/*
	 * check returned data
	 */

	for (logdata_start(result);  logdata_has_more(); logdata_next())
	{
		c = logdata_get();
		if (c == '\n')
		{
			  if (first_newline_position == 0)
				  first_newline_position = logdata_get_index();
			  logdata_incr_line_count();
			  if (logdata_index_gt_max_line_size())
			  	logdata_set_max_line_size();
			  logdata_reset_index();
		}
	
	}

	logdata_set_first_newline_position(first_newline_position);

	elog(DEBUG1, "pg_read_internal: checked %d characters in %d lines (longest=%d)", logdata_get_char_count(), logdata_get_line_count(), logdata_get_max_line_size());
	g_result = result;

	return (Datum)0;

}

Datum pg_procfs(PG_FUNCTION_ARGS)
{
   char *filename;

   filename = PG_GETARG_CSTRING(0); 
   elog(DEBUG1, "pg_procfs: filename=%s", filename); 

   pg_read_internal(filename);
   return (pg_procfs_internal(fcinfo));
}


static Datum pg_procfs_internal(FunctionCallInfo fcinfo)
{


	ReturnSetInfo 	*rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	bool		randomAccess;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	AttInMetadata	 *attinmeta;
	MemoryContext 	oldcontext;

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

	/* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
#if PG_VERSION_NUM <= 120000
	tupdesc = CreateTemplateTupleDesc(2, false);
#else
	tupdesc = CreateTemplateTupleDesc(2);
#endif
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "lineno", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "message", TEXTOID, -1, 0);

	randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
	tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	attinmeta = TupleDescGetAttInMetadata(tupdesc);

	for (logdata_start(g_result), i=0 ;logdata_has_more();logdata_next(), i++)
        {
		char		buf_v1[20];
		char		buf_v2[PG_PROCFS_MAX_LINE_SIZE];
		char 		*values[2];
		HeapTuple	tuple;

		c = logdata_get();
		buf_v2[i] = c;
		
		if ( logdata_get_index() > PG_PROCFS_MAX_LINE_SIZE - 1)
			elog(ERROR, "pg_procfs_internal: log line %d larger than %d", logdata_get_line_count() + 1, PG_PROCFS_MAX_LINE_SIZE);

                if (c == '\n')
                {
			sprintf(buf_v1, "%d", logdata_get_line_count());	
			buf_v2[i] = '\0';
			logdata_incr_line_count();
			i = -1;

			values[0] = buf_v1;
			values[1] = buf_v2;
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);
                }
        }

	return (Datum)0;

}

