/*
 * The Multicorn Foreign Data Wrapper allows you to fetch foreign data in
 * Python in your PostgreSQL server
 *
 * This software is released under the postgresql licence
 *
 * author: Kozea
 */
#include "multicorn.h"
#include "commands/explain.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "access/reloptions.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_type.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "access/relscan.h"


PG_MODULE_MAGIC;


extern Datum multicorn_handler(PG_FUNCTION_ARGS);
extern Datum multicorn_validator(PG_FUNCTION_ARGS);


PG_FUNCTION_INFO_V1(multicorn_handler);
PG_FUNCTION_INFO_V1(multicorn_validator);


void		_PG_init(void);
void		_PG_fini(void);




/*
 * FDW functions declarations
 */
static AttrNumber multicornGetForeignRelWidth(PlannerInfo *root,
							RelOptInfo *baserel,
							Relation foreignrel,
							bool inhparent,
							List *targetList);
static void multicornGetForeignRelSize(PlannerInfo *root,
						   RelOptInfo *baserel,
						   Oid foreigntableid);
static void multicornGetForeignPaths(PlannerInfo *root,
						 RelOptInfo *baserel,
						 Oid foreigntableid);
static ForeignScan *multicornGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
						List *scan_clauses);
static void multicornExplainForeignScan(ForeignScanState *node,
							ExplainState *es);
static void multicornBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *multicornIterateForeignScan(ForeignScanState *node);
static void multicornReScanForeignScan(ForeignScanState *node);
static void multicornEndForeignScan(ForeignScanState *node);
static List *multicornPlanForeignModify(PlannerInfo *root,
						   ModifyTable *plan,
						   Index resultRelation,
						   Plan *subplan);
static void multicornBeginForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *resultRelInfo,
							List *fdw_private,
							Plan *subplan,
							int eflags);
static int multicornExecForeignInsert(ResultRelInfo *resultRelInfo,
						   HeapTuple tuple);
static int multicornExecForeignDelete(ResultRelInfo *resultRelInfo,
						   const char *rowid);
static int multicornExecForeignUpdate(ResultRelInfo *resultRelInfo,
						   const char *rowid,
						   HeapTuple tuple);
static void multicornEndForeignModify(ResultRelInfo *resultRelInfo);


/*	Helpers functions */
void	   *serializePlanState(MulticornPlanState * planstate);
MulticornExecState *initializeExecState(void *internal_plan_state);

void
_PG_init()
{
	Py_Initialize();
}

void
_PG_fini()
{
	Py_Finalize();
}


Datum
multicorn_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdw_routine = makeNode(FdwRoutine);

	/* Plan phase */
	fdw_routine->GetForeignRelSize = multicornGetForeignRelSize;
	fdw_routine->GetForeignPaths = multicornGetForeignPaths;
	fdw_routine->GetForeignPlan = multicornGetForeignPlan;
	fdw_routine->ExplainForeignScan = multicornExplainForeignScan;

	/* Scan phase */
	fdw_routine->BeginForeignScan = multicornBeginForeignScan;
	fdw_routine->IterateForeignScan = multicornIterateForeignScan;
	fdw_routine->ReScanForeignScan = multicornReScanForeignScan;
	fdw_routine->EndForeignScan = multicornEndForeignScan;

	/* Code for 9.3 */
	fdw_routine->GetForeignRelWidth = multicornGetForeignRelWidth;
	/* Writable API */
	fdw_routine->PlanForeignModify = multicornPlanForeignModify;
	fdw_routine->BeginForeignModify = multicornBeginForeignModify;
	fdw_routine->ExecForeignInsert = multicornExecForeignInsert;
	fdw_routine->ExecForeignDelete = multicornExecForeignDelete;
	fdw_routine->ExecForeignUpdate = multicornExecForeignUpdate;
	fdw_routine->EndForeignModify = multicornEndForeignModify;

	PG_RETURN_POINTER(fdw_routine);
}

