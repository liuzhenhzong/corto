/* ast_StaticInitializer.c
 *
 * This file contains the implementation for the generated interface.
 *
 * Don't mess with the begin and end tags, since these will ensure that modified
 * code in interface functions isn't replaced when code is re-generated.
 */

#include "ast.h"

/* $header() */
#include "ast__private.h"

cx_word ast_Initializer_offset(ast_StaticInitializer this, cx_uint32 variable) {
    cx_word result, base;
    cx_uint16 fp = ast_Initializer(this)->fp;
    ast_InitializerFrame *frame = &ast_Initializer(this)->frames[fp?fp-1:0];
    ast_StaticInitializerFrame *baseFrame = &(this->frames[fp?fp-1:0]);
    ast_InitializerFrame *thisFrame = &ast_Initializer(this)->frames[fp];
    result = 0;

    base = baseFrame->ptr[variable];
    if (!base) {
        ast_Parser_error(yparser(), "parser error: base is zero in offset calculation");
        goto error;
    }

    /* Switch on current type */
    switch(frame->type->kind) {
    case CX_PRIMITIVE:
        result = base;
        break;
    case CX_COMPOSITE:
        if (fp) {
            result = base + thisFrame->member->offset;
        } else {
            result = base;
        }
        break;
    case CX_ITERATOR:
        result = base;
        break;
    case CX_COLLECTION: {
        if (fp) {
            cx_uint32 elementSize = cx_type_sizeof(cx_collection(frame->type)->elementType);
            switch(cx_collection(frame->type)->kind) {
            case CX_SEQUENCE:
                ((cx_objectseq*)base)->length++;
                ((cx_objectseq*)base)->buffer = cx_realloc(((cx_objectseq*)base)->buffer, ((cx_objectseq*)base)->length * elementSize);
                base = (cx_word)((cx_objectseq*)base)->buffer;
            case CX_ARRAY:
                result = base + thisFrame->location * cx_type_sizeof(cx_collection(frame->type)->elementType);
                memset((void*)result, 0, elementSize);
                break;
            case CX_LIST: {
                if (cx_collection_elementRequiresAlloc(cx_collection(frame->type))) {
                    result = (cx_word)cx_calloc(elementSize);
                }
                if (!*(cx_ll*)base) {
                    *(cx_ll*)base = cx_llNew();
                }
                cx_llAppend(*(cx_ll*)base, (void*)result);
                if (!result) {
                    result = (cx_word)cx_llGetPtr(*(cx_ll*)base, cx_llSize(*(cx_ll*)base)-1);
                }
                break;
            }
            case CX_MAP: {
                cx_type keyType = cx_map(frame->type)->keyType;
                if (!thisFrame->isKey) {
                    if (cx_collection_elementRequiresAlloc(cx_collection(frame->type))) {
                        result = (cx_word)cx_calloc(elementSize);
                    }
                    if (!*(cx_rbtree*)base) {
                        *(cx_rbtree*)base = cx_rbtreeNew(frame->type);
                    }
                    cx_rbtreeSet(*(cx_rbtree*)base, (void*)this->frames[fp].keyPtr[variable], (void*)result);
                    if (!result) {
                        if (this->frames[fp].keyPtr[variable]) {
                            result = (cx_word)cx_rbtreeGetPtr(*(cx_rbtree*)base, (void*)this->frames[fp].keyPtr[variable]);
                        } else {
                            ast_Parser_error(yparser(), "cannot set element without keyvalue");
                            goto error;
                        }
                    }
                } else {
                    result = (cx_word)cx_calloc(cx_type_sizeof(keyType));
                    this->frames[fp].keyPtr[variable] = result;
                    thisFrame->isKey = FALSE;
                }
                break;
            }
            }
        } else {
            result = base;
        }
        break;
    }
    default: {
        cx_id id;
        ast_Parser_error(yparser(), "invalid initializer type '%s'", ast_Parser_id(frame->type, id));
        break;
    }
    }

    return result;
error:
    return 0;
}
/* $end */

/* ::corto::ast::StaticInitializer::construct() */
cx_int16 _ast_StaticInitializer_construct(ast_StaticInitializer this) {
/* $begin(::corto::ast::StaticInitializer::construct) */
    cx_int8 variable;
    
    /* Copy offsets of variables into frames */
    for(variable=0; variable<ast_Initializer(this)->variableCount; variable++) {
         this->frames[0].ptr[variable] =
                (cx_word)ast_Object(ast_Initializer(this)->variables[variable].object)->value;
        if (!this->frames[0].ptr[variable]) {
            ast_Parser_error(yparser(), "non-static variable in static initializer");
            goto error;
        }
    }
    
    return ast_Initializer_construct(ast_Initializer(this));
error:
    return -1;
/* $end */
}

