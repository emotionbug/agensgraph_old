/*
 * parse_cypher_utils.h
 *
 * Copyright (c) 2022 by Bitnine Global, Inc.
 *
 * IDENTIFICATION
 *	  src/include/parser/parse_cypher_utils.h
 */

#ifndef AGENSGRAPH_PARSE_CYPHER_UTILS_H
#define AGENSGRAPH_PARSE_CYPHER_UTILS_H

#include "parser/parse_node.h"

FuncExpr *makeJsonbFuncAccessor(Node *expr, List *path);
bool IsJsonbAccessor(Node *expr);
void getAccessorArguments(Node *node, Node **expr, List **path);
FuncExpr *makeJsonbSliceFunc(Node *expr, Node *lidx, Node *uidx);

#endif
