/*-------------------------------------------------------------------------
 *
 * execExprInterp.c
 *	  Interpreted evaluation of an expression step list.
 *
 * This file provides either a "direct threaded" (for gcc, clang and
 * compatible) or a "switch threaded" (for all compilers) implementation of
 * expression evaluation.  The former is amongst the fastest known methods
 * of interpreting programs without resorting to assembly level work, or
 * just-in-time compilation, but it requires support for computed gotos.
 * The latter is amongst the fastest approaches doable in standard C.
 *
 * In either case we use ExprEvalStep->opcode to dispatch to the code block
 * within ExecInterpExpr() that implements the specific opcode type.
 *
 * Switch-threading uses a plain switch() statement to perform the
 * dispatch.  This has the advantages of being plain C and allowing the
 * compiler to warn if implementation of a specific opcode has been forgotten.
 * The disadvantage is that dispatches will, as commonly implemented by
 * compilers, happen from a single location, requiring more jumps and causing
 * bad branch prediction.
 *
 * In direct threading, we use gcc's label-as-values extension - also adopted
 * by some other compilers - to replace ExprEvalStep->opcode with the address
 * of the block implementing the instruction. Dispatch to the next instruction
 * is done by a "computed goto".  This allows for better branch prediction
 * (as the jumps are happening from different locations) and fewer jumps
 * (as no preparatory jump to a common dispatch location is needed).
 *
 * When using direct threading, ExecReadyInterpretedExpr will replace
 * each step's opcode field with the address of the relevant code block and
 * ExprState->flags will contain EEO_FLAG_DIRECT_THREADED to remember that
 * that's been done.
 *
 * For very simple instructions the overhead of the full interpreter
 * "startup", as minimal as it is, is noticeable.  Therefore
 * ExecReadyInterpretedExpr will choose to implement certain simple
 * opcode patterns using special fast-path routines (ExecJust*).
 *
 * Complex or uncommon instructions are not implemented in-line in
 * ExecInterpExpr(), rather we call out to a helper function appearing later
 * in this file.  For one reason, there'd not be a noticeable performance
 * benefit, but more importantly those complex routines are intended to be
 * shared between different expression evaluation approaches.  For instance
 * a JIT compiler would generate calls to them.  (This is why they are
 * exported rather than being "static" in this file.)
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execExprInterp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tuptoaster.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "utils/memutils.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_cypher_expr.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/expandedrecord.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"

/* added for TopMemoryContext */
#include "utils/memutils.h"


/*
 * Use computed-goto-based opcode dispatch when computed gotos are available.
 * But use a separate symbol so that it's easy to adjust locally in this file
 * for development and testing.
 */
#ifdef HAVE_COMPUTED_GOTO
#define EEO_USE_COMPUTED_GOTO
#endif							/* HAVE_COMPUTED_GOTO */

/*
 * Macros for opcode dispatch.
 *
 * EEO_SWITCH - just hides the switch if not in use.
 * EEO_CASE - labels the implementation of named expression step type.
 * EEO_DISPATCH - jump to the implementation of the step type for 'op'.
 * EEO_OPCODE - compute opcode required by used expression evaluation method.
 * EEO_NEXT - increment 'op' and jump to correct next step type.
 * EEO_JUMP - jump to the specified step number within the current expression.
 */
#if defined(EEO_USE_COMPUTED_GOTO)

/* struct for jump target -> opcode lookup table */
typedef struct ExprEvalOpLookup
{
	const void *opcode;
	ExprEvalOp	op;
} ExprEvalOpLookup;

/* to make dispatch_table accessible outside ExecInterpExpr() */
static const void **dispatch_table = NULL;

/* jump target -> opcode lookup table */
static ExprEvalOpLookup reverse_dispatch_table[EEOP_LAST];

#define EEO_SWITCH()
#define EEO_CASE(name)		CASE_##name:
#define EEO_DISPATCH()		goto *((void *) op->opcode)
#define EEO_OPCODE(opcode)	((intptr_t) dispatch_table[opcode])

#else							/* !EEO_USE_COMPUTED_GOTO */

#define EEO_SWITCH()		starteval: switch ((ExprEvalOp) op->opcode)
#define EEO_CASE(name)		case name:
#define EEO_DISPATCH()		goto starteval
#define EEO_OPCODE(opcode)	(opcode)

#endif							/* EEO_USE_COMPUTED_GOTO */

#define EEO_NEXT() \
	do { \
		op++; \
		EEO_DISPATCH(); \
	} while (0)

#define EEO_JUMP(stepno) \
	do { \
		op = &state->steps[stepno]; \
		EEO_DISPATCH(); \
	} while (0)


static Datum ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull);
static void ExecInitInterpreter(void);

/* support functions */
static void CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype);
static TupleDesc get_cached_rowtype(Oid type_id, int32 typmod,
				   TupleDesc *cache_field, ExprContext *econtext);
static void ShutdownTupleDescRef(Datum arg);
static void ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
				   ExprContext *econtext, bool checkisnull);

/* fast-path evaluation functions */
static Datum ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustApplyFuncToCase(ExprState *state, ExprContext *econtext, bool *isnull);

static JsonbValue *cypher_access_object(JsonbContainer *container,
										CypherAccessPathElem *pathelem);
static JsonbValue *cypher_access_bin_array(JsonbValue *ajv,
										   CypherAccessPathElem *pathelem);
static JsonbValue *cypher_access_mem_array(JsonbValue *ajv,
										   CypherAccessPathElem *pathelem);
static int cypher_access_range(CypherIndexResult *cidxres, const int nelems,
							   const int defidx);
static int cypher_access_index(CypherIndexResult *cidxres, const int nelems);
static Datum get_numeric_0_datum(void);

/*
 * Prepare ExprState for interpreted execution.
 */
void
ExecReadyInterpretedExpr(ExprState *state)
{
	/* Ensure one-time interpreter setup has been done */
	ExecInitInterpreter();

	/* Simple validity checks on expression */
	Assert(state->steps_len >= 1);
	Assert(state->steps[state->steps_len - 1].opcode == EEOP_DONE);

	/*
	 * Don't perform redundant initialization. This is unreachable in current
	 * cases, but might be hit if there's additional expression evaluation
	 * methods that rely on interpreted execution to work.
	 */
	if (state->flags & EEO_FLAG_INTERPRETER_INITIALIZED)
		return;

	/*
	 * First time through, check whether attribute matches Var.  Might not be
	 * ok anymore, due to schema changes. We do that by setting up a callback
	 * that does checking on the first call, which then sets the evalfunc
	 * callback to the actual method of execution.
	 */
	state->evalfunc = ExecInterpExprStillValid;

	/* DIRECT_THREADED should not already be set */
	Assert((state->flags & EEO_FLAG_DIRECT_THREADED) == 0);

	/*
	 * There shouldn't be any errors before the expression is fully
	 * initialized, and even if so, it'd lead to the expression being
	 * abandoned.  So we can set the flag now and save some code.
	 */
	state->flags |= EEO_FLAG_INTERPRETER_INITIALIZED;

	/*
	 * Select fast-path evalfuncs for very simple expressions.  "Starting up"
	 * the full interpreter is a measurable overhead for these, and these
	 * patterns occur often enough to be worth optimizing.
	 */
	if (state->steps_len == 3)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;
		ExprEvalOp	step1 = state->steps[1].opcode;

		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustScanVar;
			return;
		}
		else if (step0 == EEOP_INNER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = (void *) ExecJustAssignScanVar;
			return;
		}
		else if (step0 == EEOP_CASE_TESTVAL &&
				 step1 == EEOP_FUNCEXPR_STRICT &&
				 state->steps[0].d.casetest.value)
		{
			state->evalfunc_private = (void *) ExecJustApplyFuncToCase;
			return;
		}
	}
	else if (state->steps_len == 2 &&
			 state->steps[0].opcode == EEOP_CONST)
	{
		state->evalfunc_private = (void *) ExecJustConst;
		return;
	}

#if defined(EEO_USE_COMPUTED_GOTO)

	/*
	 * In the direct-threaded implementation, replace each opcode with the
	 * address to jump to.  (Use ExecEvalStepOp() to get back the opcode.)
	 */
	{
		int			off;

		for (off = 0; off < state->steps_len; off++)
		{
			ExprEvalStep *op = &state->steps[off];

			op->opcode = EEO_OPCODE(op->opcode);
		}

		state->flags |= EEO_FLAG_DIRECT_THREADED;
	}
#endif							/* EEO_USE_COMPUTED_GOTO */

	state->evalfunc_private = (void *) ExecInterpExpr;
}


/*
 * Evaluate expression identified by "state" in the execution context
 * given by "econtext".  *isnull is set to the is-null flag for the result,
 * and the Datum value is the function result.
 *
 * As a special case, return the dispatch table's address if state is NULL.
 * This is used by ExecInitInterpreter to set up the dispatch_table global.
 * (Only applies when EEO_USE_COMPUTED_GOTO is defined.)
 */
static Datum
ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op;
	TupleTableSlot *resultslot;
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;

	/*
	 * This array has to be in the same order as enum ExprEvalOp.
	 */
#if defined(EEO_USE_COMPUTED_GOTO)
	static const void *const dispatch_table[] = {
		&&CASE_EEOP_DONE,
		&&CASE_EEOP_INNER_FETCHSOME,
		&&CASE_EEOP_OUTER_FETCHSOME,
		&&CASE_EEOP_SCAN_FETCHSOME,
		&&CASE_EEOP_INNER_VAR,
		&&CASE_EEOP_OUTER_VAR,
		&&CASE_EEOP_SCAN_VAR,
		&&CASE_EEOP_INNER_SYSVAR,
		&&CASE_EEOP_OUTER_SYSVAR,
		&&CASE_EEOP_SCAN_SYSVAR,
		&&CASE_EEOP_WHOLEROW,
		&&CASE_EEOP_ASSIGN_INNER_VAR,
		&&CASE_EEOP_ASSIGN_OUTER_VAR,
		&&CASE_EEOP_ASSIGN_SCAN_VAR,
		&&CASE_EEOP_ASSIGN_TMP,
		&&CASE_EEOP_ASSIGN_TMP_MAKE_RO,
		&&CASE_EEOP_CONST,
		&&CASE_EEOP_FUNCEXPR,
		&&CASE_EEOP_FUNCEXPR_STRICT,
		&&CASE_EEOP_FUNCEXPR_FUSAGE,
		&&CASE_EEOP_FUNCEXPR_STRICT_FUSAGE,
		&&CASE_EEOP_BOOL_AND_STEP_FIRST,
		&&CASE_EEOP_BOOL_AND_STEP,
		&&CASE_EEOP_BOOL_AND_STEP_LAST,
		&&CASE_EEOP_BOOL_OR_STEP_FIRST,
		&&CASE_EEOP_BOOL_OR_STEP,
		&&CASE_EEOP_BOOL_OR_STEP_LAST,
		&&CASE_EEOP_BOOL_NOT_STEP,
		&&CASE_EEOP_QUAL,
		&&CASE_EEOP_JUMP,
		&&CASE_EEOP_JUMP_IF_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_TRUE,
		&&CASE_EEOP_NULLTEST_ISNULL,
		&&CASE_EEOP_NULLTEST_ISNOTNULL,
		&&CASE_EEOP_NULLTEST_ROWISNULL,
		&&CASE_EEOP_NULLTEST_ROWISNOTNULL,
		&&CASE_EEOP_BOOLTEST_IS_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_FALSE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_FALSE,
		&&CASE_EEOP_PARAM_EXEC,
		&&CASE_EEOP_PARAM_EXTERN,
		&&CASE_EEOP_PARAM_CALLBACK,
		&&CASE_EEOP_CASE_TESTVAL,
		&&CASE_EEOP_MAKE_READONLY,
		&&CASE_EEOP_IOCOERCE,
		&&CASE_EEOP_DISTINCT,
		&&CASE_EEOP_NOT_DISTINCT,
		&&CASE_EEOP_NULLIF,
		&&CASE_EEOP_SQLVALUEFUNCTION,
		&&CASE_EEOP_CURRENTOFEXPR,
		&&CASE_EEOP_NEXTVALUEEXPR,
		&&CASE_EEOP_ARRAYEXPR,
		&&CASE_EEOP_ARRAYCOERCE,
		&&CASE_EEOP_ROW,
		&&CASE_EEOP_ROWCOMPARE_STEP,
		&&CASE_EEOP_ROWCOMPARE_FINAL,
		&&CASE_EEOP_MINMAX,
		&&CASE_EEOP_FIELDSELECT,
		&&CASE_EEOP_FIELDSTORE_DEFORM,
		&&CASE_EEOP_FIELDSTORE_FORM,
		&&CASE_EEOP_ARRAYREF_SUBSCRIPT,
		&&CASE_EEOP_ARRAYREF_OLD,
		&&CASE_EEOP_ARRAYREF_ASSIGN,
		&&CASE_EEOP_ARRAYREF_FETCH,
		&&CASE_EEOP_DOMAIN_TESTVAL,
		&&CASE_EEOP_DOMAIN_NOTNULL,
		&&CASE_EEOP_DOMAIN_CHECK,
		&&CASE_EEOP_CONVERT_ROWTYPE,
		&&CASE_EEOP_SCALARARRAYOP,
		&&CASE_EEOP_XMLEXPR,
		&&CASE_EEOP_AGGREF,
		&&CASE_EEOP_GROUPING_FUNC,
		&&CASE_EEOP_WINDOW_FUNC,
		&&CASE_EEOP_SUBPLAN,
		&&CASE_EEOP_ALTERNATIVE_SUBPLAN,
		&&CASE_EEOP_AGG_STRICT_DESERIALIZE,
		&&CASE_EEOP_AGG_DESERIALIZE,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK,
		&&CASE_EEOP_AGG_INIT_TRANS,
		&&CASE_EEOP_AGG_STRICT_TRANS_CHECK,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS,
		&&CASE_EEOP_AGG_ORDERED_TRANS_DATUM,
		&&CASE_EEOP_AGG_ORDERED_TRANS_TUPLE,
		&&CASE_EEOP_CYPHERTYPECAST,
		&&CASE_EEOP_CYPHERMAPEXPR,
		&&CASE_EEOP_CYPHERLISTEXPR,
		&&CASE_EEOP_CYPHERLISTCOMP_BEGIN,
		&&CASE_EEOP_CYPHERLISTCOMP_ELEM,
		&&CASE_EEOP_CYPHERLISTCOMP_END,
		&&CASE_EEOP_CYPHERLISTCOMP_ITER_INIT,
		&&CASE_EEOP_CYPHERLISTCOMP_ITER_NEXT,
		&&CASE_EEOP_CYPHERLISTCOMP_VAR,
		&&CASE_EEOP_CYPHERACCESSEXPR,
		&&CASE_EEOP_LAST
	};

	StaticAssertStmt(EEOP_LAST + 1 == lengthof(dispatch_table),
					 "dispatch_table out of whack with ExprEvalOp");

	if (unlikely(state == NULL))
		return PointerGetDatum(dispatch_table);
#else
	Assert(state != NULL);
#endif							/* EEO_USE_COMPUTED_GOTO */

	/* setup state */
	op = state->steps;
	resultslot = state->resultslot;
	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;

#if defined(EEO_USE_COMPUTED_GOTO)
	EEO_DISPATCH();
