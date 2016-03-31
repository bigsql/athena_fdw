/*-------------------------------------------------------------------------
 *
 *		  foreign-data wrapper for HIVE
 *
 * Copyright (c) 2012, PostgreSQL Global Development Group
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Atri Sharma <atri.jiit@gmail.com>
 *
 * IDENTIFICATION
 *		  hive_fdw/hive_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libpq/pqsignal.h>
#include "funcapi.h"
#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "storage/ipc.h"

#if (PG_VERSION_NUM >= 90200)
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/planmain.h"
#endif

#include "jni.h"

#define Str(arg) #arg
#define StrValue(arg) Str(arg)
#define STR_PKGLIBDIR StrValue(PKG_LIB_DIR)


PG_MODULE_MAGIC;

static JNIEnv *env;
static JavaVM *jvm;
jobject java_call;
static bool InterruptFlag;   /* Used for checking for SIGINT interrupt */


/*
 * Describes the valid options for objects that use this wrapper.
 */
struct hiveFdwOption
{
	const char	*optname;
	Oid		optcontext;	/* Oid of catalog in which option may appear */
};

/*
 * Valid options for hive_fdw.
 *
 */
static struct hiveFdwOption valid_options[] =
{

	/* Connection options */
        { "host",               ForeignServerRelationId },
        { "port",               ForeignServerRelationId },
	{ "maxheapsize",	ForeignServerRelationId },
	{ "username",		UserMappingRelationId },
	{ "password",		UserMappingRelationId },
	{ "table",		ForeignTableRelationId },
	/* Sentinel */
	{ NULL,			InvalidOid }
};

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */

typedef struct hiveFdwExecutionState
{
	char		*query;
	int		NumberOfRows;
	jobject 	java_call;
	int 		NumberOfColumns;
} hiveFdwExecutionState;

/*
 * SQL functions
 */
extern Datum hive_fdw_handler(PG_FUNCTION_ARGS);
extern Datum hive_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(hive_fdw_handler);
PG_FUNCTION_INFO_V1(hive_fdw_validator);

/*
 * FDW callback routines
 */
#if (PG_VERSION_NUM < 90200)
	static FdwPlan *hivePlanForeignScan(Oid foreigntableid,PlannerInfo *root, RelOptInfo *baserel);
#endif

#if (PG_VERSION_NUM >= 90200)
	static void hiveGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
	static void hiveGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid);
	static ForeignScan *hiveGetForeignPlan(
	    PlannerInfo *root, 
	    RelOptInfo *baserel, 
	    Oid foreigntableid, 
	    ForeignPath *best_path, 
	    List *tlist, 
	    List *scan_clauses
	);
#endif

static void hiveExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void hiveBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *hiveIterateForeignScan(ForeignScanState *node);
static void hiveReScanForeignScan(ForeignScanState *node);
static void hiveEndForeignScan(ForeignScanState *node);

/*
 * Helper functions
 */
static bool hiveIsValidOption(const char *option, Oid context);
static void hiveGetOptions(
    Oid foreigntableid, 
    char **host,
    int *port,	 
    int* maxheapsize, 
    char **username, 
    char **password, 
    char **table
);

/*
 * Uses a String object's content to create an instance of C String
 */
static char* ConvertStringToCString(jobject);
/*
 * JVM Initialization function
 */
static void JVMInitialization(Oid);
/*
 * JVM destroy function
 */
static void DestroyJVM();
/*
 * SIGINT interrupt handler
 */
static void SIGINTInterruptHandler(int);
/*
 * SIGINT interrupt check and process function
 */
static void SIGINTInterruptCheckProcess();

/*
 * SIGINTInterruptCheckProcess
 *		Checks and processes if SIGINT interrupt occurs
 */