/* ::corto::ast::StaticInitializer::define() */
cx_int16 _ast_StaticInitializer_define(ast_StaticInitializer this) {
/* $begin(::corto::ast::StaticInitializer::define) */
    cx_uint32 variable;
    cx_object o;

    /* Types are defined during 1st pass. All other objects are defined when a program starts. Defining types
     * at compile-time is required to do validity checking while parsing. On the other hand, objects cannot be
     * defined at compile-time because class constructors\destructors are not yet defined at that point. This
     * would cause object creation\destruction without calling the appropriate constructors\destructors.
     */
    for(variable=0; variable<ast_Initializer(this)->variableCount; variable++) {
        o = (cx_object)ast_Object(ast_Initializer(this)->variables[variable].object)->value;
        if (cx_instanceof(cx_type(cx_type_o), o)
                || (cx_checkAttr(o, CX_ATTR_SCOPED) && cx_instanceof(cx_type(cx_type_o), cx_parentof(o)))) {
            if (cx_define(o)) {
                cx_id id1, id2;
                ast_Parser_error(yparser(), "define of variable '%s' of type '%s' failed",
                        ast_Parser_id(o, id1),
                        ast_Parser_id(cx_typeof(o), id2));
                goto error;
            }
        } else {
            ast_Expression refVar = ast_Expression(ast_ObjectCreate(o));
            refVar->isReference = TRUE; /* Always treat object as reference */
            ast_Define defineStmt = ast_DefineCreate(refVar);
            ast_Parser_addStatement(yparser(), ast_Node(defineStmt));
            ast_Parser_collect(yparser(), defineStmt);
            ast_Parser_collect(yparser(), refVar);
        }
    }
    
    ast_Initializer_define_v(ast_Initializer(this));

    return 0;
error:
    return -1;
/* $end */
}

/* ::corto::ast::StaticInitializer::push() */
cx_int16 _ast_StaticInitializer_push(ast_StaticInitializer this) {
/* $begin(::corto::ast::StaticInitializer::push) */
    cx_uint8 variable;
    
    /* Obtain offset for all that variables being initialized */
    for(variable=0; variable<ast_Initializer(this)->variableCount; variable++) {
        /* Calculate the offset for the current value */
        this->frames[ast_Initializer(this)->fp].ptr[variable] = ast_Initializer_offset(this, variable);
    }

    return ast_Initializer_push_v(ast_Initializer(this));
/* $end */
}

/* ::corto::ast::StaticInitializer::value(Expression v) */
cx_int16 _ast_StaticInitializer_value(ast_StaticInitializer this, ast_Expression v) {
/* $begin(::corto::ast::StaticInitializer::value) */
    cx_word offset;
    cx_uint32 variable;
    cx_uint32 fp = ast_Initializer(this)->fp;
    cx_type type = ast_Initializer_currentType(ast_Initializer(this));
    cx_type vType = ast_Expression_getType_type(v, type);
    
    if (!type) {
        cx_id id;
        ast_Parser_error(yparser(), "excess elements in initializer of type '%s'", 
            ast_Parser_id(ast_Object(ast_Expression(this)->type)->value, id));
        goto error;
    }

    if (!vType) {
        goto error;
    }

    /* Validate whether expression type matches current type of initializer */
    if (vType && !cx_type_castable(type, vType)) {
        cx_id id, id2;
        ast_Parser_error(yparser(), "type '%s' invalid here (expected '%s')", 
            ast_Parser_id(vType, id), ast_Parser_id(type, id2));
        goto error;
    }

    /* Validate whether value has no side-effects - which are not allowed in a static initializer */
    if (ast_Expression_hasSideEffects(v)) {
        ast_Parser_error(yparser(), "expressions with side-effects are not allowed in static initializers.");
        goto error;
    }

    /* Serialize value to all variables being initialized */
    for(variable=0; variable<ast_Initializer(this)->variableCount; variable++) {
        /* Calculate the offset for the current value */
        this->frames[fp].ptr[variable] = ast_Initializer_offset(this, variable);
        offset = this->frames[fp].ptr[variable];

        if (!offset) {
            goto error;
        }
        
        if (ast_Expression_serialize(v, type, offset)) {
            goto error;
        }
    }

    return ast_Initializer_next(ast_Initializer(this));
error:
    return -1;
/* $end */
}