#endif

	EEO_SWITCH()
	{
		EEO_CASE(EEOP_DONE)
		{
			goto out;
		}

		EEO_CASE(EEOP_INNER_FETCHSOME)
		{
			/* XXX: worthwhile to check tts_nvalid inline first? */
			slot_getsomeattrs(innerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_FETCHSOME)
		{
			slot_getsomeattrs(outerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_FETCHSOME)
		{
			slot_getsomeattrs(scanslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_VAR)
		{
			int			attnum = op->d.var.attnum;

			/*
			 * Since we already extracted all referenced columns from the
			 * tuple with a FETCHSOME step, we can just grab the value
			 * directly out of the slot's decomposed-data arrays.  But let's
			 * have an Assert to check that that did happen.
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			*op->resvalue = innerslot->tts_values[attnum];
			*op->resnull = innerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			*op->resvalue = outerslot->tts_values[attnum];
			*op->resnull = outerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			*op->resvalue = scanslot->tts_values[attnum];
			*op->resnull = scanslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_SYSVAR)
		{
			int			attnum = op->d.var.attnum;
			Datum		d;

			/* these asserts must match defenses in slot_getattr */
			Assert(innerslot->tts_tuple != NULL);
			Assert(innerslot->tts_tuple != &(innerslot->tts_minhdr));

			/* heap_getsysattr has sufficient defenses against bad attnums */
			d = heap_getsysattr(innerslot->tts_tuple, attnum,
								innerslot->tts_tupleDescriptor,
								op->resnull);
			*op->resvalue = d;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_SYSVAR)
		{
			int			attnum = op->d.var.attnum;
			Datum		d;

			/* these asserts must match defenses in slot_getattr */
			Assert(outerslot->tts_tuple != NULL);
			Assert(outerslot->tts_tuple != &(outerslot->tts_minhdr));

			/* heap_getsysattr has sufficient defenses against bad attnums */
			d = heap_getsysattr(outerslot->tts_tuple, attnum,
								outerslot->tts_tupleDescriptor,
								op->resnull);
			*op->resvalue = d;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_SYSVAR)
		{
			int			attnum = op->d.var.attnum;
			Datum		d;

			/* these asserts must match defenses in slot_getattr */
			Assert(scanslot->tts_tuple != NULL);
			Assert(scanslot->tts_tuple != &(scanslot->tts_minhdr));

			/* heap_getsysattr has sufficient defenses against bad attnums */
			d = heap_getsysattr(scanslot->tts_tuple, attnum,
								scanslot->tts_tupleDescriptor,
								op->resnull);
			*op->resvalue = d;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_WHOLEROW)
		{
			/* too complex for an inline implementation */
			ExecEvalWholeRowVar(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_INNER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			resultslot->tts_values[resultnum] = innerslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = innerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_OUTER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			resultslot->tts_values[resultnum] = outerslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = outerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_SCAN_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			resultslot->tts_values[resultnum] = scanslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = scanslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP)
		{
			int			resultnum = op->d.assign_tmp.resultnum;

			resultslot->tts_values[resultnum] = state->resvalue;
			resultslot->tts_isnull[resultnum] = state->resnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP_MAKE_RO)
		{
			int			resultnum = op->d.assign_tmp.resultnum;

			resultslot->tts_isnull[resultnum] = state->resnull;
			if (!resultslot->tts_isnull[resultnum])
				resultslot->tts_values[resultnum] =
					MakeExpandedObjectReadOnlyInternal(state->resvalue);
			else
				resultslot->tts_values[resultnum] = state->resvalue;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONST)
		{
			*op->resnull = op->d.constval.isnull;
			*op->resvalue = op->d.constval.value;

			EEO_NEXT();
		}

		/*
		 * Function-call implementations. Arguments have previously been
		 * evaluated directly into fcinfo->args.
		 *
		 * As both STRICT checks and function-usage are noticeable performance
		 * wise, and function calls are a very hot-path (they also back
		 * operators!), it's worth having so many separate opcodes.
		 *
		 * Note: the reason for using a temporary variable "d", here and in
		 * other places, is that some compilers think "*op->resvalue = f();"
		 * requires them to evaluate op->resvalue into a register before
		 * calling f(), just in case f() is able to modify op->resvalue
		 * somehow.  The extra line of code can save a useless register spill
		 * and reload across the function call.
		 */
		EEO_CASE(EEOP_FUNCEXPR)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			Datum		d;

			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			*op->resvalue = d;
			*op->resnull = fcinfo->isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			bool	   *argnull = fcinfo->argnull;
			int			argno;
			Datum		d;

			/* strict function, so check for NULL args */
			for (argno = 0; argno < op->d.func.nargs; argno++)
			{
				if (argnull[argno])
				{
					*op->resnull = true;
					goto strictfail;
				}
			}
			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			*op->resvalue = d;
			*op->resnull = fcinfo->isnull;

	strictfail:
			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_FUSAGE)
		{
			/* not common enough to inline */
			ExecEvalFuncExprFusage(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT_FUSAGE)
		{
			/* not common enough to inline */
			ExecEvalFuncExprStrictFusage(state, op, econtext);

			EEO_NEXT();
		}

		/*
		 * If any of its clauses is FALSE, an AND's result is FALSE regardless
		 * of the states of the rest of the clauses, so we can stop evaluating
		 * and return FALSE immediately.  If none are FALSE and one or more is
		 * NULL, we return NULL; otherwise we return TRUE.  This makes sense
		 * when you interpret NULL as "don't know": perhaps one of the "don't
		 * knows" would have been FALSE if we'd known its value.  Only when
		 * all the inputs are known to be TRUE can we state confidently that
		 * the AND's result is TRUE.
		 */
		EEO_CASE(EEOP_BOOL_AND_STEP_FIRST)
		{
			*op->d.boolexpr.anynull = false;

			/*
			 * EEOP_BOOL_AND_STEP_FIRST resets anynull, otherwise it's the
			 * same as EEOP_BOOL_AND_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_AND_STEP)
		{
			if (*op->resnull)
			{
				*op->d.boolexpr.anynull = true;
			}
			else if (!DatumGetBool(*op->resvalue))
			{
				/* result is already set to FALSE, need not change it */
				/* bail out early */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_AND_STEP_LAST)
		{
			if (*op->resnull)
			{
				/* result is already set to NULL, need not change it */
			}
			else if (!DatumGetBool(*op->resvalue))
			{
				/* result is already set to FALSE, need not change it */

				/*
				 * No point jumping early to jumpdone - would be same target
				 * (as this is the last argument to the AND expression),
				 * except more expensive.
				 */
			}
			else if (*op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			else
			{
				/* result is already set to TRUE, need not change it */
			}

			EEO_NEXT();
		}

		/*
		 * If any of its clauses is TRUE, an OR's result is TRUE regardless of
		 * the states of the rest of the clauses, so we can stop evaluating
		 * and return TRUE immediately.  If none are TRUE and one or more is
		 * NULL, we return NULL; otherwise we return FALSE.  This makes sense
		 * when you interpret NULL as "don't know": perhaps one of the "don't
		 * knows" would have been TRUE if we'd known its value.  Only when all
		 * the inputs are known to be FALSE can we state confidently that the
		 * OR's result is FALSE.
		 */
		EEO_CASE(EEOP_BOOL_OR_STEP_FIRST)
		{
			*op->d.boolexpr.anynull = false;

			/*
			 * EEOP_BOOL_OR_STEP_FIRST resets anynull, otherwise it's the same
			 * as EEOP_BOOL_OR_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_OR_STEP)
		{
			if (*op->resnull)
			{
				*op->d.boolexpr.anynull = true;
			}
			else if (DatumGetBool(*op->resvalue))
			{
				/* result is already set to TRUE, need not change it */
				/* bail out early */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_OR_STEP_LAST)
		{
			if (*op->resnull)
			{
				/* result is already set to NULL, need not change it */
			}
			else if (DatumGetBool(*op->resvalue))
			{
				/* result is already set to TRUE, need not change it */

				/*
				 * No point jumping to jumpdone - would be same target (as
				 * this is the last argument to the AND expression), except
				 * more expensive.
				 */
			}
			else if (*op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			else
			{
				/* result is already set to FALSE, need not change it */
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_NOT_STEP)
		{
			/*
			 * Evaluation of 'not' is simple... if expr is false, then return
			 * 'true' and vice versa.  It's safe to do this even on a
			 * nominally null value, so we ignore resnull; that means that
			 * NULL in produces NULL out, which is what we want.
			 */
			*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_QUAL)
		{
			/* simplified version of BOOL_AND_STEP for use by ExecQual() */

			/* If argument (also result) is false or null ... */
			if (*op->resnull ||
				!DatumGetBool(*op->resvalue))
			{
				/* ... bail out early, returning FALSE */
				*op->resnull = false;
				*op->resvalue = BoolGetDatum(false);
				EEO_JUMP(op->d.qualexpr.jumpdone);
			}

			/*
			 * Otherwise, leave the TRUE value in place, in case this is the
			 * last qual.  Then, TRUE is the correct answer.
			 */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP)
		{
			/* Unconditionally jump to target step */
			EEO_JUMP(op->d.jump.jumpdone);
		}

		EEO_CASE(EEOP_JUMP_IF_NULL)
		{
			/* Transfer control if current result is null */
			if (*op->resnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_NULL)
		{
			/* Transfer control if current result is non-null */
			if (!*op->resnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_TRUE)
		{
			/* Transfer control if current result is null or false */
			if (*op->resnull || !DatumGetBool(*op->resvalue))
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNULL)
		{
			*op->resvalue = BoolGetDatum(*op->resnull);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNOTNULL)
		{
			*op->resvalue = BoolGetDatum(!*op->resnull);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNULL)
		{
			/* out of line implementation: too large */
			ExecEvalRowNull(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNOTNULL)
		{
			/* out of line implementation: too large */
			ExecEvalRowNotNull(state, op, econtext);

			EEO_NEXT();
		}

		/* BooleanTest implementations for all booltesttypes */

		EEO_CASE(EEOP_BOOLTEST_IS_TRUE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			/* else, input value is the correct output as well */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_TRUE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_FALSE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_FALSE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			/* else, input value is the correct output as well */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXEC)
		{
			/* out of line implementation: too large */
			ExecEvalParamExec(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXTERN)
		{
			/* out of line implementation: too large */
			ExecEvalParamExtern(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_CALLBACK)
		{
			/* allow an extension module to supply a PARAM_EXTERN value */
			op->d.cparam.paramfunc(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_CASE_TESTVAL)
		{
			/*
			 * Normally upper parts of the expression tree have setup the
			 * values to be returned here, but some parts of the system
			 * currently misuse {caseValue,domainValue}_{datum,isNull} to set
			 * run-time data.  So if no values have been set-up, use
			 * ExprContext's.  This isn't pretty, but also not *that* ugly,
			 * and this is unlikely to be performance sensitive enough to
			 * worry about an extra branch.
			 */
			if (op->d.casetest.value)
			{
				*op->resvalue = *op->d.casetest.value;
				*op->resnull = *op->d.casetest.isnull;
			}
			else
			{
				*op->resvalue = econtext->caseValue_datum;
				*op->resnull = econtext->caseValue_isNull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_TESTVAL)
		{
			/*
			 * See EEOP_CASE_TESTVAL comment.
			 */
			if (op->d.casetest.value)
			{
				*op->resvalue = *op->d.casetest.value;
				*op->resnull = *op->d.casetest.isnull;
			}
			else
			{
				*op->resvalue = econtext->domainValue_datum;
				*op->resnull = econtext->domainValue_isNull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MAKE_READONLY)
		{
			/*
			 * Force a varlena value that might be read multiple times to R/O
			 */
			if (!*op->d.make_readonly.isnull)
				*op->resvalue =
					MakeExpandedObjectReadOnlyInternal(*op->d.make_readonly.value);
			*op->resnull = *op->d.make_readonly.isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_IOCOERCE)
		{
			/*
			 * Evaluate a CoerceViaIO node.  This can be quite a hot path, so
			 * inline as much work as possible.  The source value is in our
			 * result variable.
			 */
			char	   *str;

			/* call output function (similar to OutputFunctionCall) */
			if (*op->resnull)
			{
				/* output functions are not called on nulls */
				str = NULL;
			}
			else
			{
				FunctionCallInfo fcinfo_out;

				fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				fcinfo_out->arg[0] = *op->resvalue;
				fcinfo_out->argnull[0] = false;

				fcinfo_out->isnull = false;
				str = DatumGetCString(FunctionCallInvoke(fcinfo_out));

				/* OutputFunctionCall assumes result isn't null */
				Assert(!fcinfo_out->isnull);
			}

			/* call input function (similar to InputFunctionCall) */
			if (!op->d.iocoerce.finfo_in->fn_strict || str != NULL)
			{
				FunctionCallInfo fcinfo_in;
				Datum		d;

				fcinfo_in = op->d.iocoerce.fcinfo_data_in;
				fcinfo_in->arg[0] = PointerGetDatum(str);
				fcinfo_in->argnull[0] = *op->resnull;
				/* second and third arguments are already set up */

				fcinfo_in->isnull = false;
				d = FunctionCallInvoke(fcinfo_in);
				*op->resvalue = d;

				/* Should get null result if and only if str is NULL */
				if (str == NULL)
				{
					Assert(*op->resnull);
					Assert(fcinfo_in->isnull);
				}
				else
				{
					Assert(!*op->resnull);
					Assert(!fcinfo_in->isnull);
				}
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DISTINCT)
		{
			/*
			 * IS DISTINCT FROM must evaluate arguments (already done into
			 * fcinfo->arg/argnull) to determine whether they are NULL; if
			 * either is NULL then the result is determined.  If neither is
			 * NULL, then proceed to evaluate the comparison function, which
			 * is just the type's standard equality operator.  We need not
			 * care whether that function is strict.  Because the handling of
			 * nulls is different, we can't just reuse EEOP_FUNCEXPR.
			 */
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			/* check function arguments for NULLness */
			if (fcinfo->argnull[0] && fcinfo->argnull[1])
			{
				/* Both NULL? Then is not distinct... */
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else if (fcinfo->argnull[0] || fcinfo->argnull[1])
			{
				/* Only one is NULL? Then is distinct... */
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
			{
				/* Neither null, so apply the equality function */
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				/* Must invert result of "="; safe to do even if null */
				*op->resvalue = BoolGetDatum(!DatumGetBool(eqresult));
				*op->resnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		/* see EEOP_DISTINCT for comments, this is just inverted */
		EEO_CASE(EEOP_NOT_DISTINCT)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			if (fcinfo->argnull[0] && fcinfo->argnull[1])
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else if (fcinfo->argnull[0] || fcinfo->argnull[1])
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
			{
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				*op->resvalue = eqresult;
				*op->resnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLIF)
		{
			/*
			 * The arguments are already evaluated into fcinfo->arg/argnull.
			 */
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			/* if either argument is NULL they can't be equal */
			if (!fcinfo->argnull[0] && !fcinfo->argnull[1])
			{
				Datum		result;

				fcinfo->isnull = false;
				result = op->d.func.fn_addr(fcinfo);

				/* if the arguments are equal return null */
				if (!fcinfo->isnull && DatumGetBool(result))
				{
					*op->resvalue = (Datum) 0;
					*op->resnull = true;

					EEO_NEXT();
				}
			}

			/* Arguments aren't equal, so return the first one */
			*op->resvalue = fcinfo->arg[0];
			*op->resnull = fcinfo->argnull[0];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SQLVALUEFUNCTION)
		{
			/*
			 * Doesn't seem worthwhile to have an inline implementation
			 * efficiency-wise.
			 */
			ExecEvalSQLValueFunction(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CURRENTOFEXPR)
		{
			/* error invocation uses space, and shouldn't ever occur */
			ExecEvalCurrentOfExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEXTVALUEEXPR)
		{
			/*
			 * Doesn't seem worthwhile to have an inline implementation
			 * efficiency-wise.
			 */
			ExecEvalNextValueExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYEXPR)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYCOERCE)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayCoerce(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROW)
		{
			/* too complex for an inline implementation */
			ExecEvalRow(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_STEP)
		{
			FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
			Datum		d;

			/* force NULL result if strict fn and NULL input */
			if (op->d.rowcompare_step.finfo->fn_strict &&
				(fcinfo->argnull[0] || fcinfo->argnull[1]))
			{
				*op->resnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}

			/* Apply comparison function */
			fcinfo->isnull = false;
			d = op->d.rowcompare_step.fn_addr(fcinfo);
			*op->resvalue = d;

			/* force NULL result if NULL function result */
			if (fcinfo->isnull)
			{
				*op->resnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}
			*op->resnull = false;

			/* If unequal, no need to compare remaining columns */
			if (DatumGetInt32(*op->resvalue) != 0)
			{
				EEO_JUMP(op->d.rowcompare_step.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_FINAL)
		{
			int32		cmpresult = DatumGetInt32(*op->resvalue);
			RowCompareType rctype = op->d.rowcompare_final.rctype;

			*op->resnull = false;
			switch (rctype)
			{
					/* EQ and NE cases aren't allowed here */
				case ROWCOMPARE_LT:
					*op->resvalue = BoolGetDatum(cmpresult < 0);
					break;
				case ROWCOMPARE_LE:
					*op->resvalue = BoolGetDatum(cmpresult <= 0);
					break;
				case ROWCOMPARE_GE:
					*op->resvalue = BoolGetDatum(cmpresult >= 0);
					break;
				case ROWCOMPARE_GT:
					*op->resvalue = BoolGetDatum(cmpresult > 0);
					break;
				default:
					Assert(false);
					break;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MINMAX)
		{
			/* too complex for an inline implementation */
			ExecEvalMinMax(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSELECT)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldSelect(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_DEFORM)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldStoreDeForm(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_FORM)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldStoreForm(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYREF_SUBSCRIPT)
		{
			/* Process an array subscript */

			/* too complex for an inline implementation */
			if (ExecEvalArrayRefSubscript(state, op))
			{
				EEO_NEXT();
			}
			else
			{
				/* Subscript is null, short-circuit ArrayRef to NULL */
				EEO_JUMP(op->d.arrayref_subscript.jumpdone);
			}
		}

		EEO_CASE(EEOP_ARRAYREF_OLD)
		{
			/*
			 * Fetch the old value in an arrayref assignment, in case it's
			 * referenced (via a CaseTestExpr) inside the assignment
			 * expression.
			 */

			/* too complex for an inline implementation */
			ExecEvalArrayRefOld(state, op);

			EEO_NEXT();
		}

		/*
		 * Perform ArrayRef assignment
		 */
		EEO_CASE(EEOP_ARRAYREF_ASSIGN)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayRefAssign(state, op);

			EEO_NEXT();
		}

		/*
		 * Fetch subset of an array.
		 */
		EEO_CASE(EEOP_ARRAYREF_FETCH)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayRefFetch(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONVERT_ROWTYPE)
		{
			/* too complex for an inline implementation */
			ExecEvalConvertRowtype(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCALARARRAYOP)
		{
			/* too complex for an inline implementation */
			ExecEvalScalarArrayOp(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_NOTNULL)
		{
			/* too complex for an inline implementation */
			ExecEvalConstraintNotNull(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_CHECK)
		{
			/* too complex for an inline implementation */
			ExecEvalConstraintCheck(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_XMLEXPR)
		{
			/* too complex for an inline implementation */
			ExecEvalXmlExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGGREF)
		{
			/*
			 * Returns a Datum whose value is the precomputed aggregate value
			 * found in the given expression context.
			 */
			AggrefExprState *aggref = op->d.aggref.astate;

			Assert(econtext->ecxt_aggvalues != NULL);

			*op->resvalue = econtext->ecxt_aggvalues[aggref->aggno];
			*op->resnull = econtext->ecxt_aggnulls[aggref->aggno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_GROUPING_FUNC)
		{
			/* too complex/uncommon for an inline implementation */
			ExecEvalGroupingFunc(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_WINDOW_FUNC)
		{
			/*
			 * Like Aggref, just return a precomputed value from the econtext.
			 */
			WindowFuncExprState *wfunc = op->d.window_func.wfstate;

			Assert(econtext->ecxt_aggvalues != NULL);

			*op->resvalue = econtext->ecxt_aggvalues[wfunc->wfuncno];
			*op->resnull = econtext->ecxt_aggnulls[wfunc->wfuncno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SUBPLAN)
		{
			/* too complex for an inline implementation */
			ExecEvalSubPlan(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ALTERNATIVE_SUBPLAN)
		{
			/* too complex for an inline implementation */
			ExecEvalAlternativeSubPlan(state, op, econtext);

			EEO_NEXT();
		}

		/* evaluate a strict aggregate deserialization function */
		EEO_CASE(EEOP_AGG_STRICT_DESERIALIZE)
		{
			bool	   *argnull = op->d.agg_deserialize.fcinfo_data->argnull;

			/* Don't call a strict deserialization function with NULL input */
			if (argnull[0])
				EEO_JUMP(op->d.agg_deserialize.jumpnull);

			/* fallthrough */
		}

		/* evaluate aggregate deserialization function (non-strict portion) */
		EEO_CASE(EEOP_AGG_DESERIALIZE)
		{
			FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
			AggState   *aggstate = op->d.agg_deserialize.aggstate;
			MemoryContext oldContext;

			/*
			 * We run the deserialization functions in per-input-tuple memory
			 * context.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
			fcinfo->isnull = false;
			*op->resvalue = FunctionCallInvoke(fcinfo);
			*op->resnull = fcinfo->isnull;
			MemoryContextSwitchTo(oldContext);

			EEO_NEXT();
		}

		/*
		 * Check that a strict aggregate transition / combination function's
		 * input is not NULL.
		 */
		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK)
		{
			int			argno;
			bool	   *nulls = op->d.agg_strict_input_check.nulls;
			int			nargs = op->d.agg_strict_input_check.nargs;

			for (argno = 0; argno < nargs; argno++)
			{
				if (nulls[argno])
					EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			}
			EEO_NEXT();
		}

		/*
		 * Initialize an aggregate's first value if necessary.
		 */
		EEO_CASE(EEOP_AGG_INIT_TRANS)
		{
			AggState   *aggstate;
			AggStatePerGroup pergroup;

			aggstate = op->d.agg_init_trans.aggstate;
			pergroup = &aggstate->all_pergroups
				[op->d.agg_init_trans.setoff]
				[op->d.agg_init_trans.transno];

			/* If transValue has not yet been initialized, do so now. */
			if (pergroup->noTransValue)
			{
				AggStatePerTrans pertrans = op->d.agg_init_trans.pertrans;

				aggstate->curaggcontext = op->d.agg_init_trans.aggcontext;
				aggstate->current_set = op->d.agg_init_trans.setno;

				ExecAggInitGroup(aggstate, pertrans, pergroup);

				/* copied trans value from input, done this round */
				EEO_JUMP(op->d.agg_init_trans.jumpnull);
			}

			EEO_NEXT();
		}

		/* check that a strict aggregate's input isn't NULL */
		EEO_CASE(EEOP_AGG_STRICT_TRANS_CHECK)
		{
			AggState   *aggstate;
			AggStatePerGroup pergroup;

			aggstate = op->d.agg_strict_trans_check.aggstate;
			pergroup = &aggstate->all_pergroups
				[op->d.agg_strict_trans_check.setoff]
				[op->d.agg_strict_trans_check.transno];

			if (unlikely(pergroup->transValueIsNull))
				EEO_JUMP(op->d.agg_strict_trans_check.jumpnull);

			EEO_NEXT();
		}

		/*
		 * Evaluate aggregate transition / combine function that has a
		 * by-value transition type. That's a seperate case from the
		 * by-reference implementation because it's a bit simpler.
		 */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYVAL)
		{
			AggState   *aggstate;
			AggStatePerTrans pertrans;
			AggStatePerGroup pergroup;
			FunctionCallInfo fcinfo;
			MemoryContext oldContext;
			Datum		newVal;

			aggstate = op->d.agg_trans.aggstate;
			pertrans = op->d.agg_trans.pertrans;

			pergroup = &aggstate->all_pergroups
				[op->d.agg_trans.setoff]
				[op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			fcinfo = &pertrans->transfn_fcinfo;

			/* cf. select_current_set() */
			aggstate->curaggcontext = op->d.agg_trans.aggcontext;
			aggstate->current_set = op->d.agg_trans.setno;

			/* set up aggstate->curpertrans for AggGetAggref() */
			aggstate->curpertrans = pertrans;

			/* invoke transition function in per-tuple context */
			oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

			fcinfo->arg[0] = pergroup->transValue;
			fcinfo->argnull[0] = pergroup->transValueIsNull;
			fcinfo->isnull = false; /* just in case transfn doesn't set it */

			newVal = FunctionCallInvoke(fcinfo);

			pergroup->transValue = newVal;
			pergroup->transValueIsNull = fcinfo->isnull;

			MemoryContextSwitchTo(oldContext);

			EEO_NEXT();
		}

		/*
		 * Evaluate aggregate transition / combine function that has a
		 * by-reference transition type.
		 *
		 * Could optimize a bit further by splitting off by-reference
		 * fixed-length types, but currently that doesn't seem worth it.
		 */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS)
		{
			AggState   *aggstate;
			AggStatePerTrans pertrans;
			AggStatePerGroup pergroup;
			FunctionCallInfo fcinfo;
			MemoryContext oldContext;
			Datum		newVal;

			aggstate = op->d.agg_trans.aggstate;
			pertrans = op->d.agg_trans.pertrans;

			pergroup = &aggstate->all_pergroups
				[op->d.agg_trans.setoff]
				[op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			fcinfo = &pertrans->transfn_fcinfo;

			/* cf. select_current_set() */
			aggstate->curaggcontext = op->d.agg_trans.aggcontext;
			aggstate->current_set = op->d.agg_trans.setno;

			/* set up aggstate->curpertrans for AggGetAggref() */
			aggstate->curpertrans = pertrans;

			/* invoke transition function in per-tuple context */
			oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

			fcinfo->arg[0] = pergroup->transValue;
			fcinfo->argnull[0] = pergroup->transValueIsNull;
			fcinfo->isnull = false; /* just in case transfn doesn't set it */

			newVal = FunctionCallInvoke(fcinfo);

			/*
			 * For pass-by-ref datatype, must copy the new value into
			 * aggcontext and free the prior transValue.  But if transfn
			 * returned a pointer to its first input, we don't need to do
			 * anything.  Also, if transfn returned a pointer to a R/W
			 * expanded object that is already a child of the aggcontext,
			 * assume we can adopt that value without copying it.
			 *
			 * It's safe to compare newVal with pergroup->transValue without
			 * regard for either being NULL, because ExecAggTransReparent()
			 * takes care to set transValue to 0 when NULL. Otherwise we could
			 * end up accidentally not reparenting, when the transValue has
			 * the same numerical value as newValue, despite being NULL.  This
			 * is a somewhat hot path, making it undesirable to instead solve
			 * this with another branch for the common case of the transition
			 * function returning its (modified) input argument.
			 */
			if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
				newVal = ExecAggTransReparent(aggstate, pertrans,
											  newVal, fcinfo->isnull,
											  pergroup->transValue,
											  pergroup->transValueIsNull);

			pergroup->transValue = newVal;
			pergroup->transValueIsNull = fcinfo->isnull;

			MemoryContextSwitchTo(oldContext);

			EEO_NEXT();
		}

		/* process single-column ordered aggregate datum */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_DATUM)
		{
			/* too complex for an inline implementation */
			ExecEvalAggOrderedTransDatum(state, op, econtext);

			EEO_NEXT();
		}

		/* process multi-column ordered aggregate tuple */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_TUPLE)
		{
			/* too complex for an inline implementation */
			ExecEvalAggOrderedTransTuple(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERTYPECAST)
		{
			ExecEvalCypherTypeCast(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERMAPEXPR)
		{
			ExecEvalCypherMapExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTEXPR)
		{
			ExecEvalCypherListExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTCOMP_BEGIN)
		{
			*op->d.cypherlistcomp.liststate = NULL;
			pushJsonbValue(op->d.cypherlistcomp.liststate,
						   WJB_BEGIN_ARRAY, NULL);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTCOMP_ELEM)
		{
			JsonbValue	_ejv;
			JsonbValue *ejv;

			if (*op->d.cypherlistcomp.elemnull)
			{
				_ejv.type = jbvNull;
				ejv = &_ejv;
			}
			else
			{
				Jsonb	   *ejb;

				ejb = DatumGetJsonbP(*op->d.cypherlistcomp.elemvalue);
				if (JB_ROOT_IS_SCALAR(ejb))
				{
					ejv = getIthJsonbValueFromContainer(&ejb->root, 0);
				}
				else
				{
					_ejv.type = jbvBinary;
					_ejv.val.binary.data = &ejb->root;
					ejv = &_ejv;
				}
			}

			pushJsonbValue(op->d.cypherlistcomp.liststate, WJB_ELEM, ejv);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTCOMP_END)
		{
			JsonbValue *jv;

			jv = pushJsonbValue(op->d.cypherlistcomp.liststate,
								WJB_END_ARRAY, NULL);

			*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb(jv));
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTCOMP_ITER_INIT)
		{
			Jsonb	   *listjb;
			JsonbIterator **ji;
			JsonbValue	jv;

			Assert(!*op->d.cypherlistcomp_iter.listnull);

			listjb = DatumGetJsonbP(*op->d.cypherlistcomp_iter.listvalue);
			if (!JB_ROOT_IS_ARRAY(listjb) || JB_ROOT_IS_SCALAR(listjb))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("list is expected but %s",
						 JsonbToCString(NULL, &listjb->root,
										VARSIZE(listjb)))));

			ji = op->d.cypherlistcomp_iter.listiter;
			*ji = JsonbIteratorInit(&listjb->root);
			JsonbIteratorNext(ji, &jv, false);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTCOMP_ITER_NEXT)
		{
			JsonbIterator **ji;
			JsonbValue	jv;
			JsonbIteratorToken jt;

			ji = op->d.cypherlistcomp_iter.listiter;
			jt = JsonbIteratorNext(ji, &jv, true);
			if (jt == WJB_ELEM)
			{
				*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb(&jv));
				*op->resnull = false;
			}
			else
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERLISTCOMP_VAR)
		{
			*op->resvalue = *op->d.cypherlistcomp_var.elemvalue;
			*op->resnull = *op->d.cypherlistcomp_var.elemnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CYPHERACCESSEXPR)
		{
			ExecEvalCypherAccessExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_LAST)
		{
			/* unreachable */
			Assert(false);
			goto out;
		}
	}

out:
	*isnull = state->resnull;
	return state->resvalue;
}

/*
 * Expression evaluation callback that performs extra checks before executing
 * the expression. Declared extern so other methods of execution can use it
 * too.
 */
Datum
ExecInterpExprStillValid(ExprState *state, ExprContext *econtext, bool *isNull)
{
	/*
	 * First time through, check whether attribute matches Var.  Might not be
	 * ok anymore, due to schema changes.
	 */
	CheckExprStillValid(state, econtext);

	/* skip the check during further executions */
	state->evalfunc = (ExprStateEvalFunc) state->evalfunc_private;

	/* and actually execute */
	return state->evalfunc(state, econtext, isNull);
}

/*
 * Check that an expression is still valid in the face of potential schema
 * changes since the plan has been created.
 */
void
CheckExprStillValid(ExprState *state, ExprContext *econtext)
{
	int			i = 0;
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;

	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;

	for (i = 0; i < state->steps_len; i++)
	{
		ExprEvalStep *op = &state->steps[i];

		switch (ExecEvalStepOp(state, op))
		{
			case EEOP_INNER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(innerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_OUTER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(outerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_SCAN_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(scanslot, attnum + 1, op->d.var.vartype);
					break;
				}
			default:
				break;
		}
	}
}

/*
 * Check whether a user attribute in a slot can be referenced by a Var
 * expression.  This should succeed unless there have been schema changes
 * since the expression tree has been created.
 */
static void
CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype)
{
	/*
	 * What we have to check for here is the possibility of an attribute
	 * having been dropped or changed in type since the plan tree was created.
	 * Ideally the plan will get invalidated and not re-used, but just in
	 * case, we keep these defenses.  Fortunately it's sufficient to check
	 * once on the first time through.
	 *
	 * Note: ideally we'd check typmod as well as typid, but that seems
	 * impractical at the moment: in many cases the tupdesc will have been
	 * generated by ExecTypeFromTL(), and that can't guarantee to generate an
	 * accurate typmod in all cases, because some expression node types don't
	 * carry typmod.  Fortunately, for precisely that reason, there should be
	 * no places with a critical dependency on the typmod of a value.
	 *
	 * System attributes don't require checking since their types never
	 * change.
	 */
	if (attnum > 0)
	{
		TupleDesc	slot_tupdesc = slot->tts_tupleDescriptor;
		Form_pg_attribute attr;

		if (attnum > slot_tupdesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 attnum, slot_tupdesc->natts);

		attr = TupleDescAttr(slot_tupdesc, attnum - 1);

		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of type %s has been dropped",
							attnum, format_type_be(slot_tupdesc->tdtypeid))));

		if (vartype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d of type %s has wrong type",
							attnum, format_type_be(slot_tupdesc->tdtypeid)),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(vartype))));
	}
}

/*
 * get_cached_rowtype: utility function to lookup a rowtype tupdesc
 *
 * type_id, typmod: identity of the rowtype
 * cache_field: where to cache the TupleDesc pointer in expression state node
 *		(field must be initialized to NULL)
 * econtext: expression context we are executing in
 *
 * NOTE: because the shutdown callback will be called during plan rescan,
 * must be prepared to re-do this during any node execution; cannot call
 * just once during expression initialization.
 */
static TupleDesc
get_cached_rowtype(Oid type_id, int32 typmod,
				   TupleDesc *cache_field, ExprContext *econtext)
{
	TupleDesc	tupDesc = *cache_field;

	/* Do lookup if no cached value or if requested type changed */
	if (tupDesc == NULL ||
		type_id != tupDesc->tdtypeid ||
		typmod != tupDesc->tdtypmod)
	{
		tupDesc = lookup_rowtype_tupdesc(type_id, typmod);

		if (*cache_field)
		{
			/* Release old tupdesc; but callback is already registered */
			ReleaseTupleDesc(*cache_field);
		}
		else
		{
			/* Need to register shutdown callback to release tupdesc */
			RegisterExprContextCallback(econtext,
										ShutdownTupleDescRef,
										PointerGetDatum(cache_field));
		}
		*cache_field = tupDesc;
	}
	return tupDesc;
}

/*
 * Callback function to release a tupdesc refcount at econtext shutdown
 */
static void
ShutdownTupleDescRef(Datum arg)
{
	TupleDesc  *cache_field = (TupleDesc *) DatumGetPointer(arg);

	if (*cache_field)
		ReleaseTupleDesc(*cache_field);
	*cache_field = NULL;
}

/*
 * Fast-path functions, for very simple expressions
 */

/* Simple reference to inner Var */
static Datum
ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.var.attnum + 1;
	TupleTableSlot *slot = econtext->ecxt_innertuple;

	/*
	 * Since we use slot_getattr(), we don't need to implement the FETCHSOME
	 * step explicitly, and we also needn't Assert that the attnum is in range
	 * --- slot_getattr() will take care of any problems.
	 */
	return slot_getattr(slot, attnum, isnull);
}

/* Simple reference to outer Var */
static Datum
ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.var.attnum + 1;
	TupleTableSlot *slot = econtext->ecxt_outertuple;

	/* See comments in ExecJustInnerVar */
	return slot_getattr(slot, attnum, isnull);
}

/* Simple reference to scan Var */
static Datum
ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.var.attnum + 1;
	TupleTableSlot *slot = econtext->ecxt_scantuple;

	/* See comments in ExecJustInnerVar */
	return slot_getattr(slot, attnum, isnull);
}

/* Simple Const expression */
static Datum
ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];

	*isnull = op->d.constval.isnull;
	return op->d.constval.value;
}

/* Evaluate inner Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.assign_var.attnum + 1;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *inslot = econtext->ecxt_innertuple;
	TupleTableSlot *outslot = state->resultslot;

	/*
	 * We do not need CheckVarSlotCompatibility here; that was taken care of
	 * at compilation time.
	 *
	 * Since we use slot_getattr(), we don't need to implement the FETCHSOME
	 * step explicitly, and we also needn't Assert that the attnum is in range
	 * --- slot_getattr() will take care of any problems.
	 */
	outslot->tts_values[resultnum] =
		slot_getattr(inslot, attnum, &outslot->tts_isnull[resultnum]);
	return 0;
}

/* Evaluate outer Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.assign_var.attnum + 1;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *inslot = econtext->ecxt_outertuple;
	TupleTableSlot *outslot = state->resultslot;

	/* See comments in ExecJustAssignInnerVar */
	outslot->tts_values[resultnum] =
		slot_getattr(inslot, attnum, &outslot->tts_isnull[resultnum]);
	return 0;
}

/* Evaluate scan Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.assign_var.attnum + 1;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *inslot = econtext->ecxt_scantuple;
	TupleTableSlot *outslot = state->resultslot;

	/* See comments in ExecJustAssignInnerVar */
	outslot->tts_values[resultnum] =
		slot_getattr(inslot, attnum, &outslot->tts_isnull[resultnum]);
	return 0;
}

/* Evaluate CASE_TESTVAL and apply a strict function to it */
static Datum
ExecJustApplyFuncToCase(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	FunctionCallInfo fcinfo;
	bool	   *argnull;
	int			argno;
	Datum		d;

	/*
	 * XXX with some redesign of the CaseTestExpr mechanism, maybe we could
	 * get rid of this data shuffling?
	 */
	*op->resvalue = *op->d.casetest.value;
	*op->resnull = *op->d.casetest.isnull;

	op++;

	fcinfo = op->d.func.fcinfo_data;
	argnull = fcinfo->argnull;

	/* strict function, so check for NULL args */
	for (argno = 0; argno < op->d.func.nargs; argno++)
	{
		if (argnull[argno])
		{
			*isnull = true;
			return (Datum) 0;
		}
	}
	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*isnull = fcinfo->isnull;
	return d;
}

#if defined(EEO_USE_COMPUTED_GOTO)
/*
 * Comparator used when building address->opcode lookup table for
 * ExecEvalStepOp() in the threaded dispatch case.
 */
static int
dispatch_compare_ptr(const void *a, const void *b)
{
	const ExprEvalOpLookup *la = (const ExprEvalOpLookup *) a;
	const ExprEvalOpLookup *lb = (const ExprEvalOpLookup *) b;

	if (la->opcode < lb->opcode)
		return -1;
	else if (la->opcode > lb->opcode)
		return 1;
	return 0;
}
#endif

/*
 * Do one-time initialization of interpretation machinery.
 */
static void
ExecInitInterpreter(void)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	/* Set up externally-visible pointer to dispatch table */
	if (dispatch_table == NULL)
	{
		int			i;

		dispatch_table = (const void **)
			DatumGetPointer(ExecInterpExpr(NULL, NULL, NULL));

		/* build reverse lookup table */
		for (i = 0; i < EEOP_LAST; i++)
		{
			reverse_dispatch_table[i].opcode = dispatch_table[i];
			reverse_dispatch_table[i].op = (ExprEvalOp) i;
		}

		/* make it bsearch()able */
		qsort(reverse_dispatch_table,
			  EEOP_LAST /* nmembers */ ,
			  sizeof(ExprEvalOpLookup),
			  dispatch_compare_ptr);
	}
#endif
}

/*
 * Function to return the opcode of an expression step.
 *
 * When direct-threading is in use, ExprState->opcode isn't easily
 * decipherable. This function returns the appropriate enum member.
 */
ExprEvalOp
ExecEvalStepOp(ExprState *state, ExprEvalStep *op)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	if (state->flags & EEO_FLAG_DIRECT_THREADED)
	{
		ExprEvalOpLookup key;
		ExprEvalOpLookup *res;

		key.opcode = (void *) op->opcode;
		res = bsearch(&key,
					  reverse_dispatch_table,
					  EEOP_LAST /* nmembers */ ,
					  sizeof(ExprEvalOpLookup),
					  dispatch_compare_ptr);
		Assert(res);			/* unknown ops shouldn't get looked up */
		return res->op;
	}
#endif
	return (ExprEvalOp) op->opcode;
}


/*
 * Out-of-line helper functions for complex instructions.
 */

/*
 * Evaluate EEOP_FUNCEXPR_FUSAGE
 */
void
ExecEvalFuncExprFusage(ExprState *state, ExprEvalStep *op,
					   ExprContext *econtext)
{
	FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
	PgStat_FunctionCallUsage fcusage;
	Datum		d;

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*op->resvalue = d;
	*op->resnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * Evaluate EEOP_FUNCEXPR_STRICT_FUSAGE
 */
void
ExecEvalFuncExprStrictFusage(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{

	FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
	PgStat_FunctionCallUsage fcusage;
	bool	   *argnull = fcinfo->argnull;
	int			argno;
	Datum		d;

	/* strict function, so check for NULL args */
	for (argno = 0; argno < op->d.func.nargs; argno++)
	{
		if (argnull[argno])
		{
			*op->resnull = true;
			return;
		}
	}

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*op->resvalue = d;
	*op->resnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * Evaluate a PARAM_EXEC parameter.
 *
 * PARAM_EXEC params (internal executor parameters) are stored in the
 * ecxt_param_exec_vals array, and can be accessed by array index.
 */
void
ExecEvalParamExec(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamExecData *prm;

	prm = &(econtext->ecxt_param_exec_vals[op->d.param.paramid]);
	if (unlikely(prm->execPlan != NULL))
	{
		/* Parameter not evaluated yet, so go do it */
		ExecSetParamPlan(prm->execPlan, econtext);
		/* ExecSetParamPlan should have processed this param... */
		Assert(prm->execPlan == NULL);
	}
	*op->resvalue = prm->value;
	*op->resnull = prm->isnull;
}

/*
 * Evaluate a PARAM_EXTERN parameter.
 *
 * PARAM_EXTERN parameters must be sought in ecxt_param_list_info.
 */
void
ExecEvalParamExtern(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;
	int			paramId = op->d.param.paramid;

	if (likely(paramInfo &&
			   paramId > 0 && paramId <= paramInfo->numParams))
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		/* give hook a chance in case parameter is dynamic */
		if (paramInfo->paramFetch != NULL)
			prm = paramInfo->paramFetch(paramInfo, paramId, false, &prmdata);
		else
			prm = &paramInfo->params[paramId - 1];

		if (likely(OidIsValid(prm->ptype)))
		{
			/* safety check in case hook did something unexpected */
			if (unlikely(prm->ptype != op->d.param.paramtype))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								paramId,
								format_type_be(prm->ptype),
								format_type_be(op->d.param.paramtype))));
			*op->resvalue = prm->value;
			*op->resnull = prm->isnull;
			return;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", paramId)));
}

/*
 * Evaluate a SQLValueFunction expression.
 */
void
ExecEvalSQLValueFunction(ExprState *state, ExprEvalStep *op)
{
	SQLValueFunction *svf = op->d.sqlvaluefunction.svf;
	FunctionCallInfoData fcinfo;

	*op->resnull = false;

	/*
	 * Note: current_schema() can return NULL.  current_user() etc currently
	 * cannot, but might as well code those cases the same way for safety.
	 */
	switch (svf->op)
	{
		case SVFOP_CURRENT_DATE:
			*op->resvalue = DateADTGetDatum(GetSQLCurrentDate());
			break;
		case SVFOP_CURRENT_TIME:
		case SVFOP_CURRENT_TIME_N:
			*op->resvalue = TimeTzADTPGetDatum(GetSQLCurrentTime(svf->typmod));
			break;
		case SVFOP_CURRENT_TIMESTAMP:
		case SVFOP_CURRENT_TIMESTAMP_N:
			*op->resvalue = TimestampTzGetDatum(GetSQLCurrentTimestamp(svf->typmod));
			break;
		case SVFOP_LOCALTIME:
		case SVFOP_LOCALTIME_N:
			*op->resvalue = TimeADTGetDatum(GetSQLLocalTime(svf->typmod));
			break;
		case SVFOP_LOCALTIMESTAMP:
		case SVFOP_LOCALTIMESTAMP_N:
			*op->resvalue = TimestampGetDatum(GetSQLLocalTimestamp(svf->typmod));
			break;
		case SVFOP_CURRENT_ROLE:
		case SVFOP_CURRENT_USER:
		case SVFOP_USER:
			InitFunctionCallInfoData(fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_user(&fcinfo);
			*op->resnull = fcinfo.isnull;
			break;
		case SVFOP_SESSION_USER:
			InitFunctionCallInfoData(fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = session_user(&fcinfo);
			*op->resnull = fcinfo.isnull;
			break;
		case SVFOP_CURRENT_CATALOG:
			InitFunctionCallInfoData(fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_database(&fcinfo);
			*op->resnull = fcinfo.isnull;
			break;
		case SVFOP_CURRENT_SCHEMA:
			InitFunctionCallInfoData(fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_schema(&fcinfo);
			*op->resnull = fcinfo.isnull;
			break;
	}
}

/*
 * Raise error if a CURRENT OF expression is evaluated.
 *
 * The planner should convert CURRENT OF into a TidScan qualification, or some
 * other special handling in a ForeignScan node.  So we have to be able to do
 * ExecInitExpr on a CurrentOfExpr, but we shouldn't ever actually execute it.
 * If we get here, we suppose we must be dealing with CURRENT OF on a foreign
 * table whose FDW doesn't handle it, and complain accordingly.
 */
void
ExecEvalCurrentOfExpr(ExprState *state, ExprEvalStep *op)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("WHERE CURRENT OF is not supported for this table type")));
}

/*
 * Evaluate NextValueExpr.
 */
void
ExecEvalNextValueExpr(ExprState *state, ExprEvalStep *op)
{
	int64		newval = nextval_internal(op->d.nextvalueexpr.seqid, false);

	switch (op->d.nextvalueexpr.seqtypid)
	{
		case INT2OID:
			*op->resvalue = Int16GetDatum((int16) newval);
			break;
		case INT4OID:
			*op->resvalue = Int32GetDatum((int32) newval);
			break;
		case INT8OID:
			*op->resvalue = Int64GetDatum((int64) newval);
			break;
		default:
			elog(ERROR, "unsupported sequence type %u",
				 op->d.nextvalueexpr.seqtypid);
	}
	*op->resnull = false;
}

/*
 * Evaluate NullTest / IS NULL for rows.
 */
void
ExecEvalRowNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ExecEvalRowNullInt(state, op, econtext, true);
}

/*
 * Evaluate NullTest / IS NOT NULL for rows.
 */
void
ExecEvalRowNotNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ExecEvalRowNullInt(state, op, econtext, false);
}

/* Common code for IS [NOT] NULL on a row value */
static void
ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
				   ExprContext *econtext, bool checkisnull)
{
	Datum		value = *op->resvalue;
	bool		isnull = *op->resnull;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;
	int			att;

	*op->resnull = false;

	/* NULL row variables are treated just as NULL scalar columns */
	if (isnull)
	{
		*op->resvalue = BoolGetDatum(checkisnull);
		return;
	}

	/*
	 * The SQL standard defines IS [NOT] NULL for a non-null rowtype argument
	 * as:
	 *
	 * "R IS NULL" is true if every field is the null value.
	 *
	 * "R IS NOT NULL" is true if no field is the null value.
	 *
	 * This definition is (apparently intentionally) not recursive; so our
	 * tests on the fields are primitive attisnull tests, not recursive checks
	 * to see if they are all-nulls or no-nulls rowtypes.
	 *
	 * The standard does not consider the possibility of zero-field rows, but
	 * here we consider them to vacuously satisfy both predicates.
	 */

	tuple = DatumGetHeapTupleHeader(value);

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);

	/* Lookup tupdesc if first time through or if type changes */
	tupDesc = get_cached_rowtype(tupType, tupTypmod,
								 &op->d.nulltest_row.argdesc,
								 econtext);

	/*
	 * heap_attisnull needs a HeapTuple not a bare HeapTupleHeader.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	for (att = 1; att <= tupDesc->natts; att++)
	{
		/* ignore dropped columns */
		if (TupleDescAttr(tupDesc, att - 1)->attisdropped)
			continue;
		if (heap_attisnull(&tmptup, att, tupDesc))
		{
			/* null field disproves IS NOT NULL */
			if (!checkisnull)
			{
				*op->resvalue = BoolGetDatum(false);
				return;
			}
		}
		else
		{
			/* non-null field disproves IS NULL */
			if (checkisnull)
			{
				*op->resvalue = BoolGetDatum(false);
				return;
			}
		}
	}

	*op->resvalue = BoolGetDatum(true);
}

/*
 * Evaluate an ARRAY[] expression.
 *
 * The individual array elements (or subarrays) have already been evaluated
 * into op->d.arrayexpr.elemvalues[]/elemnulls[].
 */
void
ExecEvalArrayExpr(ExprState *state, ExprEvalStep *op)
{
	ArrayType  *result;
	Oid			element_type = op->d.arrayexpr.elemtype;
	int			nelems = op->d.arrayexpr.nelems;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	/* Set non-null as default */
	*op->resnull = false;

	if (!op->d.arrayexpr.multidims)
	{
		/* Elements are presumably of scalar type */
		Datum	   *dvalues = op->d.arrayexpr.elemvalues;
		bool	   *dnulls = op->d.arrayexpr.elemnulls;

		/* setup for 1-D array of the given length */
		ndims = 1;
		dims[0] = nelems;
		lbs[0] = 1;

		result = construct_md_array(dvalues, dnulls, ndims, dims, lbs,
									element_type,
									op->d.arrayexpr.elemlength,
									op->d.arrayexpr.elembyval,
									op->d.arrayexpr.elemalign);
	}
	else
	{
		/* Must be nested array expressions */
		int			nbytes = 0;
		int			nitems = 0;
		int			outer_nelems = 0;
		int			elem_ndims = 0;
		int		   *elem_dims = NULL;
		int		   *elem_lbs = NULL;
		bool		firstone = true;
		bool		havenulls = false;
		bool		haveempty = false;
		char	  **subdata;
		bits8	  **subbitmaps;
		int		   *subbytes;
		int		   *subnitems;
		int32		dataoffset;
		char	   *dat;
		int			iitem;
		int			elemoff;
		int			i;

		subdata = (char **) palloc(nelems * sizeof(char *));
		subbitmaps = (bits8 **) palloc(nelems * sizeof(bits8 *));
		subbytes = (int *) palloc(nelems * sizeof(int));
		subnitems = (int *) palloc(nelems * sizeof(int));

		/* loop through and get data area from each element */
		for (elemoff = 0; elemoff < nelems; elemoff++)
		{
			Datum		arraydatum;
			bool		eisnull;
			ArrayType  *array;
			int			this_ndims;

			arraydatum = op->d.arrayexpr.elemvalues[elemoff];
			eisnull = op->d.arrayexpr.elemnulls[elemoff];

			/* temporarily ignore null subarrays */
			if (eisnull)
			{
				haveempty = true;
				continue;
			}

			array = DatumGetArrayTypeP(arraydatum);

			/* run-time double-check on element type */
			if (element_type != ARR_ELEMTYPE(array))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot merge incompatible arrays"),
						 errdetail("Array with element type %s cannot be "
								   "included in ARRAY construct with element type %s.",
								   format_type_be(ARR_ELEMTYPE(array)),
								   format_type_be(element_type))));

			this_ndims = ARR_NDIM(array);
			/* temporarily ignore zero-dimensional subarrays */
			if (this_ndims <= 0)
			{
				haveempty = true;
				continue;
			}

			if (firstone)
			{
				/* Get sub-array details from first member */
				elem_ndims = this_ndims;
				ndims = elem_ndims + 1;
				if (ndims <= 0 || ndims > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions (%d) exceeds " \
									"the maximum allowed (%d)", ndims, MAXDIM)));

				elem_dims = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_dims, ARR_DIMS(array), elem_ndims * sizeof(int));
				elem_lbs = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_lbs, ARR_LBOUND(array), elem_ndims * sizeof(int));

				firstone = false;
			}
			else
			{
				/* Check other sub-arrays are compatible */
				if (elem_ndims != this_ndims ||
					memcmp(elem_dims, ARR_DIMS(array),
						   elem_ndims * sizeof(int)) != 0 ||
					memcmp(elem_lbs, ARR_LBOUND(array),
						   elem_ndims * sizeof(int)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
							 errmsg("multidimensional arrays must have array "
									"expressions with matching dimensions")));
			}

			subdata[outer_nelems] = ARR_DATA_PTR(array);
			subbitmaps[outer_nelems] = ARR_NULLBITMAP(array);
			subbytes[outer_nelems] = ARR_SIZE(array) - ARR_DATA_OFFSET(array);
			nbytes += subbytes[outer_nelems];
			subnitems[outer_nelems] = ArrayGetNItems(this_ndims,
													 ARR_DIMS(array));
			nitems += subnitems[outer_nelems];
			havenulls |= ARR_HASNULL(array);
			outer_nelems++;
		}

		/*
		 * If all items were null or empty arrays, return an empty array;
		 * otherwise, if some were and some weren't, raise error.  (Note: we
		 * must special-case this somehow to avoid trying to generate a 1-D
		 * array formed from empty arrays.  It's not ideal...)
		 */
		if (haveempty)
		{
			if (ndims == 0)		/* didn't find any nonempty array */
			{
				*op->resvalue = PointerGetDatum(construct_empty_array(element_type));
				return;
			}
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("multidimensional arrays must have array "
							"expressions with matching dimensions")));
		}

		/* setup for multi-D array */
		dims[0] = outer_nelems;
		lbs[0] = 1;
		for (i = 1; i < ndims; i++)
		{
			dims[i] = elem_dims[i - 1];
			lbs[i] = elem_lbs[i - 1];
		}

		if (havenulls)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;		/* marker for no null bitmap */
			nbytes += ARR_OVERHEAD_NONULLS(ndims);
		}

		result = (ArrayType *) palloc(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = ndims;
		result->dataoffset = dataoffset;
		result->elemtype = element_type;
		memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

		dat = ARR_DATA_PTR(result);
		iitem = 0;
		for (i = 0; i < outer_nelems; i++)
		{
			memcpy(dat, subdata[i], subbytes[i]);
			dat += subbytes[i];
			if (havenulls)
				array_bitmap_copy(ARR_NULLBITMAP(result), iitem,
								  subbitmaps[i], 0,
								  subnitems[i]);
			iitem += subnitems[i];
		}
	}

	*op->resvalue = PointerGetDatum(result);
}

/*
 * Evaluate an ArrayCoerceExpr expression.
 *
 * Source array is in step's result variable.
 */
void
ExecEvalArrayCoerce(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	Datum		arraydatum;

	/* NULL array -> NULL result */
	if (*op->resnull)
		return;

	arraydatum = *op->resvalue;

	/*
	 * If it's binary-compatible, modify the element type in the array header,
	 * but otherwise leave the array as we received it.
	 */
	if (op->d.arraycoerce.elemexprstate == NULL)
	{
		/* Detoast input array if necessary, and copy in any case */
		ArrayType  *array = DatumGetArrayTypePCopy(arraydatum);

		ARR_ELEMTYPE(array) = op->d.arraycoerce.resultelemtype;
		*op->resvalue = PointerGetDatum(array);
		return;
	}

	/*
	 * Use array_map to apply the sub-expression to each array element.
	 */
	*op->resvalue = array_map(arraydatum,
							  op->d.arraycoerce.elemexprstate,
							  econtext,
							  op->d.arraycoerce.resultelemtype,
							  op->d.arraycoerce.amstate);
}

/*
 * Evaluate a ROW() expression.
 *
 * The individual columns have already been evaluated into
 * op->d.row.elemvalues[]/elemnulls[].
 */
void
ExecEvalRow(ExprState *state, ExprEvalStep *op)
{
	HeapTuple	tuple;

	/* build tuple from evaluated field values */
	tuple = heap_form_tuple(op->d.row.tupdesc,
							op->d.row.elemvalues,
							op->d.row.elemnulls);

	*op->resvalue = HeapTupleGetDatum(tuple);
	*op->resnull = false;
}

/*
 * Evaluate GREATEST() or LEAST() expression (note this is *not* MIN()/MAX()).
 *
 * All of the to-be-compared expressions have already been evaluated into
 * op->d.minmax.values[]/nulls[].
 */
void
ExecEvalMinMax(ExprState *state, ExprEvalStep *op)
{
	Datum	   *values = op->d.minmax.values;
	bool	   *nulls = op->d.minmax.nulls;
	FunctionCallInfo fcinfo = op->d.minmax.fcinfo_data;
	MinMaxOp	operator = op->d.minmax.op;
	int			off;

	/* set at initialization */
	Assert(fcinfo->argnull[0] == false);
	Assert(fcinfo->argnull[1] == false);

	/* default to null result */
	*op->resnull = true;

	for (off = 0; off < op->d.minmax.nelems; off++)
	{
		/* ignore NULL inputs */
		if (nulls[off])
			continue;

		if (*op->resnull)
		{
			/* first nonnull input, adopt value */
			*op->resvalue = values[off];
			*op->resnull = false;
		}
		else
		{
			int			cmpresult;

			/* apply comparison function */
			fcinfo->arg[0] = *op->resvalue;
			fcinfo->arg[1] = values[off];

			fcinfo->isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(fcinfo));
			if (fcinfo->isnull) /* probably should not happen */
				continue;

			if (cmpresult > 0 && operator == IS_LEAST)
				*op->resvalue = values[off];
			else if (cmpresult < 0 && operator == IS_GREATEST)
				*op->resvalue = values[off];
		}
	}
}

/*
 * Evaluate a FieldSelect node.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalFieldSelect(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	AttrNumber	fieldnum = op->d.fieldselect.fieldnum;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	Form_pg_attribute attr;
	HeapTupleData tmptup;

	/* NULL record -> NULL result */
	if (*op->resnull)
		return;

	tupDatum = *op->resvalue;

	/* We can special-case expanded records for speed */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(tupDatum)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(tupDatum);

		Assert(erh->er_magic == ER_MAGIC);

		/* Extract record's TupleDesc */
		tupDesc = expanded_record_get_tupdesc(erh);

		/*
		 * Find field's attr record.  Note we don't support system columns
		 * here: a datum tuple doesn't have valid values for most of the
		 * interesting system columns anyway.
		 */
		if (fieldnum <= 0)		/* should never happen */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* Check for dropped column, and force a NULL result if so */
		if (attr->attisdropped)
		{
			*op->resnull = true;
			return;
		}

		/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
		/* As in CheckVarSlotCompatibility, we should but can't check typmod */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* extract the field */
		*op->resvalue = expanded_record_get_field(erh, fieldnum,
												  op->resnull);
	}
	else
	{
		/* Get the composite datum and extract its type fields */
		tuple = DatumGetHeapTupleHeader(tupDatum);

		tupType = HeapTupleHeaderGetTypeId(tuple);
		tupTypmod = HeapTupleHeaderGetTypMod(tuple);

		/* Lookup tupdesc if first time through or if type changes */
		tupDesc = get_cached_rowtype(tupType, tupTypmod,
									 &op->d.fieldselect.argdesc,
									 econtext);

		/*
		 * Find field's attr record.  Note we don't support system columns
		 * here: a datum tuple doesn't have valid values for most of the
		 * interesting system columns anyway.
		 */
		if (fieldnum <= 0)		/* should never happen */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* Check for dropped column, and force a NULL result if so */
		if (attr->attisdropped)
		{
			*op->resnull = true;
			return;
		}

		/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
		/* As in CheckVarSlotCompatibility, we should but can't check typmod */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* heap_getattr needs a HeapTuple not a bare HeapTupleHeader */
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
		tmptup.t_data = tuple;

		/* extract the field */
		*op->resvalue = heap_getattr(&tmptup,
									 fieldnum,
									 tupDesc,
									 op->resnull);
	}
}