Datum
multicorn_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	char	   *className = NULL;
	ListCell   *cell;
	PyObject   *p_class;

	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "wrapper") == 0)
		{
			/* Only at server creation can we set the wrapper,	*/
			/* for security issues. */
			if (catalog == ForeignTableRelationId)
			{
				ereport(ERROR, (errmsg("%s", "Cannot set the wrapper class on the table"),
								errhint("%s", "Set it on the server")));
			}
			else
			{
				className = (char *) defGetString(def);
			}
		}
	}
	if (catalog == ForeignServerRelationId)
	{
		if (className == NULL)
		{
			ereport(ERROR, (errmsg("%s", "The wrapper parameter is mandatory, specify a valid class name")));
		}
		/* Try to import the class. */
		p_class = getClassString(className);
		errorCheck();
		Py_DECREF(p_class);
	}
	PG_RETURN_VOID();
}

/*
 * multicornGetForeignRelWidth
 *		Gets the row widths, in number of attributes.
 */
static AttrNumber
multicornGetForeignRelWidth(PlannerInfo *root,
							RelOptInfo *baserel,
							Relation foreignrel,
							bool inhparent,
							List *targetList)
{
	AttrNumber	rv;
	MulticornPlanState *planstate = palloc0(sizeof(MulticornPlanState));

	planstate->fdw_instance = getInstance(foreignrel->rd_id);
	planstate->rowid_attnum = get_pseudo_rowid_column(baserel, targetList);
	if (planstate->rowid_attnum != InvalidAttrNumber)
	{
		/* We need a pseudo-rowid column. */
		/* Ask the python implementation what attribute should be used. */
		rv = planstate->rowid_attnum;
		planstate->rowid_attname = getRowIdColumn(planstate->fdw_instance);
	}
	else
	{
		rv = RelationGetNumberOfAttributes(foreignrel);
	}
	planstate->numattrs = rv;
	baserel->fdw_private = planstate;
	return rv;
}


/*
 * multicornGetForeignRelSize
 *		Obtain relation size estimates for a foreign table.
 *		This is done by calling the
 */
static void
multicornGetForeignRelSize(PlannerInfo *root,
						   RelOptInfo *baserel,
						   Oid foreigntableid)
{
	MulticornPlanState *planstate = baserel->fdw_private;
	ForeignTable *ftable = GetForeignTable(foreigntableid);
	ListCell   *lc;

	planstate->foreigntableid = foreigntableid;
	/* Initialize the conversion info array */
	{
		Relation	rel = RelationIdGetRelation(ftable->relid);
		AttInMetadata *attinmeta = TupleDescGetAttInMetadata(rel->rd_att);

		planstate->cinfos = palloc0(sizeof(ConversionInfo *) *
									planstate->numattrs);
		initConversioninfo(planstate->cinfos, attinmeta,
						   planstate->rowid_attnum, planstate->rowid_attname);
		RelationClose(rel);
	}

	/* Pull "var" clauses to build an appropriate target list */
	foreach(lc, extractColumns(root, baserel))
	{
		Var		   *var = (Var *) lfirst(lc);
		Value	   *colname = colnameFromVar(var, root, planstate);

		/* Store only a Value node containing the string name of the column. */
		if (colname != NULL)
		{
			planstate->target_list = lappend(planstate->target_list, colname);
		}
	}
	foreach(lc, baserel->baserestrictinfo)
	{
		extractRestrictions(root, baserel, (RestrictInfo *) lfirst(lc),
							&planstate->qual_list,
							&planstate->param_list);

	}
	/* Extract the restrictions from the plan. */
	/* Inject the "rows" and "width" attribute into the baserel */
	getRelSize(planstate, root, &baserel->rows, &baserel->width);
}

/*
 * multicornGetForeignPaths
 *		Create possible access paths for a scan on the foreign table.
 *		This is done by calling the "get_path_keys method on the python side,
 *		and parsing its result to build parameterized paths according to the
 *		equivalence classes found in the plan.
 */
static void
multicornGetForeignPaths(PlannerInfo *root,
						 RelOptInfo *baserel,
						 Oid foreigntableid)
{
	Path	   *path;
	MulticornPlanState *planstate = baserel->fdw_private;

	/* Extract a friendly version of the pathkeys. */
	List	   *possiblePaths = pathKeys(planstate);

	findPaths(root, baserel, possiblePaths);
	/* Add a default path */
	path = (Path *) create_foreignscan_path(root, baserel,
											baserel->rows,
											baserel->baserestrictcost.startup,
											baserel->rows * baserel->width,
											NIL,		/* no pathkeys */
											NULL,		/* no outer rel either */
											(void *) baserel->fdw_private);

	add_path(baserel, path);
}