static void
SIGINTInterruptCheckProcess()
{

	if (InterruptFlag == true)
	{
		jclass 		HIVEUtilsClass;
		jmethodID 	id_cancel;
		jstring 	cancel_result = NULL;
		char 		*cancel_result_cstring = NULL;

		HIVEUtilsClass = (*env)->FindClass(env, "HIVEUtils");
		if (HIVEUtilsClass == NULL) 
		{
			elog(ERROR, "HIVEUtilsClass is NULL");
		}

		id_cancel = (*env)->GetMethodID(env, HIVEUtilsClass, "Cancel", "()Ljava/lang/String;");
		if (id_cancel == NULL) 
		{
			elog(ERROR, "id_cancel is NULL");
		}
		
		cancel_result = (*env)->CallObjectMethod(env,java_call,id_cancel);
		if (cancel_result != NULL)
		{
			cancel_result_cstring = ConvertStringToCString((jobject)cancel_result);
			elog(ERROR, "%s", cancel_result_cstring);
		}

		InterruptFlag = false;
		elog(ERROR, "Query has been cancelled");

		(*env)->ReleaseStringUTFChars(env, cancel_result, cancel_result_cstring);
		(*env)->DeleteLocalRef(env, cancel_result);
	}
}

/*
 * ConvertStringToCString
 *		Uses a String object passed as a jobject to the function to 
 *		create an instance of C String.
 */
static char*
ConvertStringToCString(jobject java_cstring)
{
	jclass 	JavaString;
	char 	*StringPointer;

	SIGINTInterruptCheckProcess();

	JavaString = (*env)->FindClass(env, "java/lang/String");
	if (!((*env)->IsInstanceOf(env, java_cstring, JavaString)))
	{
		elog(ERROR, "Object not an instance of String class");
	}

	if (java_cstring != NULL)
	{
		StringPointer = (char*)(*env)->GetStringUTFChars(env, (jstring)java_cstring, 0);
	}
	else
	{
		StringPointer = NULL;
	}

	return (StringPointer);
}

/*
 * DestroyJVM
 *		Shuts down the JVM.
 */
static void 
DestroyJVM()
{

	(*jvm)->DestroyJavaVM(jvm);
}

/*
 * JVMInitialization
 *		Create the JVM which will be used for calling the Java routines
 *	        that use HIVE to connect and access the foreign database.
 *
 */
static void
JVMInitialization(Oid foreigntableid)
{
	jint 		res = -5;/* Initializing the value of res so that we can check it later to see whether JVM has been correctly created or not*/
	JavaVMInitArgs 	vm_args;
	JavaVMOption 	*options;
	static bool 	FunctionCallCheck = false;   /* This flag safeguards against multiple calls of JVMInitialization().*/
	char 		strpkglibdir[] = STR_PKGLIBDIR;
	char 		*classpath;
        char            *svr_host = NULL;
        int             svr_port = 0;
	char		*svr_username = NULL;
	char		*svr_password = NULL;
	char		*svr_table = NULL;
	char 		*maxheapsizeoption = NULL;
	int 		svr_maxheapsize = 0;
    	char            *var_CP = NULL;
        char            *var_PGHOME = NULL;
        int             cp_len = 0;
	
	hiveGetOptions(
	    foreigntableid, 
	    &svr_host,
            &svr_port, 
	    &svr_maxheapsize, 
	    &svr_username, 
	    &svr_password, 
	    &svr_table
	);

	SIGINTInterruptCheckProcess();

	if (FunctionCallCheck == false)
	{

	        var_CP = getenv("HIVE_JDBC_CLASSPATH");
                elog(LOG, "HIVE_FDW: var_CP=%s", var_CP);
                var_PGHOME = getenv("PGHOME");
                elog(LOG, "HIVE_FDW: var_PGHOME=%s", var_PGHOME);

                cp_len = strlen(var_CP) + strlen(var_PGHOME) + 25;
                classpath = (char*)palloc(cp_len);
                snprintf(classpath, cp_len, "-Djava.class.path=%s/lib:%s", var_PGHOME, var_CP);

		if (svr_maxheapsize != 0)   /* If the user has given a value for setting the max heap size of the JVM */
		{
			options = (JavaVMOption*)palloc(sizeof(JavaVMOption)*2);
			maxheapsizeoption = (char*)palloc(sizeof(int) + 6);
			snprintf(maxheapsizeoption, sizeof(int) + 6, "-Xmx%dm", svr_maxheapsize);

			options[0].optionString = classpath;
			options[1].optionString = maxheapsizeoption;
			vm_args.nOptions = 2;
		}
		else
		{
			options = (JavaVMOption*)palloc(sizeof(JavaVMOption));
			options[0].optionString = classpath;
			vm_args.nOptions = 1;
		}

		vm_args.version = 0x00010002;
		vm_args.options = options;
		vm_args.ignoreUnrecognized = JNI_FALSE;

		/* Create the Java VM */
		res = JNI_CreateJavaVM(&jvm, (void**)&env, &vm_args);
		if (res < 0) 
		{
			ereport(ERROR,
				 (errmsg("Failed to create Java VM")
				 ));
		}

		InterruptFlag = false;
		/* Register an on_proc_exit handler that shuts down the JVM.*/
		on_proc_exit(DestroyJVM, 0);
		FunctionCallCheck = true;
	}
}
/*
 * SIGINTInterruptHandler
 *		Handles SIGINT interrupt
 */