/*
 * Deform source tuple, filling in the step's values/nulls arrays, before
 * evaluating individual new values as part of a FieldStore expression.
 * Subsequent steps will overwrite individual elements of the values/nulls
 * arrays with the new field values, and then FIELDSTORE_FORM will build the
 * new tuple value.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalFieldStoreDeForm(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	TupleDesc	tupDesc;

	/* Lookup tupdesc if first time through or after rescan */
	tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
								 op->d.fieldstore.argdesc, econtext);

	/* Check that current tupdesc doesn't have more fields than we allocated */
	if (unlikely(tupDesc->natts > op->d.fieldstore.ncolumns))
		elog(ERROR, "too many columns in composite type %u",
			 op->d.fieldstore.fstore->resulttype);

	if (*op->resnull)
	{
		/* Convert null input tuple into an all-nulls row */
		memset(op->d.fieldstore.nulls, true,
			   op->d.fieldstore.ncolumns * sizeof(bool));
	}
	else
	{
		/*
		 * heap_deform_tuple needs a HeapTuple not a bare HeapTupleHeader. We
		 * set all the fields in the struct just in case.
		 */
		Datum		tupDatum = *op->resvalue;
		HeapTupleHeader tuphdr;
		HeapTupleData tmptup;

		tuphdr = DatumGetHeapTupleHeader(tupDatum);
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = tuphdr;

		heap_deform_tuple(&tmptup, tupDesc,
						  op->d.fieldstore.values,
						  op->d.fieldstore.nulls);
	}
}

