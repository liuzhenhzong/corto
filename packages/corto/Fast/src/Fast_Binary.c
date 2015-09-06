/* Fast_Binary.c
 *
 * This file contains the implementation for the generated interface.
 *
 * Don't mess with the begin and end tags, since these will ensure that modified
 * code in interface functions isn't replaced when code is re-generated.
 */

#define corto_Fast_LIB
#include "Fast.h"

/* $header() */
#include "Fast__private.h"

/* Determine whether the expression is an arithmic operation */
cx_bool Fast_Binary_isArithmic(Fast_Binary expr) {
    cx_bool result;
    switch(expr->operator) {
    case CX_ADD:
    case CX_SUB:
    case CX_MUL:
    case CX_DIV:
    case CX_MOD:
    case CX_AND:
    case CX_OR:
    case CX_XOR:
        result = TRUE;
        break;
    default:
        result = FALSE;
        break;
    }
    return result;
}

/* Determine whether expression is an assignment */
cx_bool Fast_Binary_isConditional(Fast_Binary expr) {
    cx_bool result;
    switch(expr->operator) {
    case CX_COND_EQ:
    case CX_COND_NEQ:
    case CX_COND_LT:
    case CX_COND_LTEQ:
    case CX_COND_GT:
    case CX_COND_GTEQ:
    case CX_COND_AND:
    case CX_COND_OR:
    case CX_COND_NOT:
        result = TRUE;
        break;
    default:
        result = FALSE;
        break;
    }
    return result;
}

cx_bool Fast_isNull(Fast_Expression e) {
    return (Fast_Node(e)->kind == Fast_LiteralExpr) && (Fast_Literal(e)->kind == Fast_Nothing);
}

/* Get deref mode for expression */
cx_int16 Fast_Binary_getDerefKind(Fast_Binary this, cx_type lvalueType, cx_type rvalueType) {

    if ((this->lvalue->deref == Fast_ByReference) && Fast_isNull(this->rvalue)) {
        this->deref = Fast_ByReference;
    } else if ((this->rvalue->deref == Fast_ByReference) && Fast_isNull(this->lvalue)) {
        this->deref = Fast_ByReference;
    } else if (!Fast_isOperatorAssignment(this->operator)) {
        if (this->lvalue->deref != this->rvalue->deref) {
            if (lvalueType->kind == CX_VOID) {
                if (this->rvalue->isReference) {
                    this->deref = Fast_ByReference;
                } else {
                    Fast_Parser_error(yparser(), "cannot access right operand by reference");
                    goto error;
                }
            } else if (rvalueType->kind == CX_VOID) {
                if (this->lvalue->isReference) {
                    this->deref = Fast_ByReference;
                } else {
                    Fast_Parser_error(yparser(), "cannot access left operand by reference");
                    goto error;
                }
            } else {
                Fast_Parser_error(yparser(), "inconsistent usage of references (left = %s, right = %s)",
                    this->lvalue->deref == Fast_ByReference ? "by address" : "by value",
                    this->rvalue->deref == Fast_ByReference ? "by address" : "by value");
                goto error;
            }
        } else {
            this->deref = this->lvalue->deref;
        }
    } else {
        if (Fast_Node(this->rvalue)->kind != Fast_InitializerExpr) {
            if ((this->lvalue->deref == Fast_ByReference) && (!this->rvalue->isReference)) {
                cx_id id;
                Fast_Parser_error(yparser(), "cannot access right operand by reference (type = '%s', kind = '%s')",
                    Fast_Parser_id(rvalueType, id),
                        cx_nameof(cx_enum_constant(Fast_nodeKind_o, Fast_Node(this->rvalue)->kind)));
                goto error;
            }

            this->deref = this->lvalue->deref;
        } else {
            /* Initializer expressions are always by value */
            this->deref = Fast_ByValue;
        }
    }

    return 0;
error:
    return -1;
}