static void
SIGINTInterruptHandler(int sig)
{

	InterruptFlag = true;
}

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
hive_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine 	*fdwroutine = makeNode(FdwRoutine);
	
	#if (PG_VERSION_NUM < 90200)
	fdwroutine->PlanForeignScan = hivePlanForeignScan;
	#endif

	#if (PG_VERSION_NUM >= 90200)
	fdwroutine->GetForeignRelSize = hiveGetForeignRelSize;
	fdwroutine->GetForeignPaths = hiveGetForeignPaths;
	fdwroutine->GetForeignPlan = hiveGetForeignPlan;
	#endif

	fdwroutine->ExplainForeignScan = hiveExplainForeignScan;
	fdwroutine->BeginForeignScan = hiveBeginForeignScan;
	fdwroutine->IterateForeignScan = hiveIterateForeignScan;
	fdwroutine->ReScanForeignScan = hiveReScanForeignScan;
	fdwroutine->EndForeignScan = hiveEndForeignScan;

	pqsignal(SIGINT, SIGINTInterruptHandler);

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses hive_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
hive_fdw_validator(PG_FUNCTION_ARGS)
{
	List		*options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid		catalog = PG_GETARG_OID(1);
        char            *svr_query = NULL; 
	char            *svr_host = NULL;
        int             svr_port = 0;
	char		*svr_username = NULL;
	char		*svr_password = NULL;
	char		*svr_table = NULL;
	int 		svr_maxheapsize = 0;
	ListCell	*cell;

	/*
	 * Check that only options supported by hive_fdw,
	 * and allowed for the current object type, are given.
	*/ 
	foreach(cell, options_list)
	{
		DefElem	   *def = (DefElem *) lfirst(cell);

		if (!hiveIsValidOption(def->defname, catalog))
		{
			struct hiveFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
							 opt->optname);
			}

			ereport(ERROR, 
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME), 
				errmsg("invalid option \"%s\"", def->defname), 
				errhint("Valid options in this context are: %s", buf.len ? buf.data : "<none>")
				));
		}

		 if (strcmp(def->defname, "host") == 0)
                {
                        if (svr_host)
                                ereport(ERROR,
                                                (errcode(ERRCODE_SYNTAX_ERROR),
                                                 errmsg("conflicting or redundant options")));
                        svr_host = defGetString(def);
                }
                if (strcmp(def->defname, "port") == 0)
                {
                        if (svr_port)
                                ereport(ERROR,
                                                (errcode(ERRCODE_SYNTAX_ERROR),
                                                 errmsg("conflicting or redundant options")));
                        svr_port = atoi(defGetString(def));
                }

		if (strcmp(def->defname, "maxheapsize") == 0)
		{
			if (svr_maxheapsize)
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), 
					errmsg("conflicting or redundant options: maxheapsize (%s)", defGetString(def))
					));

			svr_maxheapsize = atoi(defGetString(def));
		}

		if (strcmp(def->defname, "username") == 0)
		{
			if (svr_username)
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: username (%s)", defGetString(def))
					));

			svr_username = defGetString(def);
		}

		if (strcmp(def->defname, "password") == 0)
		{
			if (svr_password)
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: password (%s)", defGetString(def))
					));

			svr_password = defGetString(def);
		}
		else if (strcmp(def->defname, "query") == 0)
		{
			if (svr_table)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting options: query cannot be used with table")
					));

			if (svr_query)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: query (%s)", defGetString(def))
					));

			svr_query = defGetString(def);
		}
		else if (strcmp(def->defname, "table") == 0)
		{
			if (svr_query)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting options: table cannot be used with query")
					));

			if (svr_table)
				ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("conflicting or redundant options: table (%s)", defGetString(def))
					));

			svr_table = defGetString(def);
		}
	}
	 if (catalog == ForeignServerRelationId && svr_host == NULL)
                ereport(ERROR,
                                (errcode(ERRCODE_SYNTAX_ERROR),
                                 errmsg("host must be specified")));

	if (catalog == ForeignTableRelationId && svr_query == NULL && svr_table == NULL)
	{
		ereport(ERROR,
		(errcode(ERRCODE_SYNTAX_ERROR),
		errmsg("either a table or a query must be specified")
		));
	}

	PG_RETURN_VOID();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