/*
 * Compute the new composite datum after each individual field value of a
 * FieldStore expression has been evaluated.
 */
void
ExecEvalFieldStoreForm(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	HeapTuple	tuple;

	/* argdesc should already be valid from the DeForm step */
	tuple = heap_form_tuple(*op->d.fieldstore.argdesc,
							op->d.fieldstore.values,
							op->d.fieldstore.nulls);

	*op->resvalue = HeapTupleGetDatum(tuple);
	*op->resnull = false;
}

/*
 * Process a subscript in an ArrayRef expression.
 *
 * If subscript is NULL, throw error in assignment case, or in fetch case
 * set result to NULL and return false (instructing caller to skip the rest
 * of the ArrayRef sequence).
 *
 * Subscript expression result is in subscriptvalue/subscriptnull.
 * On success, integer subscript value has been saved in upperindex[] or
 * lowerindex[] for use later.
 */
bool
ExecEvalArrayRefSubscript(ExprState *state, ExprEvalStep *op)
{
	ArrayRefState *arefstate = op->d.arrayref_subscript.state;
	int		   *indexes;
	int			off;

	/* If any index expr yields NULL, result is NULL or error */
	if (arefstate->subscriptnull)
	{
		if (arefstate->isassignment)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("array subscript in assignment must not be null")));
		*op->resnull = true;
		return false;
	}

	/* Convert datum to int, save in appropriate place */
	if (op->d.arrayref_subscript.isupper)
		indexes = arefstate->upperindex;
	else
		indexes = arefstate->lowerindex;
	off = op->d.arrayref_subscript.off;

	indexes[off] = DatumGetInt32(arefstate->subscriptvalue);

	return true;
}