/* If types of a binary expression don't match, figure out which casts are required */
cx_int16 Fast_Binary_cast(Fast_Binary this, cx_type *returnType) {
    cx_type lvalueType, rvalueType, castType = NULL;
    Fast_Expression lvalue, rvalue;
    cx_bool referenceMismatch = FALSE;
    cx_bool equal = FALSE;
    cx_bool isNull;

    /* Get lvalueType and rvalueType */
    lvalue = this->lvalue;
    rvalue = this->rvalue;
    if (!(lvalueType = Fast_Expression_getType(lvalue))) {
        goto error;
    }

    if (!(rvalueType = Fast_Expression_getType_expr(rvalue, lvalue))) {
        goto error;
    }

    isNull = cx_instanceof(cx_type(Fast_Null_o), lvalue) ||
        cx_instanceof(cx_type(Fast_Null_o), rvalue);

    /* Narrow expressions where possible */
    if (!isNull) {
        lvalue = Fast_Expression_narrow(lvalue, rvalueType);
        if (lvalue) {
            cx_setref(&this->lvalue, lvalue);
            lvalueType = Fast_Expression_getType(this->lvalue);
        }

        rvalue = Fast_Expression_narrow(rvalue, lvalueType);
        if (rvalue) {
            cx_setref(&this->rvalue, rvalue);
            rvalueType = Fast_Expression_getType_expr(this->rvalue, this->lvalue);
        }
    }

    if (!lvalueType) {
        lvalueType = rvalueType;
    }

    /* If objects are not scoped, verify whether they're equal */
    if (!cx_checkAttr(lvalueType, CX_ATTR_SCOPED) && !cx_checkAttr(rvalueType, CX_ATTR_SCOPED)) {
        if (cx_compare(lvalueType, rvalueType) == CX_EQ) {
            equal = TRUE;
        }
    } else {
        equal = lvalueType == rvalueType;
    }

    if (this->operator == CX_DIV) {
        castType = cx_type(cx_float64_o);
    } else if (!equal) {
        Fast_nodeKind lKind = Fast_Node(this->lvalue)->kind;
        Fast_nodeKind rKind = Fast_Node(this->rvalue)->kind;

        /* If one of the operands is a literal, always cast to the type of the non-literal */
        if ((lKind == Fast_LiteralExpr) ^ (rKind == Fast_LiteralExpr)) {
            if (lKind == Fast_LiteralExpr) {
                castType = rvalueType;
            } else {
                castType = lvalueType;
            }
        } else

        /* Can only cast between primitive types */
        if ((lvalueType->kind == CX_PRIMITIVE ) && (rvalueType->kind == CX_PRIMITIVE)) {
            cx_primitive ltype = cx_primitive(lvalueType), rtype = cx_primitive(rvalueType);
            cx_int8 lscore, rscore;
            cx_int8 lCastScore, rCastScore;

            lscore = Fast_Expression_getTypeScore(ltype);
            rscore = Fast_Expression_getTypeScore(rtype);
            lCastScore = Fast_Expression_getCastScore(ltype);
            rCastScore = Fast_Expression_getCastScore(rtype);

            /* If expression is an assignment, always take type of lvalue. Otherwise determine based on
             * expressibility score which type to cast to.
             */
            if (Fast_isOperatorAssignment(this->operator)) {
                if (lCastScore == rCastScore) {
                    if (ltype->width != rtype->width) {
                        castType = lvalueType;
                    }
                } else {
                    castType = lvalueType;
                }
            } else {
                if (lCastScore == rCastScore) {
                    if (ltype->width == rtype->width) {
                        /* If width and kind are equal, no conversion is required. */
                        if (lscore > rscore) {
                            *returnType = lvalueType;
                        } else if (lscore < rscore) {
                            *returnType = rvalueType;
                        }
                    } else {
                        /* If lvalue has a larger width than rvalue, cast rvalue to lvalueType (and vice versa) */
                        if (ltype->width > rtype->width) {
                            castType = lvalueType;
                        } else {
                            castType = rvalueType;
                        }
                    }

                /* If kinds do not match figure a cast is potentially needed. Figure out which type to cast to */
                } else {
                    if (lscore > rscore) {
                        castType = lvalueType;
                    } else {
                        castType = rvalueType;
                    }
                }
            }
        } else if (lvalueType->reference && rvalueType->reference) {
            castType = NULL;
        /* Check if types are compatible */


        } else if ((rvalueType->reference || (this->rvalue->deref == Fast_ByReference)) && !lvalueType->reference) {
            /* Enable casting references to boolean values */
            if (cx_type_castable(lvalueType, cx_object_o)) {
                castType = lvalueType;
            }
        } else {
            cx_id id1, id2;
            Fast_Parser_error(yparser(), "cannot cast from '%s' to '%s'",
                    Fast_Parser_id(lvalueType, id1), Fast_Parser_id(rvalueType, id2));
            goto error;
        }
    } else if ((rvalueType->reference || (this->rvalue->deref == Fast_ByReference)) && !lvalueType->reference) {
        if (cx_type_castable(lvalueType, cx_object_o)) {
            castType = lvalueType;
            referenceMismatch = TRUE;
        }
    }

    /* Cast-score indicates whether a cast is required */
    if (rvalueType && castType && ((rvalueType != castType) || referenceMismatch)) {
        Fast_Expression oldRvalue = this->rvalue;
        this->rvalue = Fast_Expression_cast(this->rvalue, castType, this->lvalue->deref == Fast_ByReference);
        if (this->rvalue) {
            cx_claim(this->rvalue);
            cx_release(oldRvalue);
        } else {
            cx_id id, id2;
            Fast_Parser_error(yparser(), "cannot cast from '%s' to '%s'",
                    Fast_Parser_id(rvalueType, id), Fast_Parser_id(castType, id2));
            goto error;
        }
    }
    if (lvalueType && castType && (lvalueType != castType)) {
        Fast_Expression oldLvalue = this->lvalue;
        this->lvalue = Fast_Expression_cast(this->lvalue, castType, this->rvalue->deref == Fast_ByReference);
        if (this->lvalue) {
            cx_claim(this->lvalue);
            cx_release(oldLvalue);
        } else {
            cx_id id, id2;
            Fast_Parser_error(yparser(), "cannot cast from '%s' to '%s'",
                    Fast_Parser_id(lvalueType, id), Fast_Parser_id(castType, id2));
            goto error;
        }
    }

    if (castType) {
        *returnType = castType;
    }

    return 0;
error:
    return -1;
}