hiveIsValidOption(const char *option, Oid context)
{
	struct hiveFdwOption 	*opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a hive_fdw foreign table.
 */
/*static void
hiveGetOptions(Oid foreigntableid, char **drivername, char **url, int *querytimeout, char **jarfile, int *maxheapsize, char **username, char **password, char **query, char **table)
*/
static void
hiveGetOptions(Oid foreigntableid, char **host, int *port, int *maxheapsize, char **username, char **password, char **table)
{
	ForeignTable	*f_table;
	ForeignServer	*f_server;
	UserMapping	*f_mapping;
	List		*options;
	ListCell	*lc;

	/*
	 * Extract options from FDW objects.
	 */
	f_table = GetForeignTable(foreigntableid);
	f_server = GetForeignServer(f_table->serverid);
	f_mapping = GetUserMapping(GetUserId(), f_table->serverid);

	options = NIL;
	options = list_concat(options, f_table->options);
	options = list_concat(options, f_server->options);
	options = list_concat(options, f_mapping->options);

	/* Loop through the options, and get the server/port */
	foreach(lc, options)
	{
		DefElem *def = (DefElem *) lfirst(lc);

/*		if (strcmp(def->defname, "drivername") == 0)
		{
			*drivername = defGetString(def);
		}
*/
		if (strcmp(def->defname, "username") == 0)
		{
			*username = defGetString(def);
		}

/*		if (strcmp(def->defname, "querytimeout") == 0)
		{
			*querytimeout = atoi(defGetString(def));
		}
*/
/*		if (strcmp(def->defname, "jarfile") == 0)
		{
			*jarfile = defGetString(def);
		}
*/
		if (strcmp(def->defname, "maxheapsize") == 0)
		{
			*maxheapsize = atoi(defGetString(def));
		}

		if (strcmp(def->defname, "password") == 0)
		{
			*password = defGetString(def);
		}

		if (strcmp(def->defname, "table") == 0)
		{
			*table = defGetString(def);
		}

                if (strcmp(def->defname, "host") == 0)
                {
                        *host = defGetString(def);
                }
                if (strcmp(def->defname, "port") == 0)
                {
                        *port = atoi(defGetString(def));
                }

	}
}

#if (PG_VERSION_NUM < 90200)
/*
 * hivePlanForeignScan
 *		Create a FdwPlan for a scan on the foreign table
 */
static FdwPlan*
hivePlanForeignScan(Oid foreigntableid, PlannerInfo *root, RelOptInfo *baserel)
{
	FdwPlan 	*fdwplan = NULL;
	char		*svr_drivername = NULL;
	char		*svr_username = NULL;
	char		*svr_password = NULL;
	char 		*svr_query = NULL;
	char 		*svr_table = NULL;
	char 		*svr_url = NULL;
        char    *svr_host = NULL;
        int             svr_port = 0;
	char 		*svr_jarfile = NULL;
	int 		svr_querytimeout = 0;
	int 		svr_maxheapsize = 0;
	char		*query;

	SIGINTInterruptCheckProcess();

	fdwplan = makeNode(FdwPlan);

	JVMInitialization(foreigntableid);

	/* Fetch options */
	hiveGetOptions(
	    foreigntableid, 
	    &svr_host,
	    &svr_port, 
	    &svr_maxheapsize, 
	    &svr_username, 
	    &svr_password, 
	    &svr_table
	);
	/* Build the query */
	if (svr_query)
	{
		size_t len = strlen(svr_query) + 9;

		query = (char *) palloc(len);
		snprintf(query, len, "EXPLAIN %s", svr_query);
	}
	else
	{
		size_t len = strlen(svr_table) + 23;

		query = (char *) palloc(len);
		snprintf(query, len, "EXPLAIN SELECT * FROM %s", svr_table);
	}

	return (fdwplan);
}
#endif

/*
 * hiveExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
hiveExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	char 		    *svr_drivername = NULL;
	char 		    *svr_url = NULL;
        char    *svr_host = NULL;
        int             svr_port = 0;
	char		    *svr_username = NULL;
	char		    *svr_password = NULL;
	char		    *svr_query = NULL;
	char		    *svr_table = NULL;
	char 		    *svr_jarfile = NULL;
	int 		    svr_querytimeout = 0;
	int 		    svr_maxheapsize = 0;

	/* Fetch options  */
	hiveGetOptions(
	    RelationGetRelid(node->ss.ss_currentRelation), 
	    &svr_host,
	    &svr_port,
	    &svr_maxheapsize, 
	    &svr_username, 
	    &svr_password, 
	    &svr_table
	);

	SIGINTInterruptCheckProcess();
}