/*
 * Evaluate ArrayRef fetch.
 *
 * Source array is in step's result variable.
 */
void
ExecEvalArrayRefFetch(ExprState *state, ExprEvalStep *op)
{
	ArrayRefState *arefstate = op->d.arrayref.state;

	/* Should not get here if source array (or any subscript) is null */
	Assert(!(*op->resnull));

	if (arefstate->numlower == 0)
	{
		/* Scalar case */
		*op->resvalue = array_get_element(*op->resvalue,
										  arefstate->numupper,
										  arefstate->upperindex,
										  arefstate->refattrlength,
										  arefstate->refelemlength,
										  arefstate->refelembyval,
										  arefstate->refelemalign,
										  op->resnull);
	}
	else
	{
		/* Slice case */
		*op->resvalue = array_get_slice(*op->resvalue,
										arefstate->numupper,
										arefstate->upperindex,
										arefstate->lowerindex,
										arefstate->upperprovided,
										arefstate->lowerprovided,
										arefstate->refattrlength,
										arefstate->refelemlength,
										arefstate->refelembyval,
										arefstate->refelemalign);
	}
}

/*
 * Compute old array element/slice value for an ArrayRef assignment
 * expression.  Will only be generated if the new-value subexpression
 * contains ArrayRef or FieldStore.  The value is stored into the
 * ArrayRefState's prevvalue/prevnull fields.
 */