cx_int16 Fast_Binary_complexExprCompare(Fast_Binary this) {
    Fast_Expression compareResult = NULL;
    Fast_Expression c1 = NULL, c2 = NULL;
    compareResult = Fast_Expression(Fast_createCall(this->lvalue, "compare", 1, this->rvalue));
    if (!compareResult) {
        goto error;
    }
    switch (this->operator) {
        case CX_COND_EQ:
            c1 = Fast_Expression(Fast_IntegerCreate(CX_EQ));
            break;
        case CX_COND_LT:
            c1 = Fast_Expression(Fast_IntegerCreate(CX_LT));
            break;
        case CX_COND_GT:
            c1 = Fast_Expression(Fast_IntegerCreate(CX_GT));
            break;
        case CX_COND_LTEQ:
            c1 = Fast_Expression(Fast_IntegerCreate(CX_LT));
            c2 = Fast_Expression(Fast_IntegerCreate(CX_EQ));
            break;
        case CX_COND_GTEQ:
            c1 = Fast_Expression(Fast_IntegerCreate(CX_GT));
            c2 = Fast_Expression(Fast_IntegerCreate(CX_EQ));
            break;
        case CX_COND_NEQ:
            c1 = Fast_Expression(Fast_IntegerCreate(CX_LT));
            c2 = Fast_Expression(Fast_IntegerCreate(CX_GT));
            break;
        default:
            break;
    }

    Fast_Expression c1Result = NULL, c2Result = NULL, orResult, result;
    c1Result = Fast_Expression(Fast_BinaryCreate(compareResult, c1, CX_COND_EQ));
    if (!c1Result) {
        goto error;
    }
    result = c1Result;
    if (c2) {
        c2Result = Fast_Expression(Fast_BinaryCreate(compareResult, c2, CX_COND_EQ));
        if (!c2Result) {
            goto error;
        }
        orResult = Fast_Expression(Fast_BinaryCreate(c1Result, c2Result, CX_OR));
        if (!orResult) {
            goto error;
        }
        result = orResult;
    }
    Fast_Parser_addStatement(yparser(), result);
    return 0;
error:
    return -1;
}

