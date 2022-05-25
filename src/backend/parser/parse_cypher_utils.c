/*
 * parse_cypher_utils.c
 *
 * Copyright (c) 2022 by Bitnine Global, Inc.
 *
 * IDENTIFICATION
 *	  src/include/parser/parse_cypher_utils.c
 */

#include "postgres.h"

#include "parser/parse_cypher_utils.h"
#include "catalog/pg_type_d.h"
#include "utils/fmgroids.h"
#include "nodes/makefuncs.h"
#include "parser/parse_relation.h"

FuncExpr *makeJsonbFuncAccessor(ParseState *pstate, Node *expr, List *path)
{
	FuncExpr *funcExpr;
	ArrayExpr *arrayExpr = makeNode(ArrayExpr);
	arrayExpr->array_typeid = TEXTARRAYOID;
	arrayExpr->element_typeid = TEXTOID;
	arrayExpr->elements = path;
	arrayExpr->multidims = false;
	arrayExpr->location = -1;

	if (IsA(expr, FieldSelect))
	{
		FieldSelect *fieldSelect = (FieldSelect *) expr;
		if (IsA(fieldSelect->arg, Var))
		{
			Var *var = fieldSelect->arg;
			if (var->varlevelsup == 0)
			{
				RangeTblEntry *rte;
				TargetEntry *te;

				rte = GetRTEByRangeTablePosn(pstate, var->varno, 0);
				Assert(rte->rtekind == RTE_SUBQUERY);

				te = list_nth(rte->subquery->targetList, var->varattno - 1);
				if (IsA(te->expr, RowExpr))
				{
					ListCell *l;
					int i = 0;
					foreach(l, rte->subquery->targetList)
					{
						TargetEntry *te = (TargetEntry *) lfirst(l);
						i++;
						if (strcmp(te->resname, "TEST") == 0 && IsA(te->expr, Var))
						{
							Var *tevar = te->expr;
//							expr = (Node *) makeVar(i, tevar->varattno, tevar->vartype, tevar->vartypmod, tevar->varcollid,
//							                        tevar->varlevelsup);
						}
					}
				}
			}
		}
	}

	funcExpr = makeFuncExpr(F_CYPHER_JSONB_EXTRACT_PATH,
	                        JSONBOID,
	                        list_make2(expr, arrayExpr),
	                        InvalidOid,
	                        InvalidOid,
	                        COERCE_EXPLICIT_CALL);
	funcExpr->funcvariadic = false;
	return funcExpr;
}

bool IsJsonbAccessor(Node *expr)
{
	if (IsA(expr, FuncExpr))
	{
		FuncExpr *funcExpr = (FuncExpr *) expr;
		Oid funcId = funcExpr->funcid;

		if (funcId == F_CYPHER_JSONB_EXTRACT_PATH || funcId == F_JSONB_EXTRACT_PATH_TEXT)
		{
			return true;
		}
	}

	return false;
}

void getAccessorArguments(Node *node, Node **expr, List **path)
{
	if (IsA(node, FuncExpr))
	{
		FuncExpr *funcExpr = (FuncExpr *) node;
		Oid funcId = funcExpr->funcid;

		if (funcId == F_CYPHER_JSONB_EXTRACT_PATH || funcId == F_JSONB_EXTRACT_PATH_TEXT)
		{
			List *funcArgs = funcExpr->args;
			ListCell *listHead = list_head(funcArgs);
			ArrayExpr *arrayExpr;

			*expr = lfirst(listHead);
			arrayExpr = lfirst(lnext(listHead));

			*path = arrayExpr->elements;
		}
	}
	else
	{
		elog(ERROR, "cannot extract elements from node");
	}
}

FuncExpr *makeJsonbSliceFunc(Node *expr, Node *lidx, Node *uidx)
{
	return makeFuncExpr(F_JSONB_SLICE,
						JSONBOID,
						list_make3(expr, lidx, uidx),
						InvalidOid,
						InvalidOid,
						COERCE_EXPLICIT_CALL);
}