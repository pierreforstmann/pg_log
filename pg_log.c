/*-------------------------------------------------------------------------
 *  
 * pg_log is a PostgreSQL extension which allows to display 
 * instance log using SQL. 
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

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */

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

#define PG_LOG_MAX_LINE_SIZE	32768	

PG_MODULE_MAGIC;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

PG_FUNCTION_INFO_V1(pg_get_logname);
PG_FUNCTION_INFO_V1(pg_read);
PG_FUNCTION_INFO_V1(pg_log);
PG_FUNCTION_INFO_V1(pg_log_refresh);
PG_FUNCTION_INFO_V1(pg_log_main);
static char *pg_get_logname_internal();
static Datum pg_read_internal(const char *filename);
static Datum pg_log_internal(FunctionCallInfo fcinfo);
static Datum pg_log_refresh_internal(FunctionCallInfo fcinfo);

/*---- Global variable declarations ----*/

/*
 * GUC settings
 */
static double pg_log_fraction;
static int pg_log_naptime;
static char *pg_log_datname = NULL;
static char *pg_log_default_datname = "pg_log";

/*
 * pg_read_file_v2 output
 */
static text *g_result;

/*
 * structure to store log data 
 *
 * About text, VARDATA and VARSIZE:
 *
 * 1. text is defined in PG c.h as:
 *
 * typedef struct varlena text;
 *
 * with:
 *
 * struct varlena
 * {
 *    char        vl_len_[4];
 *    char        vl_dat[FLEXIBLE_ARRAY_MEMBER];
 * }
 *
 * #define FLEXIBLE_ARRAY_MEMBER   // empty
 *
 * 2. VARDATA and VARSIZE are defined in postgres.h as:
 *
 *  #define VARDATA(PTR)                        VARDATA_4B(PTR)
 *  #define VARSIZE(PTR)                        VARSIZE_4B(PTR)
 *
 *  with:
 *
 *  #define VARDATA_4B(PTR)     (((varattrib_4b *) (PTR))->va_4byte.va_data)
 *  #define VARSIZE_4B(PTR) \
 *    ((((varattrib_4b *) (PTR))->va_4byte.va_header >> 2) & 0x3FFFFFFF)
 *
 * typedef union
 * {
 *    struct
 *    {
 *        uint32      va_header;
 *        char        va_data[FLEXIBLE_ARRAY_MEMBER];
 *    }           va_4byte;
 *    struct
 *    {
 *        uint32      va_header;
 *        uint32      va_tcinfo;/
 *        char        va_data[FLEXIBLE_ARRAY_MEMBER];
 *    }           va_compressed;
 *  } varattrib_4b;
 *
 */

struct	{
        /* data returned by pg_read_file_v2 */	
	text	*data;
	/* current number of processed characters */
	int	char_count;
	/* current number of processed lines */
	int	line_count;
	/* size of biggest line */
	int	max_line_size;
	/* index of current character in current line */
	int	i; 
	/* if pg_log_fraction != 1 this is used to start displaying data 
	 * at beginning of the right line to avoid to display a broken
	 * line.
	 */
	int	first_newline_position;
} g_logdata;

/*
 * flags set by signal handlers
 * */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;


/*
 * Signal handler for SIGTERM
 *	Set a flag to let the main loop to terminate, and set our latch to wake	it up.
 */