cx_int16 Fast_Binary_complexExpr(Fast_Binary this) {
    if (this->operator == CX_ASSIGN) {
        Fast_Expression result = NULL;
        if(Fast_Node(this->rvalue)->kind == Fast_InitializerExpr) {
            if (Fast_InitializerExpression_insert(Fast_InitializerExpression(this->rvalue), this->lvalue)) {
                goto error;
            }
        } else {
            if (!(result = Fast_Expression(Fast_createCall(this->lvalue, "copy", 1, this->rvalue)))) {
                goto error;
            }
            Fast_Parser_addStatement(yparser(), result);
        }
    } else if (this->operator == CX_COND_EQ || this->operator == CX_COND_EQ ||
            this->operator == CX_COND_NEQ || this->operator == CX_COND_GT ||
            this->operator == CX_COND_LT || this->operator == CX_COND_GTEQ ||
            this->operator == CX_COND_LTEQ) {
        if (Fast_Binary_complexExprCompare(this)) {
            goto error;
        }
    } else {
        Fast_Parser_error(yparser(), "operator invalid for complex value");
        goto error;
    }

    return 0;
error:
    return -1;
}

cx_int16 Fast_Binary_toIc_strOp(
        Fast_Binary this,
        ic_program program,
        ic_storage storage,
        ic_node lvalue,
        ic_node rvalue) {

    switch(this->operator) {
    /* Append strings */
    case CX_ADD: {
        ic_literal dummy = ic_literalCreate((cx_any){cx_type(cx_string_o), NULL, TRUE});
        IC_2(program, Fast_Node(this)->line, ic_strcat, lvalue, rvalue, IC_DEREF_VALUE, IC_DEREF_VALUE);
        IC_2(program, Fast_Node(this)->line, ic_strcpy, storage, dummy, IC_DEREF_VALUE, IC_DEREF_VALUE);
        break;
    }
    case CX_ASSIGN:
        IC_3(program, Fast_Node(this)->line, ic_opKindFromOperator(this->operator),
            storage, lvalue, rvalue, IC_DEREF_VALUE, IC_DEREF_VALUE, IC_DEREF_VALUE);
        break;
    default:
        Fast_Parser_error(yparser(), "operator '%s' invalid for strings",
            cx_nameof(cx_enum_constant(cx_operatorKind_o, this->operator)));
        goto error;
    }

    return 0;
error:
    return -1;
}

/* $end */