void
ExecEvalArrayRefOld(ExprState *state, ExprEvalStep *op)
{
	ArrayRefState *arefstate = op->d.arrayref.state;

	if (*op->resnull)
	{
		/* whole array is null, so any element or slice is too */
		arefstate->prevvalue = (Datum) 0;
		arefstate->prevnull = true;
	}
	else if (arefstate->numlower == 0)
	{
		/* Scalar case */
		arefstate->prevvalue = array_get_element(*op->resvalue,
												 arefstate->numupper,
												 arefstate->upperindex,
												 arefstate->refattrlength,
												 arefstate->refelemlength,
												 arefstate->refelembyval,
												 arefstate->refelemalign,
												 &arefstate->prevnull);
	}
	else
	{
		/* Slice case */
		/* this is currently unreachable */
		arefstate->prevvalue = array_get_slice(*op->resvalue,
											   arefstate->numupper,
											   arefstate->upperindex,
											   arefstate->lowerindex,
											   arefstate->upperprovided,
											   arefstate->lowerprovided,
											   arefstate->refattrlength,
											   arefstate->refelemlength,
											   arefstate->refelembyval,
											   arefstate->refelemalign);
		arefstate->prevnull = false;
	}
}

/*
 * Evaluate ArrayRef assignment.
 *
 * Input array (possibly null) is in result area, replacement value is in
 * ArrayRefState's replacevalue/replacenull.
 */
void
ExecEvalArrayRefAssign(ExprState *state, ExprEvalStep *op)
{
	ArrayRefState *arefstate = op->d.arrayref.state;

	/*
	 * For an assignment to a fixed-length array type, both the original array
	 * and the value to be assigned into it must be non-NULL, else we punt and
	 * return the original array.
	 */
	if (arefstate->refattrlength > 0)	/* fixed-length array? */
	{
		if (*op->resnull || arefstate->replacenull)
			return;
	}

	/*
	 * For assignment to varlena arrays, we handle a NULL original array by
	 * substituting an empty (zero-dimensional) array; insertion of the new
	 * element will result in a singleton array value.  It does not matter
	 * whether the new element is NULL.
	 */
	if (*op->resnull)
	{
		*op->resvalue = PointerGetDatum(construct_empty_array(arefstate->refelemtype));
		*op->resnull = false;
	}

	if (arefstate->numlower == 0)
	{
		/* Scalar case */
		*op->resvalue = array_set_element(*op->resvalue,
										  arefstate->numupper,
										  arefstate->upperindex,
										  arefstate->replacevalue,
										  arefstate->replacenull,
										  arefstate->refattrlength,
										  arefstate->refelemlength,
										  arefstate->refelembyval,
										  arefstate->refelemalign);
	}
	else
	{
		/* Slice case */
		*op->resvalue = array_set_slice(*op->resvalue,
										arefstate->numupper,
										arefstate->upperindex,
										arefstate->lowerindex,
										arefstate->upperprovided,
										arefstate->lowerprovided,
										arefstate->replacevalue,
										arefstate->replacenull,
										arefstate->refattrlength,
										arefstate->refelemlength,
										arefstate->refelembyval,
										arefstate->refelemalign);
	}
}

/*
 * Evaluate a rowtype coercion operation.
 * This may require rearranging field positions.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalConvertRowtype(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ConvertRowtypeExpr *convert = op->d.convert_rowtype.convert;
	HeapTuple	result;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	HeapTupleData tmptup;
	TupleDesc	indesc,
				outdesc;

	/* NULL in -> NULL out */
	if (*op->resnull)
		return;

	tupDatum = *op->resvalue;
	tuple = DatumGetHeapTupleHeader(tupDatum);

	/* Lookup tupdescs if first time through or after rescan */
	if (op->d.convert_rowtype.indesc == NULL)
	{
		get_cached_rowtype(exprType((Node *) convert->arg), -1,
						   &op->d.convert_rowtype.indesc,
						   econtext);
		op->d.convert_rowtype.initialized = false;
	}
	if (op->d.convert_rowtype.outdesc == NULL)
	{
		get_cached_rowtype(convert->resulttype, -1,
						   &op->d.convert_rowtype.outdesc,
						   econtext);
		op->d.convert_rowtype.initialized = false;
	}

	indesc = op->d.convert_rowtype.indesc;
	outdesc = op->d.convert_rowtype.outdesc;

	/*
	 * We used to be able to assert that incoming tuples are marked with
	 * exactly the rowtype of indesc.  However, now that ExecEvalWholeRowVar
	 * might change the tuples' marking to plain RECORD due to inserting
	 * aliases, we can only make this weak test:
	 */
	Assert(HeapTupleHeaderGetTypeId(tuple) == indesc->tdtypeid ||
		   HeapTupleHeaderGetTypeId(tuple) == RECORDOID);

	/* if first time through, initialize conversion map */
	if (!op->d.convert_rowtype.initialized)
	{
		MemoryContext old_cxt;

		/* allocate map in long-lived memory context */
		old_cxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		/* prepare map from old to new attribute numbers */
		op->d.convert_rowtype.map =
			convert_tuples_by_name(indesc, outdesc,
								   gettext_noop("could not convert row type"));
		op->d.convert_rowtype.initialized = true;

		MemoryContextSwitchTo(old_cxt);
	}

	/* Following steps need a HeapTuple not a bare HeapTupleHeader */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	if (op->d.convert_rowtype.map != NULL)
	{
		/* Full conversion with attribute rearrangement needed */
		result = do_convert_tuple(&tmptup, op->d.convert_rowtype.map);
		/* Result already has appropriate composite-datum header fields */
		*op->resvalue = HeapTupleGetDatum(result);
	}
	else
	{
		/*
		 * The tuple is physically compatible as-is, but we need to insert the
		 * destination rowtype OID in its composite-datum header field, so we
		 * have to copy it anyway.  heap_copy_tuple_as_datum() is convenient
		 * for this since it will both make the physical copy and insert the
		 * correct composite header fields.  Note that we aren't expecting to
		 * have to flatten any toasted fields: the input was a composite
		 * datum, so it shouldn't contain any.  So heap_copy_tuple_as_datum()
		 * is overkill here, but its check for external fields is cheap.
		 */
		*op->resvalue = heap_copy_tuple_as_datum(&tmptup, outdesc);
	}
}

/*
 * Evaluate "scalar op ANY/ALL (array)".
 *
 * Source array is in our result area, scalar arg is already evaluated into
 * fcinfo->arg[0]/argnull[0].
 *
 * The operator always yields boolean, and we combine the results across all
 * array elements using OR and AND (for ANY and ALL respectively).  Of course
 * we short-circuit as soon as the result is known.
 */
void
ExecEvalScalarArrayOp(ExprState *state, ExprEvalStep *op)
{
	FunctionCallInfo fcinfo = op->d.scalararrayop.fcinfo_data;
	bool		useOr = op->d.scalararrayop.useOr;
	bool		strictfunc = op->d.scalararrayop.finfo->fn_strict;
	ArrayType  *arr;
	int			nitems;
	Datum		result;
	bool		resultnull;
	int			i;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
	bits8	   *bitmap;
	int			bitmask;

	/*
	 * If the array is NULL then we return NULL --- it's not very meaningful
	 * to do anything else, even if the operator isn't strict.
	 */
	if (*op->resnull)
		return;

	/* Else okay to fetch and detoast the array */
	arr = DatumGetArrayTypeP(*op->resvalue);

	/*
	 * If the array is empty, we return either FALSE or TRUE per the useOr
	 * flag.  This is correct even if the scalar is NULL; since we would
	 * evaluate the operator zero times, it matters not whether it would want
	 * to return NULL.
	 */
	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0)
	{
		*op->resvalue = BoolGetDatum(!useOr);
		*op->resnull = false;
		return;
	}

	/*
	 * If the scalar is NULL, and the function is strict, return NULL; no
	 * point in iterating the loop.
	 */
	if (fcinfo->argnull[0] && strictfunc)
	{
		*op->resnull = true;
		return;
	}

	/*
	 * We arrange to look up info about the element type only once per series
	 * of calls, assuming the element type doesn't change underneath us.
	 */
	if (op->d.scalararrayop.element_type != ARR_ELEMTYPE(arr))
	{
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &op->d.scalararrayop.typlen,
							 &op->d.scalararrayop.typbyval,
							 &op->d.scalararrayop.typalign);
		op->d.scalararrayop.element_type = ARR_ELEMTYPE(arr);
	}

	typlen = op->d.scalararrayop.typlen;
	typbyval = op->d.scalararrayop.typbyval;
	typalign = op->d.scalararrayop.typalign;

	/* Initialize result appropriately depending on useOr */
	result = BoolGetDatum(!useOr);
	resultnull = false;

	/* Loop over the array elements */
	s = (char *) ARR_DATA_PTR(arr);
	bitmap = ARR_NULLBITMAP(arr);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		Datum		thisresult;

		/* Get array element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			fcinfo->arg[1] = (Datum) 0;
			fcinfo->argnull[1] = true;
		}
		else
		{
			elt = fetch_att(s, typbyval, typlen);
			s = att_addlength_pointer(s, typlen, s);
			s = (char *) att_align_nominal(s, typalign);
			fcinfo->arg[1] = elt;
			fcinfo->argnull[1] = false;
		}

		/* Call comparison function */
		if (fcinfo->argnull[1] && strictfunc)
		{
			fcinfo->isnull = true;
			thisresult = (Datum) 0;
		}
		else
		{
			fcinfo->isnull = false;
			thisresult = op->d.scalararrayop.fn_addr(fcinfo);
		}

		/* Combine results per OR or AND semantics */
		if (fcinfo->isnull)
			resultnull = true;
		else if (useOr)
		{
			if (DatumGetBool(thisresult))
			{
				result = BoolGetDatum(true);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}
		else
		{
			if (!DatumGetBool(thisresult))
			{
				result = BoolGetDatum(false);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	*op->resvalue = result;
	*op->resnull = resultnull;
}

/*
 * Evaluate a NOT NULL domain constraint.
 */
void
ExecEvalConstraintNotNull(ExprState *state, ExprEvalStep *op)
{
	if (*op->resnull)
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("domain %s does not allow null values",
						format_type_be(op->d.domaincheck.resulttype)),
				 errdatatype(op->d.domaincheck.resulttype)));
}

/*
 * Evaluate a CHECK domain constraint.
 */
void
ExecEvalConstraintCheck(ExprState *state, ExprEvalStep *op)
{
	if (!*op->d.domaincheck.checknull &&
		!DatumGetBool(*op->d.domaincheck.checkvalue))
		ereport(ERROR,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("value for domain %s violates check constraint \"%s\"",
						format_type_be(op->d.domaincheck.resulttype),
						op->d.domaincheck.constraintname),
				 errdomainconstraint(op->d.domaincheck.resulttype,
									 op->d.domaincheck.constraintname)));
}

/*
 * Evaluate the various forms of XmlExpr.
 *
 * Arguments have been evaluated into named_argvalue/named_argnull
 * and/or argvalue/argnull arrays.
 */
void
ExecEvalXmlExpr(ExprState *state, ExprEvalStep *op)
{
	XmlExpr    *xexpr = op->d.xmlexpr.xexpr;
	Datum		value;
	int			i;

	*op->resnull = true;		/* until we get a result */
	*op->resvalue = (Datum) 0;

	switch (xexpr->op)
	{
		case IS_XMLCONCAT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				List	   *values = NIL;

				for (i = 0; i < list_length(xexpr->args); i++)
				{
					if (!argnull[i])
						values = lappend(values, DatumGetPointer(argvalue[i]));
				}

				if (values != NIL)
				{
					*op->resvalue = PointerGetDatum(xmlconcat(values));
					*op->resnull = false;
				}
			}
			break;

		case IS_XMLFOREST:
			{
				Datum	   *argvalue = op->d.xmlexpr.named_argvalue;
				bool	   *argnull = op->d.xmlexpr.named_argnull;
				StringInfoData buf;
				ListCell   *lc;
				ListCell   *lc2;

				initStringInfo(&buf);

				i = 0;
				forboth(lc, xexpr->named_args, lc2, xexpr->arg_names)
				{
					Expr	   *e = (Expr *) lfirst(lc);
					char	   *argname = strVal(lfirst(lc2));

					if (!argnull[i])
					{
						value = argvalue[i];
						appendStringInfo(&buf, "<%s>%s</%s>",
										 argname,
										 map_sql_value_to_xml_value(value,
																	exprType((Node *) e), true),
										 argname);
						*op->resnull = false;
					}
					i++;
				}

				if (!*op->resnull)
				{
					text	   *result;

					result = cstring_to_text_with_len(buf.data, buf.len);
					*op->resvalue = PointerGetDatum(result);
				}

				pfree(buf.data);
			}
			break;

		case IS_XMLELEMENT:
			*op->resvalue = PointerGetDatum(xmlelement(xexpr,
													   op->d.xmlexpr.named_argvalue,
													   op->d.xmlexpr.named_argnull,
													   op->d.xmlexpr.argvalue,
													   op->d.xmlexpr.argnull));
			*op->resnull = false;
			break;

		case IS_XMLPARSE:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				text	   *data;
				bool		preserve_whitespace;

				/* arguments are known to be text, bool */
				Assert(list_length(xexpr->args) == 2);

				if (argnull[0])
					return;
				value = argvalue[0];
				data = DatumGetTextPP(value);

				if (argnull[1]) /* probably can't happen */
					return;
				value = argvalue[1];
				preserve_whitespace = DatumGetBool(value);

				*op->resvalue = PointerGetDatum(xmlparse(data,
														 xexpr->xmloption,
														 preserve_whitespace));
				*op->resnull = false;
			}
			break;

		case IS_XMLPI:
			{
				text	   *arg;
				bool		isnull;

				/* optional argument is known to be text */
				Assert(list_length(xexpr->args) <= 1);

				if (xexpr->args)
				{
					isnull = op->d.xmlexpr.argnull[0];
					if (isnull)
						arg = NULL;
					else
						arg = DatumGetTextPP(op->d.xmlexpr.argvalue[0]);
				}
				else
				{
					arg = NULL;
					isnull = false;
				}

				*op->resvalue = PointerGetDatum(xmlpi(xexpr->name,
													  arg,
													  isnull,
													  op->resnull));
			}
			break;

		case IS_XMLROOT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				xmltype    *data;
				text	   *version;
				int			standalone;

				/* arguments are known to be xml, text, int */
				Assert(list_length(xexpr->args) == 3);

				if (argnull[0])
					return;
				data = DatumGetXmlP(argvalue[0]);

				if (argnull[1])
					version = NULL;
				else
					version = DatumGetTextPP(argvalue[1]);

				Assert(!argnull[2]);	/* always present */
				standalone = DatumGetInt32(argvalue[2]);

				*op->resvalue = PointerGetDatum(xmlroot(data,
														version,
														standalone));
				*op->resnull = false;
			}
			break;

		case IS_XMLSERIALIZE:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;

				/* argument type is known to be xml */
				Assert(list_length(xexpr->args) == 1);

				if (argnull[0])
					return;
				value = argvalue[0];

				*op->resvalue = PointerGetDatum(
												xmltotext_with_xmloption(DatumGetXmlP(value),
																		 xexpr->xmloption));
				*op->resnull = false;
			}
			break;

		case IS_DOCUMENT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;

				/* optional argument is known to be xml */
				Assert(list_length(xexpr->args) == 1);

				if (argnull[0])
					return;
				value = argvalue[0];

				*op->resvalue =
					BoolGetDatum(xml_is_document(DatumGetXmlP(value)));
				*op->resnull = false;
			}
			break;

		default:
			elog(ERROR, "unrecognized XML operation");
			break;
	}
}