/*
 * hiveBeginForeignScan
 *		Initiate access to the database
 */
static void
hiveBeginForeignScan(ForeignScanState *node, int eflags)
{
	char 			*svr_drivername = NULL;
	char 			*svr_url = NULL;
        char    		*svr_host = NULL;
        int             	svr_port = 0;
	char               	*portstr=NULL;
	char			*svr_username = NULL;
	char			*svr_password = NULL;
	char			*svr_table = NULL;
	char 			*svr_query = NULL;
	int 			svr_maxheapsize = 0;
	hiveFdwExecutionState   *festate;
	char			*query;
	jclass 			HIVEUtilsClass;
	jclass		 	JavaString;
	jstring 		StringArray[5];
	jstring 		initialize_result = NULL;
	jmethodID		id_initialize;
	jobjectArray		arg_array;
	int 			counter = 0;
	int 			referencedeletecounter = 0;
	jfieldID 		id_numberofcolumns;
	char 			strpkglibdir[] = STR_PKGLIBDIR;
	char 			*initialize_result_cstring = NULL;
	
	SIGINTInterruptCheckProcess();

	/* Fetch options  */
	hiveGetOptions(
	    RelationGetRelid(node->ss.ss_currentRelation), 
            &svr_host,
	    &svr_port, 
	    &svr_maxheapsize, 
	    &svr_username, 
	    &svr_password, 
	    &svr_table
	);

	/* Build the query */
	if (svr_query != NULL)
	{
		query = svr_query;
	}
	else
	{
		size_t len = strlen(svr_table) + 15;

		query = (char *)palloc(len);
		snprintf(query, len, "SELECT * FROM %s", svr_table);
	}

	/* Stash away the state info we have already */
	festate = (hiveFdwExecutionState *) palloc(sizeof(hiveFdwExecutionState));
	festate->query = query;
	festate->NumberOfColumns = 0;
	festate->NumberOfRows = 0;

	/* Connect to the server and execute the query */
	HIVEUtilsClass = (*env)->FindClass(env, "HIVEUtils");
	if (HIVEUtilsClass == NULL) 
	{
		elog(ERROR, "HIVEUtilsClass is NULL");
	}

	id_initialize = (*env)->GetMethodID(env, HIVEUtilsClass, "Initialize", "([Ljava/lang/String;)Ljava/lang/String;");
	if (id_initialize == NULL) 
	{
		elog(ERROR, "id_initialize is NULL");
	}

	id_numberofcolumns = (*env)->GetFieldID(env, HIVEUtilsClass, "NumberOfColumns" , "I");
	if (id_numberofcolumns == NULL)
	{
		elog(ERROR, "id_numberofcolumns is NULL");
	}
	portstr = (char*)palloc(sizeof(int));	
	snprintf(portstr, sizeof(int), "%d", svr_port);
    
	if (svr_username == NULL)
	{
		svr_username = "";
	}

	if (svr_password == NULL)
	{
		svr_password = "";
	}
	
	StringArray[0] = (*env)->NewStringUTF(env, svr_host);
	StringArray[1] = (*env)->NewStringUTF(env, portstr);
	StringArray[2] = (*env)->NewStringUTF(env, svr_username);
	StringArray[3] = (*env)->NewStringUTF(env, svr_password);
        StringArray[4] = (*env)->NewStringUTF(env, svr_table);

	JavaString = (*env)->FindClass(env, "java/lang/String");

	arg_array = (*env)->NewObjectArray(env, 5, JavaString, StringArray[0]);
	if (arg_array == NULL)
	{
		elog(ERROR, "arg_array is NULL");
	}

	for (counter = 1; counter < 5; counter++)
	{		
		(*env)->SetObjectArrayElement(env, arg_array, counter, StringArray[counter]);
	}
	
	java_call = (*env)->AllocObject(env, HIVEUtilsClass);
	if (java_call == NULL)
	{
		elog(ERROR, "java_call is NULL");
	}

	festate->java_call = java_call;

	initialize_result = (*env)->CallObjectMethod(env, java_call, id_initialize, arg_array);
	if (initialize_result != NULL)
	{
		initialize_result_cstring = ConvertStringToCString((jobject)initialize_result);
		elog(ERROR, "%s", initialize_result_cstring);
	}

	node->fdw_state = (void *) festate;
	festate->NumberOfColumns = (*env)->GetIntField(env, java_call, id_numberofcolumns);

	for (referencedeletecounter = 0; referencedeletecounter < 5; referencedeletecounter++)
	{
		(*env)->DeleteLocalRef(env, StringArray[referencedeletecounter]);
	}	
	
	(*env)->DeleteLocalRef(env, arg_array);
	(*env)->ReleaseStringUTFChars(env, initialize_result, initialize_result_cstring);
	(*env)->DeleteLocalRef(env, initialize_result);
}