/* ::corto::Fast::Binary::construct() */
cx_int16 _Fast_Binary_construct(Fast_Binary this) {
/* $begin(::corto::Fast::Binary::construct) */
    cx_type lvalueType, rvalueType;

    Fast_Node(this)->kind = Fast_BinaryExpr;
    if (!(lvalueType = Fast_Expression_getType_expr(this->lvalue, this->rvalue))) {
        goto error;
    }
    if (!(rvalueType = Fast_Expression_getType_expr(this->rvalue, this->lvalue))) {
        goto error;
    }

    /* Check if operands are valid in case of arithmic operation */
    if (Fast_Binary_isArithmic(this)) {
        if ((lvalueType->kind != CX_PRIMITIVE) || (rvalueType->kind != CX_PRIMITIVE) ||
            ((this->lvalue->deref == Fast_ByReference) || (this->lvalue->deref == Fast_ByReference))) {
            Fast_Parser_error(yparser(), "invalid operands for arithmic operation");
            goto error;
        }
    }

    /* Check if lvalue is valid in case of assignment */
    if (Fast_isOperatorAssignment(this->operator) && (Fast_Node(this->lvalue)->kind != Fast_StorageExpr)) {
        Fast_Parser_error(yparser(), "invalid lvalue");
        goto error;
    }

    if (lvalueType && rvalueType) {
        if (!cx_type_castable(lvalueType, rvalueType)) {
            cx_id id, id2;
            Fast_Parser_error(yparser(), "cannot convert '%s' to '%s'",
                    Fast_Parser_id(rvalueType, id), Fast_Parser_id(lvalueType, id2));
            goto error;
        }

        if (Fast_Binary_getDerefKind(this, lvalueType, rvalueType)) {
            goto error;
        }
    }

    /* Set operator */
    Fast_Binary_setOperator(this, this->operator);

    return 0;
error:
    return -1;
/* $end */
}

/* ::corto::Fast::Binary::fold() */
Fast_Expression _Fast_Binary_fold(Fast_Binary this) {
/* $begin(::corto::Fast::Binary::fold) */
    Fast_Expression result = Fast_Expression(this);
    void *lptr, *rptr, *resultPtr;
    cx_type type, rtype;

    if(!this->lvalue || !this->rvalue) {
        Fast_Parser_error(yparser(), "invalid binary expression");
        goto error;
    }

    type = Fast_Expression_getType_expr(this->lvalue, this->rvalue);
    rtype = Fast_Expression_getType_expr(this->rvalue, this->lvalue);

    lptr = (void*)Fast_Expression_getValue(this->lvalue);
    rptr = (void*)Fast_Expression_getValue(this->rvalue);

    if (!lptr && !rptr) {
        /* If either lvalue or rvalue do not have a compile-time defined value,
         * expression cannot be folded.
         * Only exception is null-literal of which the value is 0 */
        if ((Fast_Node(this->lvalue)->kind == Fast_LiteralExpr) &&
           (Fast_Node(this->rvalue)->kind == Fast_LiteralExpr)) {
                if (Fast_Binary_isConditional(this)) {
                    result = Fast_Expression(Fast_BooleanCreate(FALSE));
                    resultPtr = (void*)Fast_Literal_getValue(Fast_Literal(result));
                    cx_binaryOperator(cx_object_o, this->operator, &lptr, &rptr, resultPtr);
                } else {
                    result = Fast_Expression(Fast_NullCreate());
                }
           }
    } else if (lptr && rptr) {
        if (type != rtype) {
            Fast_Expression rvalueCast = Fast_Expression_cast(this->rvalue, type, this->lvalue->isReference);
            rptr = (void*)Fast_Expression_getValue(rvalueCast);
        }

        /* Create result-expression */
        if (type->kind == CX_PRIMITIVE) {
            if (Fast_Binary_isConditional(this)) {
                result = Fast_Expression(Fast_BooleanCreate(FALSE));
            } else {
                switch(cx_primitive(type)->kind) {
                case CX_BOOLEAN: result = Fast_Expression(Fast_BooleanCreate(FALSE)); break;
                case CX_CHARACTER: result = Fast_Expression(Fast_CharacterCreate('a')); break;
                case CX_BITMASK:
                case CX_UINTEGER: result = Fast_Expression(Fast_IntegerCreate(0)); break;
                case CX_ENUM:
                case CX_INTEGER: result = Fast_Expression(Fast_SignedIntegerCreate(0)); break;
                case CX_FLOAT: result = Fast_Expression(Fast_FloatingPointCreate(0)); break;
                case CX_TEXT: result = Fast_Expression(Fast_StringCreate(NULL)); break;
                default:
                    Fast_Parser_error(yparser(), "Invalid primitive for folding expression");
                    goto error;
                    break;
                }
                if ((cx_primitive(type)->kind == CX_BITMASK) || (cx_primitive(type)->kind == CX_ENUM)) {
                    cx_setref(&Fast_Expression(result)->type, type);
                }
            }

            /* Collect new expression */
            Fast_Parser_collect(yparser(), result);

            /* Obtain pointer to value-field */
            resultPtr = (void*)Fast_Literal_getValue(Fast_Literal(result));

            /* Perform operation */
            if (cx_binaryOperator(type, this->operator, lptr, rptr, resultPtr)) {
                cx_id id;
                Fast_Parser_error(yparser(), "folding of binary %s operation failed", Fast_Parser_id(type, id));
                goto error;
            }
        } else {
            /* Expressions of non-primitive type are not folded. */
        }
    }

    return result;
error:
    yparser()->errSet = TRUE;
    return NULL;
/* $end */
}