static void
pg_log_sigterm(SIGNAL_ARGS)
{
	int	save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 * 	Set a flag to tell the main loop to reread the config file, and set our latch to wake it up.
 */
static void
pg_log_sighup(SIGNAL_ARGS)
{
	int	save_errno = errno;

	got_sighup = true;
	SetLatch(MyLatch);

	errno = save_errno;
}


/*
 * Module load callback
 */
void
_PG_init(void)
{

	BackgroundWorker worker;

	elog(DEBUG5, "pg_log:_PG_init():entry");

	/* get the configuration */
	DefineCustomRealVariable("pg_log.fraction",
				"log fraction to be retrieved",
				NULL,
				&pg_log_fraction,
				0.01,
				0.001,
				1.0,
				PGC_USERSET,
				0,
				NULL,
				NULL,
				NULL);

	DefineCustomIntVariable("pg_log.naptime",
				"duration between each log table refresh (in seconds)",
				NULL,
				&pg_log_naptime,
				30,
				0.1,
				INT_MAX,
				PGC_SIGHUP,
				0,
				NULL,
				NULL,
				NULL);

	DefineCustomStringVariable("pg_log.datname",
				"database name",
				NULL,
				&pg_log_datname,
				NULL,
				PGC_POSTMASTER,
				0,
				NULL,
				NULL,
				NULL);
	

	/* set up common data for all our workers */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = pg_log_naptime;
	sprintf(worker.bgw_library_name, "pg_log");
	sprintf(worker.bgw_function_name, "pg_log_main");
	worker.bgw_notify_pid = 0;

	snprintf(worker.bgw_name, BGW_MAXLEN, "pg_log_worker");
#if PG_VERSION_NUM >= 110000
	snprintf(worker.bgw_type, BGW_MAXLEN, "pg_log");
#endif
	worker.bgw_main_arg = 0;

	RegisterBackgroundWorker(&worker);

	elog(LOG, "%s started with pg_log.naptime=%d seconds", worker.bgw_name, pg_log_naptime);

	elog(LOG, "%s started with pg_log.fraction=%f", worker.bgw_name, pg_log_fraction);

	if (pg_log_datname == NULL)
		pg_log_datname = pg_log_default_datname;

	elog(LOG, "%s started with pg_log.datname=%s", worker.bgw_name, pg_log_datname);

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
/* --- ---- */


static void logdata_start(text *p_result)
{
	g_logdata.data = p_result;	
	g_logdata.char_count = 0;
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
	if (pg_log_fraction != 1)
	 g_logdata.char_count = g_logdata.first_newline_position + 1;
	else	
	 g_logdata.char_count = 0;

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
	return *(VARDATA(g_logdata.data) + g_logdata.char_count);
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

static char *pg_get_logname_internal()
{
	/*
	 * get last modified file in <log_directory>
	 */

	StringInfoData 	buf_select;
	char		*returned_filename;
	char		*filename;
	int		ret_code;
	int		rows_number;


	SPI_connect();	

	initStringInfo(&buf_select);
	appendStringInfo(&buf_select, "select name  from pg_ls_logdir() where modification = (select max(modification) from pg_ls_logdir())");

	/*
	 * pgstart_report_activity can only be called in a transaction:
	 * TRAP: FailedAssertion("!IsTransactionOrTransactionBlock()", File: "pgstat.c", Line: 574
	 */

	ret_code = SPI_execute(buf_select.data, false, 0);
	rows_number = SPI_processed;

	if (ret_code != SPI_OK_SELECT)
		elog(ERROR, "pg_log: SELECT FROM pg_ls_logdir() failed");
	if (rows_number == 0)
		elog(ERROR, "pg_log: SELECT FROM pg_ls_logdir() returned no data");
	else if (rows_number != 1)
		elog(ERROR, "pg_log: SELECT FROM pg_ls_logdir() returned more than 1 row");

	filename = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
	if (filename == NULL)	
		elog(ERROR, "pg_log : SELECT FROM pg_ls_logdir returned NULL");

	/*
	 * SPI_getvalue is using palloc and SPI_finish will deallocate it
	 */
	returned_filename = SPI_palloc(strlen(filename) + 1);
	strcpy(returned_filename, filename);

	/*
	 * pgstart_report_activity can only be called in a transaction
	 * TRAP: FailedAssertion("!IsTransactionOrTransactionBlock()", File: "pgstat.c", Line: 574
	 */

	SPI_finish();	

	return returned_filename;

}

Datum	pg_get_logname(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING(pg_get_logname_internal());
}

Datum pg_read(PG_FUNCTION_ARGS)
{
   char *filename;

   filename = PG_GETARG_CSTRING(0); 
   return (pg_read_internal(filename));
}

static Datum pg_read_internal(const char *filename)
{
	
	const char	*log_filename;
	const char	*log_directory;
	char		*full_log_filename;
	PGFunction	func;
	text		*lfn;	
	int64		offset;
	int64		length;
	struct stat	stat_buf;
	int		rc;
	text		*result;
	char		c;
	int		first_newline_position = 0;


	log_filename = pg_get_logname_internal();
     	log_directory = GetConfigOption("log_directory", true, false);
	full_log_filename = palloc(strlen(log_filename) + strlen(log_directory) + 2);
	strcpy(full_log_filename, log_directory); 
	strcat(full_log_filename, "/");
	strcat(full_log_filename, log_filename);
	
	lfn = (text *) palloc(strlen(full_log_filename) + VARHDRSZ);
	memcpy(VARDATA(lfn), full_log_filename, strlen(full_log_filename));
	SET_VARSIZE(lfn, strlen(full_log_filename) + VARHDRSZ);

	rc = stat(full_log_filename, &stat_buf);
	if (rc != 0)
		elog(ERROR, "pg_log: stat failed on %s", full_log_filename);

	elog(DEBUG1, "pg_log: %s has %ld bytes", full_log_filename, stat_buf.st_size); 

	/*
	 * by default read only the last lines corresponding to pg_log.fraction
	 */
	offset = stat_buf.st_size * ( 1 - pg_log_fraction);
	length = stat_buf.st_size * pg_log_fraction; 
	func = pg_read_file;
	result =  (text *)DirectFunctionCall3(func, (Datum)lfn, (Datum)offset, (Datum)length);

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
	
	/*
	 * if all log must be read do not start at first new line position
	 */
	if (pg_log_fraction == 1)
		logdata_set_first_newline_position(0);
	else
 		logdata_set_first_newline_position(first_newline_position);

	elog(DEBUG1, "pg_log: checked %d characters in %d lines (longest=%d)", logdata_get_char_count(), logdata_get_line_count(), logdata_get_max_line_size());
	g_result = result;

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

	const char	*log_filename = NULL;
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

	for (logdata_start_from_newline(g_result), i=0 ;logdata_has_more();logdata_next(), i++)
        {
		char		buf_v1[20];
		char		buf_v2[PG_LOG_MAX_LINE_SIZE];
		char 		*values[2];
		HeapTuple	tuple;

		c = logdata_get();
		buf_v2[i] = c;
		
		if ( logdata_get_index() > PG_LOG_MAX_LINE_SIZE - 1)
		{
			elog(ERROR, "pg_log: log line %d larger than %d", logdata_get_line_count() + 1, PG_LOG_MAX_LINE_SIZE);
		}

                if (c == '\n')
                {
			sprintf(buf_v1, "%d", logdata_get_line_count());	
			buf_v2[i] = '\0';
			logdata_incr_line_count();
			logdata_reset_index();
			i = -1;

			values[0] = buf_v1;
			values[1] = buf_v2;
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);
                }
        }

	return (Datum)0;

}


Datum pg_log_refresh(PG_FUNCTION_ARGS)
{

   return (pg_log_refresh_internal(fcinfo));

}


static Datum pg_log_refresh_internal(FunctionCallInfo fcinfo)
{

	const char      *log_filename = NULL;
        int             i;
        int             c;
	StringInfoData	buf_insert;
	SPIPlanPtr 	plan_ptr;
	Oid argtypes[2] = { INT4OID, TEXTOID };
	Datum		values[2];
	int		ret_code;

        char            buf_v2[PG_LOG_MAX_LINE_SIZE];

	log_filename = GetConfigOption("log_filename", true, false);
        pg_read_internal(log_filename);

	initStringInfo(&buf_insert);
        appendStringInfo(&buf_insert, "insert into pglog(id, message) values ($1, $2)");

	SPI_connect();

	pgstat_report_activity(STATE_RUNNING, "truncate table pglog");
	SPI_execute("truncate table pglog", false, 0);
	/*
	** shoud only be called when not in a transaction
	** pgstat_report_stat(false);
	*/
	pgstat_report_activity(STATE_IDLE, NULL);

	plan_ptr = SPI_prepare(buf_insert.data, 2, argtypes);

	for (logdata_start_from_newline(g_result), i = 0; logdata_has_more(); logdata_next(), i++)
        {
		c = logdata_get();
                buf_v2[i] = c;

                if ( logdata_get_index() > PG_LOG_MAX_LINE_SIZE - 1)
		{
                        elog(ERROR, "pg_log: log line %d larger than %d", logdata_get_line_count() + 1, PG_LOG_MAX_LINE_SIZE);
		}

                if (c == '\n')
                {
                        buf_v2[i] = '\0';
			logdata_incr_line_count();
			logdata_reset_index();
                        i = -1;

                        values[0] = Int32GetDatum(logdata_get_line_count());
                        values[1] = CStringGetTextDatum(buf_v2);

                        pgstat_report_activity(STATE_RUNNING, buf_insert.data);
                        ret_code = SPI_execute_plan(plan_ptr, values, NULL, false, 0);  
                        pgstat_report_activity(STATE_IDLE, NULL);

                        if (ret_code != SPI_OK_INSERT)
                                 elog(ERROR, "INSERT INTO pglog failed");
                        if (SPI_processed != 1)
                                 elog(ERROR, "INSERT INTO pglog did not process 1 row");

                }
        }



	SPI_finish();
	/*
	** shoud only be called when not in a transaction
	** pgstat_report_stat(false);
	*/
	return (Datum)0;
}

Datum
pg_log_main(PG_FUNCTION_ARGS)
{
	/*
	 *
	 * code structure from src/test/modules/worker_spi/worker_spi.c
	 *
	 */

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, pg_log_sighup);
	pqsignal(SIGTERM, pg_log_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
#if PG_VERSION_NUM >=110000
	BackgroundWorkerInitializeConnection(pg_log_datname, NULL, 0);
#else
	BackgroundWorkerInitializeConnection(pg_log_datname, NULL);
#endif
	elog(LOG, "%s initialized", MyBgworkerEntry->bgw_name);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */

	while (!got_sigterm)
	{
		int	rc;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
#if PG_VERSION_NUM >= 100000
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pg_log_naptime * 1000L,
					   PG_WAIT_EXTENSION);
#else
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   pg_log_naptime * 1000L);
#endif
		ResetLatch(MyLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		CHECK_FOR_INTERRUPTS();

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Start a transaction on which we can run queries.  Note that each
		 * StartTransactionCommand() call should be preceded by a
		 * SetCurrentStatementStartTimestamp() call, which sets both the time
		 * for the statement we're about the run, and also the transaction
		 * start time.  Also, each other query sent to SPI should probably be
		 * preceded by SetCurrentStatementStartTimestamp(), so that statement
		 * start time is always up to date.
		 *
		 * The SPI_connect() call lets us run queries through the SPI manager,
		 * and the PushActiveSnapshot() call creates an "active" snapshot
		 * which is necessary for queries to have MVCC data to work on.
		 *
		 * The pgstat_report_activity() call makes our activity visible
		 * through the pgstat views.
		 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		pg_log_refresh_internal(fcinfo);

		PopActiveSnapshot();
		CommitTransactionCommand();

	}

	proc_exit(1);
}