/*
 * multicornGetForeignPlan
 *		Create a ForeignScan plan node for scanning the foreign table
 */
static ForeignScan *
multicornGetForeignPlan(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid,
						ForeignPath *best_path,
						List *tlist,
						List *scan_clauses)
{
	Index		scan_relid = baserel->relid;

	scan_clauses = extract_actual_clauses(scan_clauses, false);
	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							serializePlanState(baserel->fdw_private));
}

/*
 * multicornExplainForeignScan
 *		Placeholder for additional "EXPLAIN" information.
 *		This should (at least) output the python class name, as well
 *		as information that was taken into account for the choice of a path.
 */
static void
multicornExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
}

/*
 *	multicornBeginForeignScan
 *		Initialize the foreign scan.
 *		This (primarily) involves :
 *			- retrieving cached info from the plan phase
 *			- initializing various buffers
 */
static void
multicornBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fscan = (ForeignScan *) node->ss.ps.plan;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	MulticornExecState *execstate;

	execstate = initializeExecState(fscan->fdw_private);
	{
		TupleDesc	tupdesc = slot->tts_tupleDescriptor;

		execstate->values = palloc(sizeof(Datum) * tupdesc->natts);
		execstate->nulls = palloc(sizeof(bool) * tupdesc->natts);
		execstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);
	}
	initConversioninfo(execstate->cinfos, execstate->attinmeta,
					   execstate->rowid_attnum, execstate->rowid_attname);
	node->fdw_state = execstate;
}


/*
 * multicornIterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 *
 *		This is done by iterating over the result from the "execute" python
 *		method.
 */
static TupleTableSlot *
multicornIterateForeignScan(ForeignScanState *node)
{
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	MulticornExecState *execstate = node->fdw_state;
	PyObject   *p_value;

	if (execstate->p_iterator == NULL)
	{
		execute(node);
	}
	ExecClearTuple(slot);
	if (execstate->p_iterator == Py_None)
	{
		/* No iterator returned from get_iterator */
		return slot;
	}
	p_value = PyIter_Next(execstate->p_iterator);
	if (try_except("exceptions.StopIteration"))
	{
		return slot;
	}
	/* A none value results in an empty slot. */
	if (p_value == NULL || p_value == Py_None)
	{
		if (p_value != NULL)
		{
			Py_DECREF(p_value);
		}
		return slot;
	}
	pythonResultToTuple(p_value, execstate);
	slot->tts_values = execstate->values;
	slot->tts_isnull = execstate->nulls;
	ExecStoreVirtualTuple(slot);
	Py_DECREF(p_value);
	return slot;
}

/*
 * multicornReScanForeignScan
 *		Restart the scan
 */
static void
multicornReScanForeignScan(ForeignScanState *node)
{
	MulticornExecState *state = node->fdw_state;

	if (state->p_iterator)
	{
		Py_DECREF(state->p_iterator);
		state->p_iterator = NULL;
	}
}

/*
 *	multicornEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan.
 */
static void
multicornEndForeignScan(ForeignScanState *node)
{
	MulticornExecState *state = node->fdw_state;

	Py_DECREF(state->fdw_instance);
	if (state->p_iterator != NULL)
	{
		Py_DECREF(state->p_iterator);
	}
	state->p_iterator = NULL;
}


/*
 * multicornPlanForeignModify
 *		Plan a foreign write operation.
 *		This is done by checking the "supported operations" attribute
 *		on the python class.
 */
static List *
multicornPlanForeignModify(PlannerInfo *root,
						   ModifyTable *plan,
						   Index resultRelation,
						   Plan *subplan)
{
	MulticornModifyState *modstate = palloc0(sizeof(MulticornModifyState));
	RangeTblEntry *rte = root->simple_rte_array[resultRelation];
	Relation	rel = RelationIdGetRelation(rte->relid);

	modstate->attinmeta = TupleDescGetAttInMetadata(rel->rd_att);
	modstate->cinfos = palloc0(sizeof(ConversionInfo *) *
							   modstate->attinmeta->tupdesc->natts);
	initConversioninfo(modstate->cinfos, modstate->attinmeta,
					   -1, "");
	modstate->fdw_instance = getInstance(rte->relid);
	RelationClose(rel);
	return lappend(NULL, modstate);
}

/*
 * multicornBeginForeignModify
 *		Initialize a foreign write operation.
 */