/* ::corto::Fast::Binary::hasReturnedResource() */
cx_bool _Fast_Binary_hasReturnedResource_v(Fast_Binary this) {
/* $begin(::corto::Fast::Binary::hasReturnedResource) */

    return Fast_Expression_hasReturnedResource(this->lvalue) ||
        Fast_Expression_hasReturnedResource(this->rvalue);

/* $end */
}

/* ::corto::Fast::Binary::hasSideEffects() */
cx_bool _Fast_Binary_hasSideEffects_v(Fast_Binary this) {
/* $begin(::corto::Fast::Binary::hasSideEffects) */
    cx_bool result = FALSE;

    switch(this->operator) {
        case CX_ASSIGN:
        case CX_ASSIGN_ADD:
        case CX_ASSIGN_SUB:
        case CX_ASSIGN_DIV:
        case CX_ASSIGN_MUL:
        case CX_ASSIGN_MOD:
        case CX_ASSIGN_OR:
        case CX_ASSIGN_AND:
            result = TRUE;
            break;
        default:
            break;
    }

    return result || Fast_Expression_hasSideEffects(this->lvalue) || Fast_Expression_hasSideEffects(this->rvalue);
/* $end */
}

/* ::corto::Fast::Binary::setOperator(operatorKind kind) */
cx_void _Fast_Binary_setOperator(Fast_Binary this, cx_operatorKind kind) {
/* $begin(::corto::Fast::Binary::setOperator) */
    Fast_Binary compoundExpr = NULL;
    cx_type exprType = NULL;

    exprType = Fast_Expression_getType_expr(this->lvalue, this->rvalue);

    /* Assign operator before cast - it can influence the type */
    this->operator = kind;

    /* If operator is a compound operator (assign_*), split up in two binary expressions */
    switch(this->operator) {
    case CX_ASSIGN_ADD: compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_ADD); break;
    case CX_ASSIGN_SUB: compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_SUB); break;
    case CX_ASSIGN_DIV: compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_DIV); break;
    case CX_ASSIGN_MUL: compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_MUL); break;
    case CX_ASSIGN_MOD: compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_MOD); break;
    case CX_ASSIGN_OR:  compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_OR); break;
    case CX_ASSIGN_AND: compoundExpr = Fast_BinaryCreate(this->lvalue, this->rvalue, CX_AND); break;
        break;
    default:
        break;
    }

    if (compoundExpr) {
        this->operator = CX_ASSIGN;
        cx_setref(&this->rvalue, compoundExpr);
        Fast_Parser_collect(yparser(), compoundExpr);
    }

    /* Apply casting on operands if necessary */
    if (exprType && (exprType->kind != CX_ITERATOR)) {
        if (Fast_Binary_cast(this, &exprType)) {
            goto error;
        }
    }

    /* Depending on the operator, the returnType might be a bool or the type of the lvalue */
    switch(this->operator) {
    case CX_COND_EQ:
    case CX_COND_NEQ:
    case CX_COND_LT:
    case CX_COND_GT:
    case CX_COND_LTEQ:
    case CX_COND_GTEQ:
    case CX_COND_AND:
    case CX_COND_OR:
        cx_setref(&Fast_Expression(this)->type, cx_bool_o);
        break;
    default:
        cx_setref(&Fast_Expression(this)->type, exprType);
        break;
    }

    /* If the expression type is not a scalar, insert complex operations */
    if ((exprType->kind != CX_PRIMITIVE) && (this->deref != Fast_ByReference) &&
        (Fast_Node(this->rvalue)->kind != Fast_CallExpr)) {
        if (Fast_Binary_complexExpr(this)) {
            goto error;
        }
        this->isScalar = FALSE;
    } else {
        this->isScalar = TRUE;
    }