/*
 * hiveIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot*
hiveIterateForeignScan(ForeignScanState *node)
{
	char 			**values;
	HeapTuple		tuple;
	jmethodID		id_returnresultset;
	jclass 			HIVEUtilsClass;
	jobjectArray 		java_rowarray; 
	int 		        i = 0;
	int 			j = 0;
	jstring 		tempString;
	hiveFdwExecutionState *festate = (hiveFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	jobject 		java_call = festate->java_call;

	/* Cleanup */
	ExecClearTuple(slot);

	SIGINTInterruptCheckProcess();

	if ((*env)->PushLocalFrame(env, (festate->NumberOfColumns + 10)) < 0) 
	{
         /* frame not pushed, no PopLocalFrame needed */
		elog(ERROR, "Error"); 
     	}


	HIVEUtilsClass = (*env)->FindClass(env, "HIVEUtils");
	if (HIVEUtilsClass == NULL) 
	{
		elog(ERROR, "HIVEUtilsClass is NULL");
	}

	id_returnresultset = (*env)->GetMethodID(env, HIVEUtilsClass, "ReturnResultSet", "()[Ljava/lang/String;");
	if (id_returnresultset == NULL)
	{
		elog(ERROR, "id_returnresultset is NULL");
	}
 
	values=(char**)palloc(sizeof(char*)*(festate->NumberOfColumns));
	
	java_rowarray = (*env)->CallObjectMethod(env, java_call, id_returnresultset);

	if (java_rowarray != NULL)
	{
    
		for (i = 0; i < (festate->NumberOfColumns); i++) 
		{
        		values[i] = ConvertStringToCString((jobject)(*env)->GetObjectArrayElement(env, java_rowarray, i));
    		}

		tuple = BuildTupleFromCStrings(TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att), values);
		ExecStoreTuple(tuple, slot, InvalidBuffer, false);
		++ (festate->NumberOfRows);

		for (j = 0; j < festate->NumberOfColumns; j++)
		{
			tempString = (jstring)(*env)->GetObjectArrayElement(env, java_rowarray,j);
			(*env)->ReleaseStringUTFChars(env, tempString, values[j]);
			(*env)->DeleteLocalRef(env, tempString);
		}
	
		(*env)->DeleteLocalRef(env, java_rowarray);
	}

	(*env)->PopLocalFrame(env, NULL);

