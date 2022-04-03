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

#define PG_LOG_MAX_LINE_SIZE	1000

PG_MODULE_MAGIC;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

PG_FUNCTION_INFO_V1(pg_get_logname);
PG_FUNCTION_INFO_V1(pg_lfgn);
PG_FUNCTION_INFO_V1(pg_read);
PG_FUNCTION_INFO_V1(pg_log);
PG_FUNCTION_INFO_V1(pg_refresh_log);
PG_FUNCTION_INFO_V1(pg_log_main);
static char *pg_get_logname_internal();
static char *pg_lfgn_internal(pg_time_t timestamp, const char *suffix);
static Datum pg_read_internal(char *filename);
static Datum pg_log_internal(FunctionCallInfo fcinfo);
static Datum pg_refresh_log_internal(FunctionCallInfo fcinfo);

/*---- Variable declarations ----*/

static text *g_result;
static double pg_log_fraction;
static int pg_log_naptime;


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

	elog(LOG, "%s started with pg_log.naptime=%d seconds", 
                  worker.bgw_name,
                  pg_log_naptime);

	elog(LOG, "%s started with pg_log.fraction=%f", 
                  worker.bgw_name,
                  pg_log_fraction);


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

	SPI_finish();	
	
	return returned_filename;

}

Datum	pg_get_logname(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING(pg_get_logname_internal());
}


/*
 * from syslogger.c: logfile_getname
 */
static char *pg_lfgn_internal(pg_time_t timestamp, const char *suffix)
{
     char       *filename;
     int         len;

     char	*log_filename;
     char	*log_directory;

     log_directory = GetConfigOption("log_directory", true, false);
     log_filename = GetConfigOption("log_filename", true, false);

     filename = palloc(MAXPGPATH);

     snprintf(filename, MAXPGPATH, "%s/", log_directory);

     len = strlen(filename);

     /* treat Log_filename as a strftime pattern */
     pg_strftime(filename + len, MAXPGPATH - len, log_filename,
                 pg_localtime(&timestamp, log_timezone));

     if (suffix != NULL)
     {
         len = strlen(filename);
         if (len > 4 && (strcmp(filename + (len - 4), ".log") == 0))
             len -= 4;
         strlcpy(filename + len, suffix, MAXPGPATH - len);
     }

     return filename;
}

Datum pg_lfgn(PG_FUNCTION_ARGS)
{
	pg_time_t logger_file_time = 0;
	PG_RETURN_CSTRING(pg_lfgn_internal(logger_file_time, NULL));
}

Datum pg_read(PG_FUNCTION_ARGS)
{
   char *filename;

   filename = PG_GETARG_CSTRING(0); 
   return (pg_read_internal(filename));
}

static Datum pg_read_internal(char *filename)
{
	
	char		*log_filename;
	char		*log_directory;
	char		*full_log_filename;
	PGFunction	func;
	text		*lfn;	
	int64		offset;
	int64		length;
	struct stat	stat_buf;
	int		rc;
	text		*result;
	int		char_count;
	int		line_count;
	char		*p;
	char		c;
	int 		i;
	int		max_line_size;
	int		first_newline_position = 0;
	text		*new_result;


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

	elog(INFO, "pg_log: %s has %ld bytes", full_log_filename, stat_buf.st_size); 

	/*
	 * by default read only the last 10%
	 */
	offset = stat_buf.st_size * ( 1 - pg_log_fraction);
	length = stat_buf.st_size * pg_log_fraction; 

	func = pg_read_file_v2;
	result =  (text *)DirectFunctionCall3(func, (Datum)lfn, (Datum)offset, (Datum)length);

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
			  if (first_newline_position == 0)
				  first_newline_position = i;
			  line_count++;
			  if (i > max_line_size)
				  max_line_size = i;
			  i = 0;
		}
	}		

	elog(INFO, "pg_log: checked %d characters in %d lines (longest=%d)", char_count, line_count, max_line_size);

	/*
	 * make sure first line is a full line 
	 */
	new_result = palloc(VARSIZE_ANY_EXHDR(result) + VARHDRSZ);
	SET_VARSIZE(new_result, VARSIZE_ANY_EXHDR(result) + VARHDRSZ - first_newline_position - 1);
	memcpy((char *)VARDATA(new_result), 
		(char *)VARDATA(result) + first_newline_position + 1, 
		VARSIZE_ANY_EXHDR(result) - first_newline_position - 1);

	g_result = new_result;

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

	for (char_count = 0, p = VARDATA(g_result), line_count = 0, i = 0;
             char_count < VARSIZE(g_result);
             char_count++, i++)
        {
		char		buf_v1[20];
		char		buf_v2[PG_LOG_MAX_LINE_SIZE];
		char 		*values[2];
		HeapTuple	tuple;

                c = *(p + char_count);
		buf_v2[i] = c;
		
		if ( i > PG_LOG_MAX_LINE_SIZE - 1)
			elog(ERROR, "pg_log: log line %d larger than %d", line_count + 1, PG_LOG_MAX_LINE_SIZE);

                if (c == '\n')
                {
			sprintf(buf_v1, "%d", line_count);	
			buf_v2[i] = '\0';
			line_count++;
			i = -1;

			values[0] = buf_v1;
			values[1] = buf_v2;
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(tupstore, tuple);
                }
        }

	return (Datum)0;

}


Datum pg_refresh_log(PG_FUNCTION_ARGS)
{

   return (pg_refresh_log_internal(fcinfo));

}


static Datum pg_refresh_log_internal(FunctionCallInfo fcinfo)
{

	char            *log_filename = NULL;
	int             line_count;
        int             char_count;
        char            *p;
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

	SPI_execute("truncate table pglog", false, 0);

	plan_ptr = SPI_prepare(buf_insert.data, 2, argtypes);

	for (char_count = 0, p = VARDATA(g_result), line_count = 0, i = 0;
             char_count < VARSIZE(g_result);
             char_count++, i++)
        {

                c = *(p + char_count);
                buf_v2[i] = c;

                if ( i > PG_LOG_MAX_LINE_SIZE - 1)
                        elog(ERROR, "pg_log: log line %d larger than %d", line_count + 1, PG_LOG_MAX_LINE_SIZE);

                if (c == '\n')
                {
                        buf_v2[i] = '\0';
                        line_count++;
                        i = -1;

			values[0] = Int32GetDatum(line_count);
			values[1] = CStringGetTextDatum(buf_v2);
			ret_code = SPI_execute_plan(plan_ptr, values, NULL, false, 0);	

			if (ret_code != SPI_OK_INSERT)
				 elog(ERROR, "INSERT INTO pglog failed");
        		if (SPI_processed != 1)
				 elog(ERROR, "INSERT INTO pglog did not process 1 row");

                }
        }

	SPI_finish();

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
	BackgroundWorkerInitializeConnection("pg_log", NULL, 0);
#else
	BackgroundWorkerInitializeConnection("pg_log", NULL);
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

		pg_refresh_log_internal(fcinfo);

		PopActiveSnapshot();
		CommitTransactionCommand();

	}

	proc_exit(1);
}