error:
    return;
/* $end */
}

/* ::corto::Fast::Binary::toIc(ic::program program,ic::storage storage,bool stored) */
ic_node _Fast_Binary_toIc_v(Fast_Binary this, ic_program program, ic_storage storage, cx_bool stored) {
/* $begin(::corto::Fast::Binary::toIc) */
    ic_node returnsResult = NULL;

    if (this->isScalar) {
        ic_node lvalue, rvalue, result, conditionLvalue, conditionRvalue = NULL;
        cx_type thisType = Fast_Expression_getType(Fast_Expression(this));
        cx_bool condition = Fast_Binary_isConditional(this);
        cx_bool isReference = (this->lvalue->deref == Fast_ByReference) || (this->rvalue->deref == Fast_ByReference);
        ic_derefKind deref = this->deref == Fast_ByReference ? IC_DEREF_ADDRESS : IC_DEREF_VALUE;

        if (storage && (storage->type == thisType)) {
            result = (ic_node)storage;
        } else {
            result = (ic_node)ic_program_pushAccumulator(program, thisType, isReference, FALSE);
        }

        returnsResult = result;

        if (condition) {
            conditionLvalue = (ic_node)ic_program_pushAccumulator(program, Fast_Expression_getType(this->lvalue), this->lvalue->isReference, FALSE);
            conditionRvalue = (ic_node)ic_program_pushAccumulator(program, Fast_Expression_getType(this->rvalue), this->rvalue->isReference, FALSE);
        }

        lvalue = Fast_Node_toIc(
                Fast_Node(this->lvalue),
                program,
                condition ? (ic_storage)conditionLvalue : (ic_storage)result,
                TRUE);

        if (this->operator == CX_ASSIGN) {
            rvalue = Fast_Node_toIc(Fast_Node(this->rvalue), program, (ic_storage)lvalue, TRUE);
            if (lvalue != rvalue) {
                IC_3(program, Fast_Node(this)->line, ic_set,
                    stored ? result : NULL, lvalue, rvalue, IC_DEREF_VALUE, deref, deref);
            } else {
                returnsResult = rvalue;
            }
        } else if (stored && result) {
            rvalue = Fast_Node_toIc(Fast_Node(this->rvalue), program, (ic_storage)conditionRvalue, TRUE);
            if ((thisType->kind == CX_PRIMITIVE) && (cx_primitive(thisType)->kind == CX_TEXT)) {
                if (Fast_Binary_toIc_strOp(this, program, (ic_storage)result, ic_node(lvalue), ic_node(rvalue))) {
                    goto error;
                }
            } else {
                IC_3(program, Fast_Node(this)->line, ic_opKindFromOperator(this->operator), result, lvalue, rvalue,
                    IC_DEREF_VALUE, deref, deref);
            }
        } else {
            if (Fast_Expression_hasSideEffects(this->rvalue)) {
                rvalue = Fast_Node_toIc(Fast_Node(this->rvalue), program, (ic_storage)conditionRvalue, FALSE);
            }
        }

        if (condition) {
            ic_program_popAccumulator(program);
            ic_program_popAccumulator(program);
        }

        if ((ic_node)storage != result) {
            ic_program_popAccumulator(program);
        }
    } else {
        returnsResult = Fast_Node_toIc(
            Fast_Node(this->lvalue),
            program,
            NULL,
            TRUE);
    }

    return returnsResult;
error:
    return NULL;

/* $end */
}