/*
 * ExecEvalGroupingFunc
 *
 * Computes a bitmask with a bit for each (unevaluated) argument expression
 * (rightmost arg is least significant bit).
 *
 * A bit is set if the corresponding expression is NOT part of the set of
 * grouping expressions in the current grouping set.
 */
void
ExecEvalGroupingFunc(ExprState *state, ExprEvalStep *op)
{
	int			result = 0;
	Bitmapset  *grouped_cols = op->d.grouping_func.parent->grouped_cols;
	ListCell   *lc;

	foreach(lc, op->d.grouping_func.clauses)
	{
		int			attnum = lfirst_int(lc);

		result <<= 1;

		if (!bms_is_member(attnum, grouped_cols))
			result |= 1;
	}

	*op->resvalue = Int32GetDatum(result);
	*op->resnull = false;
}

/*
 * Hand off evaluation of a subplan to nodeSubplan.c
 */
void
ExecEvalSubPlan(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	SubPlanState *sstate = op->d.subplan.sstate;

	/* could potentially be nested, so make sure there's enough stack */
	check_stack_depth();

	*op->resvalue = ExecSubPlan(sstate, econtext, op->resnull);
}

/*
 * Hand off evaluation of an alternative subplan to nodeSubplan.c
 */
void
ExecEvalAlternativeSubPlan(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	AlternativeSubPlanState *asstate = op->d.alternative_subplan.asstate;

	/* could potentially be nested, so make sure there's enough stack */
	check_stack_depth();

	*op->resvalue = ExecAlternativeSubPlan(asstate, econtext, op->resnull);
}

/*
 * Evaluate a wholerow Var expression.
 *
 * Returns a Datum whose value is the value of a whole-row range variable
 * with respect to given expression context.
 */
void
ExecEvalWholeRowVar(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	Var		   *variable = op->d.wholerow.var;
	TupleTableSlot *slot;
	TupleDesc	output_tupdesc;
	MemoryContext oldcontext;
	HeapTupleHeader dtuple;
	HeapTuple	tuple;

	/* This was checked by ExecInitExpr */
	Assert(variable->varattno == InvalidAttrNumber);

	/* Get the input slot we want */
	switch (variable->varno)
	{
		case INNER_VAR:
			/* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR:
			/* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:
			/* get the tuple from the relation being scanned */
			slot = econtext->ecxt_scantuple;
			break;
	}

	/* Apply the junkfilter if any */
	if (op->d.wholerow.junkFilter != NULL)
		slot = ExecFilterJunk(op->d.wholerow.junkFilter, slot);

	/*
	 * If first time through, obtain tuple descriptor and check compatibility.
	 *
	 * XXX: It'd be great if this could be moved to the expression
	 * initialization phase, but due to using slots that's currently not
	 * feasible.
	 */
	if (op->d.wholerow.first)
	{
		/* optimistically assume we don't need slow path */
		op->d.wholerow.slow = false;

		/*
		 * If the Var identifies a named composite type, we must check that
		 * the actual tuple type is compatible with it.
		 */
		if (variable->vartype != RECORDOID)
		{
			TupleDesc	var_tupdesc;
			TupleDesc	slot_tupdesc;
			int			i;

			/*
			 * We really only care about numbers of attributes and data types.
			 * Also, we can ignore type mismatch on columns that are dropped
			 * in the destination type, so long as (1) the physical storage
			 * matches or (2) the actual column value is NULL.  Case (1) is
			 * helpful in some cases involving out-of-date cached plans, while
			 * case (2) is expected behavior in situations such as an INSERT
			 * into a table with dropped columns (the planner typically
			 * generates an INT4 NULL regardless of the dropped column type).
			 * If we find a dropped column and cannot verify that case (1)
			 * holds, we have to use the slow path to check (2) for each row.
			 *
			 * If vartype is a domain over composite, just look through that
			 * to the base composite type.
			 */
			var_tupdesc = lookup_rowtype_tupdesc_domain(variable->vartype,
														-1, false);

			slot_tupdesc = slot->tts_tupleDescriptor;

			if (var_tupdesc->natts != slot_tupdesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail_plural("Table row contains %d attribute, but query expects %d.",
										  "Table row contains %d attributes, but query expects %d.",
										  slot_tupdesc->natts,
										  slot_tupdesc->natts,
										  var_tupdesc->natts)));

			for (i = 0; i < var_tupdesc->natts; i++)
			{
				Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
				Form_pg_attribute sattr = TupleDescAttr(slot_tupdesc, i);

				if (vattr->atttypid == sattr->atttypid)
					continue;	/* no worries */
				if (!vattr->attisdropped)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("table row type and query-specified row type do not match"),
							 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
									   format_type_be(sattr->atttypid),
									   i + 1,
									   format_type_be(vattr->atttypid))));

				if (vattr->attlen != sattr->attlen ||
					vattr->attalign != sattr->attalign)
					op->d.wholerow.slow = true; /* need to check for nulls */
			}

			/*
			 * Use the variable's declared rowtype as the descriptor for the
			 * output values, modulo possibly assigning new column names
			 * below. In particular, we *must* absorb any attisdropped
			 * markings.
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(var_tupdesc);
			MemoryContextSwitchTo(oldcontext);

			ReleaseTupleDesc(var_tupdesc);
		}
		else
		{
			/*
			 * In the RECORD case, we use the input slot's rowtype as the
			 * descriptor for the output values, modulo possibly assigning new
			 * column names below.
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(slot->tts_tupleDescriptor);
			MemoryContextSwitchTo(oldcontext);
		}

		/*
		 * Construct a tuple descriptor for the composite values we'll
		 * produce, and make sure its record type is "blessed".  The main
		 * reason to do this is to be sure that operations such as
		 * row_to_json() will see the desired column names when they look up
		 * the descriptor from the type information embedded in the composite
		 * values.
		 *
		 * We already got the correct physical datatype info above, but now we
		 * should try to find the source RTE and adopt its column aliases, in
		 * case they are different from the original rowtype's names.  For
		 * example, in "SELECT foo(t) FROM tab t(x,y)", the first two columns
		 * in the composite output should be named "x" and "y" regardless of
		 * tab's column names.
		 *
		 * If we can't locate the RTE, assume the column names we've got are
		 * OK.  (As of this writing, the only cases where we can't locate the
		 * RTE are in execution of trigger WHEN clauses, and then the Var will
		 * have the trigger's relation's rowtype, so its names are fine.)
		 * Also, if the creator of the RTE didn't bother to fill in an eref
		 * field, assume our column names are OK.  (This happens in COPY, and
		 * perhaps other places.)
		 */
		if (econtext->ecxt_estate &&
			variable->varno <= list_length(econtext->ecxt_estate->es_range_table))
		{
			RangeTblEntry *rte = rt_fetch(variable->varno,
										  econtext->ecxt_estate->es_range_table);

			if (rte->eref)
				ExecTypeSetColNames(output_tupdesc, rte->eref->colnames);
		}

		/* Bless the tupdesc if needed, and save it in the execution state */
		op->d.wholerow.tupdesc = BlessTupleDesc(output_tupdesc);

		op->d.wholerow.first = false;
	}

	/*
	 * Make sure all columns of the slot are accessible in the slot's
	 * Datum/isnull arrays.
	 */
	slot_getallattrs(slot);

	if (op->d.wholerow.slow)
	{
		/* Check to see if any dropped attributes are non-null */
		TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
		TupleDesc	var_tupdesc = op->d.wholerow.tupdesc;
		int			i;

		Assert(var_tupdesc->natts == tupleDesc->natts);

		for (i = 0; i < var_tupdesc->natts; i++)
		{
			Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
			Form_pg_attribute sattr = TupleDescAttr(tupleDesc, i);

			if (!vattr->attisdropped)
				continue;		/* already checked non-dropped cols */
			if (slot->tts_isnull[i])
				continue;		/* null is always okay */
			if (vattr->attlen != sattr->attlen ||
				vattr->attalign != sattr->attalign)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
								   i + 1)));
		}
	}

	/*
	 * Build a composite datum, making sure any toasted fields get detoasted.
	 *
	 * (Note: it is critical that we not change the slot's state here.)
	 */
	tuple = toast_build_flattened_tuple(slot->tts_tupleDescriptor,
										slot->tts_values,
										slot->tts_isnull);
	dtuple = tuple->t_data;

	/*
	 * Label the datum with the composite type info we identified before.
	 *
	 * (Note: we could skip doing this by passing op->d.wholerow.tupdesc to
	 * the tuple build step; but that seems a tad risky so let's not.)
	 */
	HeapTupleHeaderSetTypeId(dtuple, op->d.wholerow.tupdesc->tdtypeid);
	HeapTupleHeaderSetTypMod(dtuple, op->d.wholerow.tupdesc->tdtypmod);

	*op->resvalue = PointerGetDatum(dtuple);
	*op->resnull = false;
}

/*
 * Transition value has not been initialized. This is the first non-NULL input
 * value for a group. We use it as the initial value for transValue.
 */
void
ExecAggInitGroup(AggState *aggstate, AggStatePerTrans pertrans, AggStatePerGroup pergroup)
{
	FunctionCallInfo fcinfo = &pertrans->transfn_fcinfo;
	MemoryContext oldContext;

	/*
	 * We must copy the datum into aggcontext if it is pass-by-ref. We do not
	 * need to pfree the old transValue, since it's NULL.  (We already checked
	 * that the agg's input type is binary-compatible with its transtype, so
	 * straight copy here is OK.)
	 */
	oldContext = MemoryContextSwitchTo(
									   aggstate->curaggcontext->ecxt_per_tuple_memory);
	pergroup->transValue = datumCopy(fcinfo->arg[1],
									 pertrans->transtypeByVal,
									 pertrans->transtypeLen);
	pergroup->transValueIsNull = false;
	pergroup->noTransValue = false;
	MemoryContextSwitchTo(oldContext);
}

/*
 * Ensure that the current transition value is a child of the aggcontext,
 * rather than the per-tuple context.
 *
 * NB: This can change the current memory context.
 */
Datum
ExecAggTransReparent(AggState *aggstate, AggStatePerTrans pertrans,
					 Datum newValue, bool newValueIsNull,
					 Datum oldValue, bool oldValueIsNull)
{
	Assert(newValue != oldValue);

	if (!newValueIsNull)
	{
		MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
		if (DatumIsReadWriteExpandedObject(newValue,
										   false,
										   pertrans->transtypeLen) &&
			MemoryContextGetParent(DatumGetEOHP(newValue)->eoh_context) == CurrentMemoryContext)
			 /* do nothing */ ;
		else
			newValue = datumCopy(newValue,
								 pertrans->transtypeByVal,
								 pertrans->transtypeLen);
	}
	else
	{
		/*
		 * Ensure that AggStatePerGroup->transValue ends up being 0, so
		 * callers can safely compare newValue/oldValue without having to
		 * check their respective nullness.
		 */
		newValue = (Datum) 0;
	}

	if (!oldValueIsNull)
	{
		if (DatumIsReadWriteExpandedObject(oldValue,
										   false,
										   pertrans->transtypeLen))
			DeleteExpandedObject(oldValue);
		else
			pfree(DatumGetPointer(oldValue));
	}

	return newValue;
}

/*
 * Invoke ordered transition function, with a datum argument.
 */
void
ExecEvalAggOrderedTransDatum(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	int			setno = op->d.agg_trans.setno;

	tuplesort_putdatum(pertrans->sortstates[setno],
					   *op->resvalue, *op->resnull);
}

/*
 * Invoke ordered transition function, with a tuple argument.
 */
void
ExecEvalAggOrderedTransTuple(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	int			setno = op->d.agg_trans.setno;

	ExecClearTuple(pertrans->sortslot);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);
	tuplesort_puttupleslot(pertrans->sortstates[setno], pertrans->sortslot);
}

/*
 * helper function to provide a constant numeric ZERO datum,
 * lazy initialized.
 */
static Datum
get_numeric_0_datum(void)
{
	static Datum ZERO = 0;

	if (ZERO == 0)
	{
		MemoryContext oldMemoryContext;

		oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);

		ZERO = DirectFunctionCall1(int8_numeric, Int64GetDatum(0));

		MemoryContextSwitchTo(oldMemoryContext);
	}

	return ZERO;
}

void
ExecEvalCypherTypeCast(ExprState *state, ExprEvalStep *op)
{
	Jsonb	   *argjb;
	JsonbValue *jv;
	char	   *str;
	FunctionCallInfo fcinfo_data_in;
	CypherTypeCast *cfn_expr;
	Oid			typeCastOid;
	TYPCATEGORY typcategory;
	CoercionContext cctx;

	/* validate input */
	Assert(op != NULL);

	if (*op->resnull)
		return;

	argjb = DatumGetJsonbP(*op->resvalue);

	fcinfo_data_in = op->d.cyphertypecast.fcinfo_data_in;
	Assert(fcinfo_data_in != NULL);

	cfn_expr = (CypherTypeCast *) fcinfo_data_in->flinfo->fn_expr;
	Assert(cfn_expr != NULL);

	typeCastOid = fcinfo_data_in->arg[1];
	typcategory = cfn_expr->typcategory;
	cctx = cfn_expr->cctx;

	/*
	 * if the jsonb value is not a scalar, check to see if the
	 * type cast is to boolean
	 */
	if (!JB_ROOT_IS_SCALAR(argjb))
	{
		if (typcategory == TYPCATEGORY_BOOLEAN)
		{
			Assert(JB_ROOT_IS_OBJECT(argjb) || JB_ROOT_IS_ARRAY(argjb));
			*op->resvalue = BoolGetDatum(JB_ROOT_COUNT(argjb) > 0);
			*op->resnull = false;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot cast ... (not scalar)")));
		}
		return;
	}

	/*
	 * get the jsonb value and switch on its type to determine if the
	 * cast is appropriate. Note: We always allow explicit casts.
	 */
	jv = getIthJsonbValueFromContainer(&argjb->root, 0);
	switch (jv->type)
	{
		case jbvNull:
			/* Null can be cast into all others */
			*op->resvalue = (Datum) 0;
			*op->resnull = true;
			return;

		case jbvString:
			/* string to boolean cast */
			if (typcategory == TYPCATEGORY_BOOLEAN)
			{
				*op->resvalue = BoolGetDatum(jv->val.string.len > 0);
				*op->resnull = false;
				return;
			}

			/* string to string and explicit casts */
			if (typcategory == TYPCATEGORY_STRING ||
				cctx == COERCION_EXPLICIT)
				str = pnstrdup(jv->val.string.val, jv->val.string.len);
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_CANNOT_COERCE),
						 errmsg("cannot cast %s (jsonb type string) to %s",
								JsonbToCString(NULL, &argjb->root,
											   VARSIZE(argjb)),
								format_type_be(typeCastOid))));
			}
			break;

		case jbvNumeric:
			/* numeric to boolean casts */
			if (typcategory == TYPCATEGORY_BOOLEAN)
			{
				if (numeric_is_nan(jv->val.numeric))
					*op->resvalue = BoolGetDatum(false);
				else
				{
					Datum		d;
					d = DirectFunctionCall2(numeric_ne, get_numeric_0_datum(),
											NumericGetDatum(jv->val.numeric));
					*op->resvalue = BoolGetDatum(d);
				}
				*op->resnull = false;
				return;
			}

			/* numeric to numeric and explicit casts */
			if (typcategory == TYPCATEGORY_NUMERIC ||
				cctx == COERCION_EXPLICIT)
				str = JsonbToCString(NULL, &argjb->root, VARSIZE(argjb));
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_CANNOT_COERCE),
						 errmsg("cannot cast %s (jsonb type numeric) to %s",
								JsonbToCString(NULL, &argjb->root,
											   VARSIZE(argjb)),
								format_type_be(typeCastOid))));

			}
			break;

		case jbvBool:
			/* check for boolean category or explicit cast */
			if (typcategory == TYPCATEGORY_BOOLEAN ||
				cctx == COERCION_EXPLICIT)
				str = JsonbToCString(NULL, &argjb->root, VARSIZE(argjb));
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_CANNOT_COERCE),
						 errmsg("cannot cast %s (jsonb type bool) to %s",
								JsonbToCString(NULL, &argjb->root,
											   VARSIZE(argjb)),
								format_type_be(typeCastOid))));
			}
			break;

		default:
			elog(ERROR, "unknown jsonb scalar type");
			return;
	}

	fcinfo_data_in->arg[0] = PointerGetDatum(str);
	fcinfo_data_in->argnull[0] = false;

	fcinfo_data_in->isnull = false;
	*op->resvalue = FunctionCallInvoke(fcinfo_data_in);
	*op->resnull = false;
}