static void
multicornBeginForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *resultRelInfo,
							List *fdw_private,
							Plan *subplan,
							int eflags)
{
	resultRelInfo->ri_fdw_state = linitial(fdw_private);
}

/*
 * multicornExecForeignInsert
 *		Execute a foreign insert operation
 *		This is done by calling the python "insert" method.
 */
static int
multicornExecForeignInsert(ResultRelInfo *resultRelInfo,
						   HeapTuple tuple)
{
	MulticornModifyState *modstate = resultRelInfo->ri_fdw_state;
	PyObject   *fdw_instance = modstate->fdw_instance;
	PyObject   *values = heapTupleToPyObject(tuple, modstate);

	PyObject_CallMethod(fdw_instance, "insert", "(O)", values);
	errorCheck();
	Py_DECREF(values);
	return 1;
}

/*
 * multicornExecForeignDelete
 *		Execute a foreign delete operation
 *		This is done by calling the python "delete" method, with the opaque
 *		rowid that was supplied.
 */
static int
multicornExecForeignDelete(ResultRelInfo *resultRelInfo,
						   const char *rowid)
{
	MulticornModifyState *modstate = resultRelInfo->ri_fdw_state;
	PyObject   *fdw_instance = modstate->fdw_instance,
			   *p_rowid = deserializeRowId((Datum) rowid);

	PyObject_CallMethod(fdw_instance, "delete", "(O)", p_rowid);
	Py_DECREF(p_rowid);
	errorCheck();
	return 1;
}

/*
 * multicornExecForeignUpdate
 *		Execute a foreign update operation
 *		This is done by calling the python "update" method, with the opaque
 *		rowid that was supplied.
 */
static int
multicornExecForeignUpdate(ResultRelInfo *resultRelInfo,
						   const char *rowid,
						   HeapTuple tuple)
{
	MulticornModifyState *modstate = resultRelInfo->ri_fdw_state;
	PyObject   *fdw_instance = modstate->fdw_instance,
			   *values = heapTupleToPyObject(tuple, modstate),
			   *p_rowid = deserializeRowId((Datum) rowid);

	PyObject_CallMethod(fdw_instance, "update", "(O,O)", p_rowid,
						values);
	Py_DECREF(p_rowid);
	errorCheck();
	Py_DECREF(values);
	return 1;
}

/*
 * multicornEndForeignModify
 *		Clean internal state after a modify operation.
 */
static void
multicornEndForeignModify(ResultRelInfo *resultRelInfo)
{
}


/*
 *	"Serialize" a MulticornPlanState, so that it is safe to be carried
 *	between the plan and the execution safe.
 */
void *
serializePlanState(MulticornPlanState * state)
{
	List	   *result = NULL;

	result = lappend_int(result, state->numattrs);
	result = lappend_int(result, state->foreigntableid);
	result = lappend(result, state->target_list);
	result = lappend(result, state->qual_list);
	result = lappend(result, state->param_list);
	result = lappend_int(result, state->rowid_attnum);
	result = lappend(result, makeString(state->rowid_attname));
	return result;
}

/*
 *	"Deserialize" an internal state and inject it in an
 *	MulticornExecState
 */
MulticornExecState *
initializeExecState(void *internalstate)
{
	MulticornExecState *execstate = palloc0(sizeof(MulticornExecState));
	List	   *values = (List *) internalstate;
	AttrNumber	attnum = linitial_int(values);
	Oid			foreigntableid = lsecond_int(values);

	/* Those list must be copied, because their memory context can become */
	/* invalid during the execution (in particular with the cursor interface) */
	execstate->target_list = copyObject(lthird(values));
	execstate->qual_list = copyObject(lfourth(values));
	execstate->param_list = copyObject(list_nth(values, 4));
	execstate->rowid_attnum = list_nth_int(values, 5);
	execstate->rowid_attname = strVal(list_nth(values, 6));
	execstate->fdw_instance = getInstance(foreigntableid);
	execstate->numattrs = attnum;
	execstate->buffer = makeStringInfo();
	execstate->cinfos = palloc0(sizeof(ConversionInfo *) *
								attnum);
	execstate->values = palloc(attnum * sizeof(Datum));
	execstate->nulls = palloc(execstate->numattrs * sizeof(bool));
	return execstate;
}
