/*
 * cx_def.h
 *
 *  Created on: Aug 2, 2012
 *      Author: sander
 */

#ifndef CX_DEF_H_
#define CX_DEF_H_

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NULL
#define NULL (0x0)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef TRUE
#define TRUE (!FALSE)
#endif

#define CX_NULL_STRING ("null")

typedef int (*cx_compareAction)(void* o1, void* o2);
typedef int (*cx_walkAction)(void* o, void* userData);

/* Builtin collection-implementation definitions */
typedef struct cx_rbtree_s* cx_rbtree;
typedef struct cx_ll_s* cx_ll;

/* Configuration parameters */
#define CX_MAX_SCOPE_DEPTH (256)
#define CX_MAX_TYPE_DEPTH (32)
#define CX_MAX_INHERITANCE_DEPTH (32)
#define CX_MAX_BINDINGS (256)
#define CX_MAX_THREADS (512)
#define CX_MAX_NOTIFY_DEPTH (32)
#define CX_MAX_STRING_LITERAL_LENGTH (512)
#define CX_MAX_WAIT_FOR_OBJECTS (256)

/* Debugging flags */
/* #define CX_TRACE_NOTIFICATIONS */
/* #define CX_LEAKCHECK */
/* #define CX_SERIALIZER_TRACING */
#define CX_IC_TRACING
#define CX_VM_DEBUG
#define CX_VM_BOUNDSCHECK

#define CX_PROCEDURE_STUB (0)
#define CX_PROCEDURE_CDECL (1)
#define CX_PROCEDURE_VM    (2)
#define _(txt)

#define CX_STRUCT(type) typedef struct type type
#define CX_INTERFACE(type) typedef void* type
#define CX_CLASS(type) typedef struct type##_s* type
#define CX_PROCEDURE(type) typedef struct type##_s* type
#define CX_BITMASK(type) typedef uint32_t type

/* The _() prevents expansion by class casting-macro's */
#define CX_SEQUENCE(type, subtype, postexpr) typedef struct type {uint32_t length; subtype _()(*buffer) postexpr;} type
#define CX_SEQUENCE_ANONYMOUS(subtype, postexpr) struct {uint32_t length; subtype _()(*buffer) postexpr;}

#define CX_LIST(type) typedef cx_ll type

#define CX_STRUCT_DEF(type) struct type
#define CX_CLASS_DEF(type) struct type##_s
#define CX_PROCEDURE_DEF(type) struct type##_s

#define CX_EXTEND(type) struct type##_s _parent

#define CX_ANY(__type) typedef struct __type {cx_type type; void* value; cx_bool owner;} __type

#ifdef __cplusplus
}
#endif

#endif /* CX_DEF_H_ */