void
ExecEvalCypherMapExpr(ExprState *state, ExprEvalStep *op)
{
	char	  **key_cstrings = op->d.cyphermapexpr.key_cstrings;
	Datum	   *val_values = op->d.cyphermapexpr.val_values;
	bool	   *val_nulls = op->d.cyphermapexpr.val_nulls;
	int			npairs = op->d.cyphermapexpr.npairs;
	JsonbParseState *jpstate = NULL;
	int			i;
	JsonbValue *jb;

	pushJsonbValue(&jpstate, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < npairs; i++)
	{
		JsonbIterator *vji;
		JsonbValue	vjv;
		JsonbValue	kjv;

		if (val_nulls[i])
		{
			vji = NULL;
			vjv.type = jbvNull;
		}
		else
		{
			Jsonb	   *vjb = DatumGetJsonbP(val_values[i]);

			vji = JsonbIteratorInit(&vjb->root);

			if (JB_ROOT_IS_SCALAR(vjb))
			{
				JsonbIteratorNext(&vji, &vjv, true);
				Assert(vjv.type == jbvArray);
				JsonbIteratorNext(&vji, &vjv, true);

				vji = NULL;
			}
		}

		if (vji == NULL && vjv.type == jbvNull && !allow_null_properties)
			continue;

		kjv.type = jbvString;
		kjv.val.string.val = key_cstrings[i];
		kjv.val.string.len = strlen(kjv.val.string.val);
		if (kjv.val.string.len > JENTRY_OFFLENMASK)
		{
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("string too long to represent as jsonb string"),
					 errdetail("Due to an implementation restriction, "
							   "jsonb strings cannot exceed %d bytes.",
							   JENTRY_OFFLENMASK)));
			return;
		}
		pushJsonbValue(&jpstate, WJB_KEY, &kjv);

		if (vji == NULL)
		{
			Assert(jpstate->contVal.type == jbvObject);
			pushJsonbValue(&jpstate, WJB_VALUE, &vjv);
		}
		else
		{
			for (;;)
			{
				JsonbValue	ejv;
				JsonbIteratorToken tok;

				tok = JsonbIteratorNext(&vji, &ejv, false);
				if (tok == WJB_DONE)
					break;

				if (tok == WJB_KEY)
				{
					kjv = ejv;

					tok = JsonbIteratorNext(&vji, &ejv, false);
					Assert(tok != WJB_DONE);

					if (tok == WJB_VALUE && ejv.type == jbvNull &&
						!allow_null_properties)
						continue;

					pushJsonbValue(&jpstate, WJB_KEY, &kjv);
				}

				if (tok == WJB_VALUE || tok == WJB_ELEM)
					pushJsonbValue(&jpstate, tok, &ejv);
				else
					pushJsonbValue(&jpstate, tok, NULL);
			}
		}
	}

	jb = pushJsonbValue(&jpstate, WJB_END_OBJECT, NULL);

	*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb(jb));
	*op->resnull = false;
}

void
ExecEvalCypherListExpr(ExprState *state, ExprEvalStep *op)
{
	Datum	   *elemvalues = op->d.cypherlistexpr.elemvalues;
	bool	   *elemnulls = op->d.cypherlistexpr.elemnulls;
	int			nelems = op->d.cypherlistexpr.nelems;
	JsonbParseState *jpstate = NULL;
	int			i;
	JsonbValue *jb;

	pushJsonbValue(&jpstate, WJB_BEGIN_ARRAY, NULL);

	for (i = 0; i < nelems; i++)
	{
		JsonbValue	ejv;
		Jsonb	   *ejb;
		JsonbIterator *eji;

		if (elemnulls[i])
		{
			ejv.type = jbvNull;
			pushJsonbValue(&jpstate, WJB_ELEM, &ejv);
			continue;
		}

		ejb = DatumGetJsonbP(elemvalues[i]);
		eji = JsonbIteratorInit(&ejb->root);
		if (JB_ROOT_IS_SCALAR(ejb))
		{
			JsonbIteratorNext(&eji, &ejv, true);
			Assert(ejv.type == jbvArray);
			JsonbIteratorNext(&eji, &ejv, true);

			Assert(jpstate->contVal.type == jbvArray);
			pushJsonbValue(&jpstate, WJB_ELEM, &ejv);
		}
		else
		{
			for (;;)
			{
				JsonbIteratorToken tok;

				tok = JsonbIteratorNext(&eji, &ejv, false);
				if (tok == WJB_DONE)
					break;

				if (tok == WJB_BEGIN_ARRAY || tok == WJB_END_ARRAY ||
					tok == WJB_BEGIN_OBJECT || tok == WJB_END_OBJECT)
					pushJsonbValue(&jpstate, tok, NULL);
				else
					pushJsonbValue(&jpstate, tok, &ejv);
			}
		}
	}

	jb = pushJsonbValue(&jpstate, WJB_END_ARRAY, NULL);

	*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb(jb));
	*op->resnull = false;
}

void
ExecEvalCypherAccessExpr(ExprState *state, ExprEvalStep *op)
{
	Jsonb	   *argjb;
	JsonbValue	_vjv;
	JsonbValue *vjv;
	CypherAccessPathElem *path;
	int			pathlen;
	int			i;

	/*
	 * The evaluated value of astate->arg might be NULL.
	 * `NULL.p`, `NULL[0]`, ... are NULL.
	 */
	if (*op->d.cypheraccessexpr.argnull)
	{
		*op->resvalue = (Datum) 0;
		*op->resnull = true;
		return;
	}

	argjb = DatumGetJsonbP(*op->d.cypheraccessexpr.argvalue);
	if (JB_ROOT_IS_SCALAR(argjb))
	{
		vjv = getIthJsonbValueFromContainer(&argjb->root, 0);
	}
	else
	{
		_vjv.type = jbvBinary;
		_vjv.val.binary.len = argjb->vl_len_;
		_vjv.val.binary.data = &argjb->root;
		vjv = &_vjv;
	}

	path = op->d.cypheraccessexpr.path;
	pathlen = op->d.cypheraccessexpr.pathlen;
	Assert(pathlen > 0);

	for (i = 0; i < pathlen; i++)
	{
		switch (vjv->type)
		{
			case jbvNull:
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
				return;
			case jbvString:
			case jbvNumeric:
			case jbvBool:
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("map or list is expected but scalar value")));
				return;
			case jbvArray:
				/* sliced array */
				vjv = cypher_access_mem_array(vjv, &path[i]);
				if (vjv == NULL)
				{
					*op->resvalue = (Datum) 0;
					*op->resnull = true;
					return;
				}
				break;
			case jbvObject:
				elog(ERROR, "unexpected jsonb composite type");
				return;
			case jbvBinary:
				{
					JsonbContainer *container = vjv->val.binary.data;

					if (container->header & JB_FOBJECT)
					{
						vjv = cypher_access_object(container, &path[i]);
					}
					else
					{
						Assert(container->header & JB_FARRAY);

						vjv = cypher_access_bin_array(vjv, &path[i]);
					}

					if (vjv == NULL)
					{
						*op->resvalue = (Datum) 0;
						*op->resnull = true;
						return;
					}
				}
				break;
			default:
				elog(ERROR, "unknown jsonb scalar type");
				return;
		}
	}

	if (vjv->type == jbvNull)
	{
		*op->resvalue = (Datum) 0;
		*op->resnull = true;
		return;
	}

	*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb(vjv));
	*op->resnull = false;
}

static JsonbValue *
cypher_access_object(JsonbContainer *container, CypherAccessPathElem *pathelem)
{
	CypherIndexResult *key;
	JsonbValue	_jv;
	JsonbValue *jv;
	JsonbValue	kjv;

	if (pathelem->is_slice)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("slicing map not supported")));
		return NULL;
	}

	key = &pathelem->uidx;
	if (key->isnull)
	{
		_jv.type = jbvNull;
		jv = &_jv;
	}
	else if (key->type == TEXTOID)
	{
		char	   *str = TextDatumGetCString(key->value);

		_jv.type = jbvString;
		_jv.val.string.len = strlen(str);
		_jv.val.string.val = str;
		jv = &_jv;
	}
	else if (key->type == BOOLOID)
	{
		_jv.type = jbvBool;
		_jv.val.boolean = DatumGetBool(key->value);
		jv = &_jv;
	}
	else if (key->type == JSONBOID)
	{
		Jsonb	   *jb = DatumGetJsonbP(key->value);

		if (JB_ROOT_IS_SCALAR(jb))
		{
			jv = getIthJsonbValueFromContainer(&jb->root, 0);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("key value must be scalar")));
			return NULL;
		}
	}
	else
	{
		Assert(!"unexpected key type");
		return NULL;
	}

	kjv.type = jbvString;
	switch (jv->type)
	{
		case jbvNull:
			kjv.val.string.len = 4;
			kjv.val.string.val = "null";
			break;
		case jbvString:
			kjv.val.string = jv->val.string;
			break;
		case jbvNumeric:
			elog(ERROR, "numeric key value not supported");
			return 0;
		case jbvBool:
			if (jv->val.boolean)
			{
				kjv.val.string.len = 4;
				kjv.val.string.val = "true";
			}
			else
			{
				kjv.val.string.len = 5;
				kjv.val.string.val = "false";
			}
			break;
		default:
			elog(ERROR, "unknown jsonb scalar type");
			return 0;
	}

	return findJsonbValueFromContainer(container, JB_FOBJECT, &kjv);
}

static JsonbValue *
cypher_access_bin_array(JsonbValue *ajv, CypherAccessPathElem *pathelem)
{
	JsonbContainer *container = ajv->val.binary.data;
	int			nelems = nelems = container->header & JB_CMASK;
	int			idx;

	if (pathelem->is_slice)
	{
		int			lidx;
		int			uidx;
		JsonbParseState *jpstate = NULL;
		JsonbIterator *ji;
		JsonbValue	jv;
		JsonbIteratorToken jt;

		lidx = cypher_access_range(&pathelem->lidx, nelems, 0);
		uidx = cypher_access_range(&pathelem->uidx, nelems, nelems);

		if (uidx <= lidx)
		{
			JsonbValue *newajv;

			newajv = palloc(sizeof(*newajv));
			newajv->type = jbvArray;
			newajv->val.array.nElems = 0;
			newajv->val.array.elems = NULL;
			newajv->val.array.rawScalar = false;

			return newajv;
		}

		if (uidx - lidx == nelems)
			return ajv;

		pushJsonbValue(&jpstate, WJB_BEGIN_ARRAY, NULL);

		ji = JsonbIteratorInit(container);
		jt = JsonbIteratorNext(&ji, &jv, false);
		Assert(jt == WJB_BEGIN_ARRAY);
		idx = 0;
		for (;;)
		{
			if (idx >= uidx)
				break;

			jt = JsonbIteratorNext(&ji, &jv, true);
			if (jt != WJB_ELEM)
				break;

			if (idx >= lidx)
				pushJsonbValue(&jpstate, WJB_ELEM, &jv);

			idx++;
		}

		return pushJsonbValue(&jpstate, WJB_END_ARRAY, NULL);
	}
	else
	{
		if (nelems == 0)
			return NULL;

		Assert(IsCypherIndexResultValid(&pathelem->uidx));
		idx = cypher_access_index(&pathelem->uidx, nelems);
		if (idx < 0 || idx >= nelems)
			return NULL;

		return getIthJsonbValueFromContainer(container, idx);
	}
}

static JsonbValue *
cypher_access_mem_array(JsonbValue *ajv, CypherAccessPathElem *pathelem)
{
	int			nelems = ajv->val.array.nElems;

	Assert(!ajv->val.array.rawScalar);

	if (pathelem->is_slice)
	{
		int			lidx;
		int			uidx;

		lidx = cypher_access_range(&pathelem->lidx, nelems, 0);
		uidx = cypher_access_range(&pathelem->uidx, nelems, nelems);

		if (uidx <= lidx)
		{
			ajv->val.array.nElems = 0;
			return ajv;
		}

		ajv->val.array.nElems = uidx - lidx;
		ajv->val.array.elems = ajv->val.array.elems + lidx;
		return ajv;
	}
	else
	{
		int			idx;

		if (nelems == 0)
			return NULL;

		Assert(IsCypherIndexResultValid(&pathelem->uidx));
		idx = cypher_access_index(&pathelem->uidx, nelems);
		if (idx < 0 || idx >= nelems)
			return NULL;

		return ajv->val.array.elems + idx;
	}

	return NULL;
}

static int
cypher_access_range(CypherIndexResult *cidxres, const int nelems,
					const int defidx)
{
	int			idx;

	if (!IsCypherIndexResultValid(cidxres))
		return defidx;

	idx = cypher_access_index(cidxres, nelems);
	if (idx < 0)
		return 0;
	else if (idx > nelems)
		return nelems;
	else
		return idx;
}

static int
cypher_access_index(CypherIndexResult *cidxres, const int nelems)
{
	char	   *typestr;

	if (cidxres->type == JSONBOID)
	{
		JsonbValue	_ijv;
		JsonbValue *ijv;

		if (cidxres->isnull)
		{
			_ijv.type = jbvNull;
			ijv = &_ijv;
		}
		else
		{
			Jsonb	   *jb = DatumGetJsonbP(cidxres->value);

			if (!JB_ROOT_IS_SCALAR(jb))
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("index value must be scalar")));
				return -1;
			}

			ijv = getIthJsonbValueFromContainer(&jb->root, 0);
		}

		if (ijv->type == jbvNumeric)
		{
			Datum		n = NumericGetDatum(ijv->val.numeric);
			int			idx;

			if (DatumGetInt32(DirectFunctionCall1(numeric_scale, n)) > 0)
			{
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("integer is expected for index value")));
				return -1;
			}

			idx = DatumGetInt32(DirectFunctionCall1(numeric_int4, n));
			if (idx < 0)
				idx += nelems;

			return idx;
		}
		else if (ijv->type == jbvNull)
		{
			typestr = "null";
		}
		else if (ijv->type == jbvString)
		{
			typestr = "string";
		}
		else if (ijv->type == jbvBool)
		{
			typestr = "boolean";
		}
		else
		{
			elog(ERROR, "unknown jsonb scalar type");
			return -1;
		}
	}
	else if (cidxres->type == TEXTOID)
	{
		typestr = "text";
	}
	else if (cidxres->type == BOOLOID)
	{
		typestr = "bool";
	}
	else
	{
		Assert(!"unexpected index type");
		return -1;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("%s cannot be index value", typestr)));
	return -1;
}