return (slot);
}

/*
 * hiveEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
hiveEndForeignScan(ForeignScanState *node)
{
	jmethodID 			id_close;
	jclass 				HIVEUtilsClass;
	jstring 			close_result = NULL;
	char 				*close_result_cstring = NULL;
	hiveFdwExecutionState *festate = (hiveFdwExecutionState *) node->fdw_state;
	jobject 			java_call = festate->java_call;

	SIGINTInterruptCheckProcess();

	HIVEUtilsClass = (*env)->FindClass(env, "HIVEUtils");
	if (HIVEUtilsClass == NULL) 
	{
		elog(ERROR, "HIVEUtilsClass is NULL");
	}

	id_close = (*env)->GetMethodID(env, HIVEUtilsClass, "Close", "()Ljava/lang/String;");
	if (id_close == NULL) 
	{
		elog(ERROR, "id_close is NULL");
	}

	close_result = (*env)->CallObjectMethod(env, java_call, id_close);
	if (close_result != NULL)
	{
		close_result_cstring = ConvertStringToCString((jobject)close_result);
		elog(ERROR, "%s", close_result_cstring);
	}
	if (festate->query)
	{
		pfree(festate->query);
		festate->query = 0;
	}
	
	(*env)->ReleaseStringUTFChars(env, close_result, close_result_cstring);
	(*env)->DeleteLocalRef(env, close_result);
	(*env)->DeleteGlobalRef(env, java_call);
}

/*
 * hiveReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
hiveReScanForeignScan(ForeignScanState *node)
{
	SIGINTInterruptCheckProcess();
}

#if (PG_VERSION_NUM >= 90200)
/*
 * hiveGetForeignPaths
 *		(9.2+) Get the foreign paths
 */
static void
hiveGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	Cost 		startup_cost = 0;
	Cost 		total_cost = 0;

	SIGINTInterruptCheckProcess();

	/* Create a ForeignPath node and add it as only possible path */
	add_path(baserel, (Path*)create_foreignscan_path(root, baserel, baserel->rows, startup_cost, total_cost, NIL, NULL, NIL,NULL)); 
}

/*
 * hiveGetForeignPlan
 *		(9.2+) Get a foreign scan plan node
 */
static ForeignScan *
hiveGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid, ForeignPath *best_path, List *tlist, List *scan_clauses)
{
	Index 		scan_relid = baserel->relid;

	SIGINTInterruptCheckProcess();

	JVMInitialization(foreigntableid);

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node */
	return (make_foreignscan(tlist, scan_clauses, scan_relid, NIL, NIL,NULL,NULL,NULL)); 
}

/*
 * hiveGetForeignRelSize
 *		(9.2+) Get a foreign scan plan node
 */
static void
hiveGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	SIGINTInterruptCheckProcess();
}
#endif