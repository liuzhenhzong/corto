 /*
 * cx_object.c
 *
 *  Created on: Aug 3, 2012
 *      Author: sander
 */

#define corto_lang_LIB
#include "cx__meta.h"
#include "cx__object.h"
#include "cx_memory_ser.h"
#include "cx_mm.h"
#include "cx_err.h"
#include "cx_util.h"
#include "cx_mem.h"
#include "cx_string.h"
#include "cx_serializer.h"
#include "cx_type.h"
#include "cx__class.h"
#include "cx_string_deser.h"
#include "cx_init_ser.h"
#include "cx_copy_ser.h"
#include "cx_procedure.h"
#include "cx_call.h"
#include "cx_string_ser.h"
#include "cx_convert.h"
#include "cx_interface.h"
#include "cx_dispatcher.h"
#include "cx_time.h"
#include "cx_loader.h"
#include "cx_overload.h"

#include <limits.h>

extern cx_mutex_s cx_adminLock;

static cx_object cx_adopt(cx_object parent, cx_object child);
static cx_int32 cx_notify(cx__observable *_o, cx_object observable, cx_uint32 mask);

static void cx_notifyObserverDefault(cx__observer* data, cx_object this, cx_object observable, cx_uint32 mask);
static void cx_notifyObserverThis(cx__observer* data, cx_object this, cx_object observable, cx_uint32 mask);

extern cx_threadKey CX_KEY_ATTR;

/* Thread local storage key that keeps track of the objects that are prepared to wait for. */
extern cx_threadKey CX_KEY_WAIT_ADMIN;

typedef struct cx_waitForObject {
    cx_object objects[CX_MAX_WAIT_FOR_OBJECTS];
    cx_uint32 count;
    cx_object triggered;
    cx_sem semaphore;
    cx_observer observer;
    int triggerCount; /* Value is atomically incremented\decremented to make sure wait is only triggered once */
}cx_waitForObject;

/* Thread local storage key for administration that keeps track for which observables notifications take place.
 * This key points to an element in a list keyed by threadId's for which notifications have taken place. Value
 * of this element is the observable being notified at the moment. When a listen\silence call needs to clean
 * memory it can look at this administration to see if it is in use. To prevent deadlocks, listen\silence calls
 * will not look at their own threadId, since this would indicate listen\silence is called from an observer being
 * notified. This key is created in cx_start. */
/* TODO: when a thread exits, the corresponding element must be free'd again - use tls destructor function */
extern cx_threadKey CX_KEY_OBSERVER_ADMIN;

/* Lists all the anonymous objects in use. Used by the garbage collector. */
cx_ll cx_anonymousObjects = NULL;

typedef struct cx_observerElement {
    cx__observer ** observers;
    cx_bool free;
}cx_observerElement;

typedef struct cx_observerAdmin {
    cx_thread id;
    cx_observerElement stack[CX_MAX_NOTIFY_DEPTH];
    cx_uint32 sp;
    cx_object from;
}cx_observerAdmin;
static cx_observerAdmin observerAdmin[CX_MAX_THREADS];

static struct cx_observerAdmin* cx_observerAdminGet(void) {
    cx_observerAdmin *admin = cx_threadTlsGet(CX_KEY_OBSERVER_ADMIN);
    if (!admin) {
        cx_thread thr = cx_threadSelf();
        cx_uint32 i;
        do {
            i = 0;
            while(observerAdmin[i].id) { /* Find a free slot for thread in administration */
                i++;
                if (i >= CX_MAX_THREADS) {
                    cx_critical("maximum number of supported threads reached! (%d)", CX_MAX_THREADS);
                }
            }
        }while(!cx_cas(&observerAdmin[i].id,0,thr));
        admin = &observerAdmin[i];
        cx_threadTlsSet(CX_KEY_OBSERVER_ADMIN, admin);
    }
    return admin;
}

/* Push observer-array to thread administration. Rather than passing the array pass the
 * address of the array so that setting the administration can be done atomic. */
cx__observer** cx_observersPush(cx__observer**  *observers, cx_object *this) {
    cx__observer** result;
    cx_observerAdmin *admin = cx_observerAdminGet();
    result = admin->stack[admin->sp].observers = *observers;
    admin->stack[admin->sp++].free = FALSE;

    if (this) {
        *this = admin->from;
    }

    return result;
}

cx_bool cx_observersPop(void) {
    cx_observerAdmin *admin = cx_threadTlsGet(CX_KEY_OBSERVER_ADMIN); /* Admin must always exist when popping */
    return admin->stack[--admin->sp].free;
}

cx_bool cx_observersWaitForUnused(cx__observer** observers) {
    cx_thread self = cx_threadSelf();
    cx_uint32 i, j;
    cx_bool inUse, freeArray = TRUE; /* Initialization merely to satisfy the compiler */

    if (!observers) {
        return FALSE;
    }

    /* Spinning loop which waits as long as an observer array is being used
     * in a notification by any of the running threads. */
    do {
        inUse = FALSE;
        for(i=0; i<CX_MAX_THREADS; i++) {
            if (observerAdmin[i].id) {
                /* Check whether the observer array is in use by threads other than myself */
                if ((observerAdmin[i].id != self)) {
                    for(j=0; j<observerAdmin[i].sp; j++) {
                        if (observerAdmin[i].stack[j].observers == observers) {
                            inUse = TRUE; /* Array is found, so keep waiting */
                        }
                    }

                /* Check whether the observer array is in use by my own thread */
                } else {
                    freeArray = TRUE;
                    for(j=0; j<observerAdmin[i].sp; j++) {
                        /* The array is in use by my own thread so I can't keep spinning. Notify the observing function to
                         * free the array */
                        if (observerAdmin[i].stack[j].observers == observers) {
                            observerAdmin[i].stack[j].free = TRUE; /* Signal the notification routine to free the array */
                            freeArray = FALSE; /* Since the array is still in use by myself don't free array yet */
                            break;
                        }
                    }
                }
            }
        }
    }while(inUse);

    return freeArray;
}

static cx__scope* cx__objectScope(cx__object* o) {
    cx__scope* result = NULL;

    if (o->attrs.scope) {
        result = CX_OFFSET(o, -sizeof(cx__scope));
    }
    return result;
}

static cx__writable* cx__objectWritable(cx__object* o) {
    cx__writable* result = (void*)o;

    if (o->attrs.scope) {
        result = CX_OFFSET(result, -sizeof(cx__scope));
    }
    if (o->attrs.write) {
        result = CX_OFFSET(result, -sizeof(cx__writable));
    } else {
        result = NULL;
    }

    return result;
}

static cx__observable* cx__objectObservable(cx__object* o) {
    cx__observable* result = (void*)o;

    if (o->attrs.scope) {
        result = CX_OFFSET(result, -sizeof(cx__scope));
    }
    if (o->attrs.write) {
        result = CX_OFFSET(result, -sizeof(cx__writable));
    }
    if (result && o->attrs.observable) {
        result = CX_OFFSET(result, -sizeof(cx__observable));
    } else {
        result = NULL;
    }

    return result;
}

static cx__persistent* cx__objectPersistent(cx__object* o) {
    cx__persistent* result = (void*)o;

    if (o->attrs.scope) {
        result = CX_OFFSET(result, -sizeof(cx__scope));
    }
    if (o->attrs.write) {
        result = CX_OFFSET(result, -sizeof(cx__writable));
    }
    if (result && o->attrs.observable) {
        result = CX_OFFSET(result, -sizeof(cx__observable));

        if (o->attrs.persistent) {
            result = CX_OFFSET(result, -sizeof(cx__persistent));
        } else {
            result = NULL;
        }
    } else {
        result = NULL;
    }

    return result;
}

static void* cx__objectStartAddr(cx__object* o) {
    void* result;
    result = o;
    if (o->attrs.scope) {
        result = CX_OFFSET(result, -sizeof(cx__scope));
    }
    if (o->attrs.write) {
        result = CX_OFFSET(result, -sizeof(cx__writable));
    }
    if (o->attrs.observable) {
        result = CX_OFFSET(result, -sizeof(cx__observable));
    }
    if (o->attrs.persistent) {
        result  = CX_OFFSET(result, -sizeof(cx__persistent));
    }
    return result;
}

/* Initialze scope-part of object */
static cx_object cx__initScope(cx_object o, cx_string name, cx_object parent) {
    cx__object* _o;
    cx__scope* scope;
    cx_object result = NULL;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    cx_assert(scope != NULL, "cx__initScope: created scoped object, but cx__objectScope returned NULL.");

    scope->name = cx_strdup(name);
    scope->declared = 1;
    scope->parent = parent;
    cx_rwmutexNew(&scope->scopeLock);

    /* Call framework initializer. Do this before adopting the
     * object, so that when the initializer failed, it hasn't
     * yet been added to the scope. */
    if (cx_init(o)) {
        cx_id id;
        cx_string err = cx_lasterr();
        if (err) {
            cx_seterr("%s::init failed: %s", cx_fullname(cx_typeof(o), id), err);
        } else {
            cx_seterr("%s::init failed", cx_fullname(cx_typeof(o), id));
        }
        goto error;
    }

    /* Add object to the scope of the parent-object */
    if (!(result = cx_adopt(parent, o))) {
        goto error;
    }

    if (result != o) {
        cx_dealloc(scope->name);
        cx_rwmutexFree(&scope->scopeLock);
    }

    return result;
error:
    return NULL;
}

static void cx__deinitScope(cx_object o) {
    cx__object *_o;
    cx__scope* scope;

    /* Obtain own scope */
    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    cx_assert(scope != NULL, "cx__deinitScope: called on non-scoped object <%p>.", o);
    cx_assert(scope->attached == NULL, "cx__deinitScope: object has still objects attached");

    /* Free parent */
    if (scope->parent) {
        cx_release(scope->parent);
        scope->parent = NULL;
    }

    /* We cannot actually remove the scope itself, since there might be childs which
     * have multiple cycles, which must be resolved first. The childs will take care
     * of removing the parent's scope. */

    if (scope->name) {
        cx_dealloc(scope->name);
        scope->name = NULL;
    }

    /* Finally, free own scopeLock. */
    cx_rwmutexFree(&scope->scopeLock);
}

/* Initialize writable-part of object */
static void cx__initWritable(cx_object o) {
    cx__object* _o;
    cx__writable* writable;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    writable = cx__objectWritable(_o);
    cx_assert(writable != NULL, "cx__initWritable: created writable object, but cx__objectWritable returned NULL.");

    cx_rwmutexNew(&writable->lock);
}

static void cx__deinitWritable(cx_object o) {
    cx__object* _o;
    cx__writable* writable;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    writable = cx__objectWritable(_o);
    cx_assert(writable != NULL, "cx__deinitWritable: called on non-writable object <%p>.", o);

    cx_rwmutexFree(&writable->lock);
}

/* Initialize observable-part of object */
static void cx__initObservable(cx_object o) {
    cx__object* _o;
    cx__observable *observable, *parentObservable;
    cx_object parent;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    observable = cx__objectObservable(_o);
    cx_assert(observable != NULL, "cx__initObservable: created observable object, but cx__objectObservable returned NULL.");

    cx_rwmutexNew(&observable->selfLock);

    if (cx_checkAttr(o, CX_ATTR_SCOPED)) {
        cx_rwmutexNew(&observable->childLock);
    }

    observable->onSelf = NULL;
    observable->onChild = NULL;
    observable->onSelfArray = NULL;
    observable->onChildArray = NULL;
    observable->used = FALSE;
    observable->lockRequired = FALSE;
    observable->childLockRequired = FALSE;

    if (cx_checkAttr(o, CX_ATTR_SCOPED)) {
        parent = o;
        while((parent = cx_parentof(parent))) {
            if ((parentObservable = cx__objectObservable(CX_OFFSET(parent, -sizeof(cx__object))))) {
                cx_rwmutexRead(&parentObservable->childLock);

                /* Inherit childLockRequired from first observable parent */
                if (parentObservable->childLockRequired) {
                    observable->lockRequired = TRUE;
                }

                cx_rwmutexUnlock(&parentObservable->childLock);

                if (observable->childLockRequired) {
                    break;
                }
            }
        }
    }

    /* Override lockRequired if object is not writable */
    if (!cx_checkAttr(o, CX_ATTR_WRITABLE)) {
        observable->lockRequired = FALSE;
    }
}

static void cx__deinitObservable(cx_object o) {
    cx__object* _o;
    cx__observable* observable;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    observable = cx__objectObservable(_o);
    cx_assert(observable != NULL, "cx__deinitObservable: called on non-observable object <%p>.", o);

    /* Delete observer objects in onSelf and onChild */
    if (observable->onSelf) {
        cx__observer* observer;
        while((observer = cx_llTakeFirst(observable->onSelf))) {
            /* Clear template observer data */
            if (observer->observer->_template) {
                cx_object observerObj = observer->_this;
                cx_any thisAny = {cx_typeof(observerObj), observerObj, FALSE};
                cx_class_listen(thisAny, observer->observer, 0, NULL, NULL);
            }
            if (!--observer->count) {
                cx_dealloc(observer);
            }
        }
        cx_llFree(observable->onSelf);
        observable->onSelf = NULL;
    }

    if (observable->onChild) {
        cx__observer* observer;
        while((observer = cx_llTakeFirst(observable->onChild))) {
            /* Clear template observer data */
            if (observer->observer->_template) {
                cx_object observerObj = observer->_this;
                cx_any thisAny = {cx_typeof(observerObj), observerObj, FALSE};
                cx_class_listen(thisAny, observer->observer, 0, NULL, NULL);
            }
            observer->count--;
             if (!observer->count) {
                cx_dealloc(observer);
            }
        }
        cx_llFree(observable->onChild);
        observable->onChild = NULL;
    }

    cx_rwmutexFree(&observable->selfLock);
    if (cx_checkAttr(o, CX_ATTR_SCOPED)) {
        cx_rwmutexFree(&observable->childLock);
    }
}

/* Initialize static scoped object */
void cx__newSSO(cx_object sso) {
    cx__object* o;
    cx__scope* scope;

    o = CX_OFFSET(sso, -sizeof(cx__object));
    scope = cx__objectScope(o);

    /* Don't call initScope because name is already set. */
    cx_rwmutexNew(&scope->scopeLock);
    if (scope->parent) {
        cx__adoptSSO(sso);
    }

    scope->declared = 1;

    /* Init observable */
    if (cx_checkAttr(sso, CX_ATTR_OBSERVABLE)) {
        cx__initObservable(sso);
    }

    /* Keep type */
    cx_claim(cx_typeof(sso));
}

/* Deinitialize static scoped object */
void cx__freeSSO(cx_object sso) {
    cx__object* o;
    cx__scope* scope;

    o = CX_OFFSET(sso, -sizeof(cx__object));
    scope = cx__objectScope(o);

    cx_assert(scope != NULL, "cx__freeSSO: static scoped object has no scope (very unlikely, cx__freeSSO called on non-static object?)")

    if (scope->parent) {
        cx_release(scope->parent);
    }

    if (scope->scope) {
        if (cx_rbtreeSize(scope->scope)) {
            cx_error("cx__freeSSO: scope of object '%s' is not empty (%d left)",
                cx_nameof(sso),
                cx_rbtreeSize(scope->scope));
        }
        cx_rbtreeFree(scope->scope);
        scope->scope = NULL;
    }

    cx_rwmutexFree(&scope->scopeLock);

    /* Deinitialize observable */
    if (cx_checkAttr(sso, CX_ATTR_OBSERVABLE)) {
        cx__deinitObservable(sso);
    }

    /* Free type */
    cx_release(cx_typeof(sso));
}

/* Adopt static scoped object */
int cx__adoptSSO(cx_object sso) {
    cx__object* o;
    cx__scope* scope;
    cx_object parent;

    o = CX_OFFSET(sso, -sizeof(cx__object));
    scope = cx__objectScope(o);

    cx_assert(scope != NULL, "cx__adoptSSO: static scoped object has no scope (very unlikely, cx__adoptSSO called on non-static object?)");

    parent = scope->parent;

    cx_assert(parent != NULL, "cx__adoptSSO: static scoped object has no parent");

    return !cx_adopt(parent, sso);
}

/* Find the right constructor to call */
void cx_delegateDestruct(cx_type t, cx_object o) {
    cx_function delegate = NULL;

    if (t->kind == CX_COMPOSITE) {
        if (cx_interface(t)->kind == CX_CLASS) {
            cx_interface i = cx_interface(t);
            do {
                delegate = cx_class(i)->destruct._parent.procedure;
                i = i->base;
            } while(i && !delegate);
        }
    }

    if (delegate) {
        if(delegate->kind == CX_PROCEDURE_CDECL) {
            ((void(*)(cx_function f, void *result, void *args))delegate->impl)(delegate, NULL, &o);
        } else {
            cx_call(delegate, NULL, o);
        }
    }
}

/* Destruct object */
int cx__destructor(cx_object o) {
    cx_type t;
    cx__object* _o;

    t = cx_typeof(o);
    if (cx_checkState(o, CX_DEFINED)) {
        _o = CX_OFFSET(o, -sizeof(cx__object));
        if (cx_class_instanceof(cx_class_o, t)) {
            /* Detach observers from object */
            cx_class_detachObservers(cx_class(t), o);

            /* Call destructor */
            cx_delegateDestruct(cx_typeof(o), o);
        } else if (cx_class_instanceof(cx_procedure_o, t)) {
            /* Call unbind */
            cx_procedure_unbind(cx_procedure(cx_typeof(o)), o);
        }

        _o->attrs.state &= ~CX_DEFINED;
    } else {
        cx_id id, id2;
        cx_error("%s::destruct: object '%s' is not defined", cx_fullname(t, id2), cx_fullname(o, id));
        goto error;
    }

    return 0;
error:
    return -1;
}

void cx__setState(cx_object o, cx_uint8 state) {
    cx__object* _o;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    _o->attrs.state |= state;
}

static cx_equalityKind cx_objectCompare(cx_type this, const void* o1, const void* o2) {
    int r;
    CX_UNUSED(this);
    return ((r = stricmp(o1, o2)) < 0) ? CX_LT : (r > 0) ? CX_GT : CX_EQ;
}

#include "ctype.h"
static cx_equalityKind cx_objectCompareLookup(cx_type this, const void* o1, const void* o2) {
    CX_UNUSED(this);
    int r;
    const char *ptr1, *ptr2;
    ptr1 = o1;
    ptr2 = o2;
    char ch1, ch2;

    ch2 = *ptr2;
    while((ch1 = *ptr1) && ch2) {
        if (ch1 == ch2) {
            ptr1++; ptr2++;
            ch2 = *ptr2;
            continue;
        }
        if (ch1 < 97) ch1 = tolower(ch1);
       
        /* Query is always made lower case, for efficiency reasons */
        /* if (ch2 < 97) ch2 = tolower(ch2); */
        if (ch1 != ch2) {
            r = ch1 - ch2;
            goto compare;
        }
        ptr1++;
        ptr2++;
        ch2 = *ptr2;
    }

    if ((ch1 == '(') && !ch2) {
        r = 0;
        goto compare;
    }

    r = ch1 - ch2;
compare:
    return (r < 0) ? CX_LT : (r > 0) ? CX_GT : CX_EQ;
}

/* Match a state exclusively:
 *                CX_DECLARED - CX_DECLARED | CX_DEFINED
 *  CX_DECLARED        X
 *  CX_DEFINED                       X
 *  CX_DECLARED |      X             X
 *     CX_DEFINED
 */
static cx_bool cx__checkStateXOR(cx_object o, cx_uint8 state) {
    cx_uint8 ostate;
    cx__object* _o;

    _o = CX_OFFSET(o, -sizeof(cx__object));

    ostate = _o->attrs.state;
    if (ostate & CX_DEFINED) {
        ostate = CX_DEFINED;
    }

    return ostate & state;
}

/* Adopt an object */
static cx_object cx_adopt(cx_object parent, cx_object child) {
    cx__object *_parent, *_child;
    cx__scope *p_scope, *c_scope;
    cx_type parentType;
    cx_type childType;

    _parent = CX_OFFSET(parent, -sizeof(cx__object));
    _child = CX_OFFSET(child, -sizeof(cx__object));
    childType = cx_typeof(child);

    /* Parent must be a valid object */
    if (!cx_checkState(parent, CX_VALID)) {
        cx_seterr("parent is invalid");
        goto error;
    }

    /* Obtain pointers to scope of parent and child */
    p_scope = cx__objectScope(_parent);
    if (p_scope) {
        c_scope = cx__objectScope(_child);
        if (c_scope) {

            /* Insert child in parent-scope */
            if (cx_rwmutexWrite(&p_scope->scopeLock))     
                cx_critical("cx_adopt: lock operation on scopeLock of parent failed");

            if (!p_scope->scope) {
                p_scope->scope = cx_rbtreeNew_w_func(cx_objectCompare);
            }
            
            cx_object existing = cx_rbtreeFindOrSet(p_scope->scope, c_scope->name, child);
            if (existing) {
                if (cx_typeof(existing) != cx_typeof(child)) {
                    cx_seterr("'%s' is already declared with a different type", c_scope->name);
                    goto err_existing;
                } else {
                    child = existing;
                }
            } else {
                /* Check if parentType matches scopeType of child type */
                if (childType->parentType) {
                    parentType = cx_typeof(parent);
                    if ((childType->parentType != parentType) && !cx_instanceof(childType->parentType, parent)) {
                        cx_id parentId, childParentTypeId;
                        cx_seterr("type of '%s' is not '%s'",
                                cx_fullname(parent, parentId), cx_fullname(childType->parentType, childParentTypeId));
                        goto err_invalid_parent;
                    }
                }

                /* Check if parentState matches scopeState of child type */
                if (childType->parentState && !cx__checkStateXOR(parent, childType->parentState)) {
                    cx_id parentId;
                    cx_uint32 childState = childType->parentState;
                    cx_seterr("'%s' is %s, must be %s",
                        cx_fullname(parent, parentId),
                        cx_stateStr(_parent->attrs.state),
                        cx_stateStr(childState));
                    goto err_invalid_parent;
                }
            }

            /* Parent must not be deleted before all childs are gone. */
            cx_claim(parent);
            if (cx_rwmutexUnlock(&p_scope->scopeLock)) 
                cx_critical("cx_adopt: unlock operation on scopeLock of parent failed");;
        } else {
            cx_critical("cx_adopt: child-object is not scoped");
        }
    } else {
        cx_critical("cx_adopt: parent-object is not scoped");
    }

    return child;

err_invalid_parent:
    c_scope->parent = NULL;
    cx_rbtreeRemove(p_scope->scope, c_scope->name);
err_existing:
    cx_rwmutexUnlock(&p_scope->scopeLock);
error:
    return NULL;
}

void cx_attach(cx_object parent, cx_object child) {
    cx__object *_parent;
    cx__scope *_scope;

    if (!cx_checkAttr(parent, CX_ATTR_SCOPED)) {
        cx_critical("attach: cannot attach to non-scoped object");
    }
    if (cx_checkAttr(child, CX_ATTR_SCOPED)) {
        cx_critical("attach: cannot attach scoped object");
    }

    _parent = CX_OFFSET(parent, -sizeof(cx__object));
    _scope = cx__objectScope(_parent);

    cx_rwmutexWrite(&_scope->scopeLock);
    if (!_scope->attached) {
        _scope->attached = cx_llNew();
    }
    cx_llAppend(_scope->attached, child);
    cx_rwmutexUnlock(&_scope->scopeLock);
}

void cx_detach(cx_object parent, cx_object child) {
    cx__object *_parent;
    cx__scope *_scope;

    if (!cx_checkAttr(parent, CX_ATTR_SCOPED)) {
        cx_critical("attach: cannot attach to non-scoped object");
    }
    if (cx_checkAttr(child, CX_ATTR_SCOPED)) {
        cx_critical("attach: cannot attach scoped object");
    }

    _parent = CX_OFFSET(parent, -sizeof(cx__object));
    _scope = cx__objectScope(_parent);

    cx_rwmutexWrite(&_scope->scopeLock);
    cx_llRemove(_scope->attached, child);
    cx_rwmutexUnlock(&_scope->scopeLock);
}

/* Orphan object - not a public function as this will only happen during destruction of an object. */
void cx__orphan(cx_object o) {
    cx__object *_parent, *_child;
    cx__scope *p_scope, *c_scope;

    _child = CX_OFFSET(o, -sizeof(cx__object));
    c_scope = cx__objectScope(_child);

    if (c_scope->parent) {
        _parent = CX_OFFSET(c_scope->parent, -sizeof(cx__object));
        p_scope = cx__objectScope(_parent);

        /* Remove object from parent scope */
        if (cx_rwmutexWrite(&p_scope->scopeLock)) goto err_parent_mutex;
        cx_rbtreeRemove(p_scope->scope, (void*)cx_nameof(o));

        /* If scope is empty delete it. */
        if (!cx_rbtreeSize(p_scope->scope)) {
            cx_rbtreeFree(p_scope->scope);
            p_scope->scope = NULL;
        }

        if (cx_rwmutexUnlock(&p_scope->scopeLock)) goto err_parent_mutex;
    }

    return;
err_parent_mutex:
    cx_error("cx__orphan: lock operation of scopeLock of parent failed");
}


/* Find the right initializer to call */
cx_int16 cx_delegateInit(cx_type t, void *o) {
    cx_function delegate = NULL;
    cx_int16 result = 0;

    delegate = t->init._parent.procedure;

    if (t->kind == CX_COMPOSITE) {
        if ((cx_interface(t)->kind == CX_CLASS) || ((cx_interface(t)->kind == CX_PROCEDURE))) {
            cx_interface i = cx_interface(t)->base;
            while(i && !delegate) {
                delegate = cx_type(i)->init._parent.procedure;
                i = i->base;
            }
        }
    }

    if (delegate) {
        if(delegate->kind == CX_PROCEDURE_CDECL) {
            ((void(*)(cx_function f, void *result, void *args))delegate->impl)(delegate, &result, &o);
        } else {
            cx_call(delegate, &result, o);
        }
    }

    return result;
}

cx_attr cx_setAttr(cx_attr attrs) {
    cx_attr* attr = cx_threadTlsGet(CX_KEY_ATTR);
    cx_attr oldAttr = CX_ATTR_DEFAULT;
    if (!attr) {
        attr = cx_alloc(sizeof(cx_attr));
        cx_threadTlsSet(CX_KEY_ATTR, attr);
    } else {
        oldAttr = *attr;
    }
    *attr = attrs;
    return oldAttr;
}

cx_attr cx_getAttr(void) {
    cx_attr* attr = cx_threadTlsGet(CX_KEY_ATTR);
    if (attr) {
        return *attr;
    } else {
        return CX_ATTR_DEFAULT;
    }
}

/* Create new object with attributes */
cx_object _cx_declare(cx_type type) {
    cx_uint32 size, headerSize;
    cx__object* o = NULL;
    cx_attr attrs = cx_getAttr();

    if (!type) {
        cx_seterr("parameter 'type' is null");
        goto error;
    }

    /* Type must be valid and defined */
    if (!cx_checkState(type, CX_VALID | CX_DEFINED)) {
        cx_seterr("type is not valid/defined");
        goto error;
    }

    if (attrs & CX_ATTR_DEFAULT) {
        attrs |= CX_ATTR_OBSERVABLE;

        if (type->kind != CX_VOID) {
            attrs |= CX_ATTR_WRITABLE;
        }
        if (attrs & CX_ATTR_SCOPED) {
            attrs |= CX_ATTR_PERSISTENT;
        }
    }

    headerSize = sizeof(cx__object);

    /* Get size of type */
    size = cx_type_allocSize(type);

    /* Calculate size of attributes */
    if (attrs & CX_ATTR_SCOPED) {
        headerSize += sizeof(cx__scope);
    }
    if (attrs & CX_ATTR_WRITABLE) {
        headerSize += sizeof(cx__writable);
    }
    if (attrs & CX_ATTR_OBSERVABLE) {
        headerSize += sizeof(cx__observable);
    }
    if (attrs & CX_ATTR_PERSISTENT) {
        headerSize += sizeof(cx__persistent);
    }

    size += headerSize;

    /* Allocate object */
    o = cx_calloc(size);
    if (o) {

        /* Offset o so it points to object */
        o = CX_OFFSET(o, headerSize - sizeof(cx__object));

        /* Give object initial refcount */
        o->refcount = 1;

        /* Set type */
        o->type = type;

        /* Set attributes */
        if (attrs & CX_ATTR_SCOPED) {
            o->attrs.scope = TRUE;
        }
        if (attrs & CX_ATTR_WRITABLE) {
            if (type->kind == CX_VOID) {
                cx_warning("writable void object created");
            }
            o->attrs.write = TRUE;
            cx__initWritable(CX_OFFSET(o, sizeof(cx__object)));
        }
        if (attrs & CX_ATTR_OBSERVABLE) {
            o->attrs.observable = TRUE;
        }
        if (attrs & CX_ATTR_PERSISTENT) {
            o->attrs.persistent = TRUE;
        }

        /* Initially, an object is valid and declared */
        o->attrs.state = CX_DECLARED;

        /* void objects are instantly defined because they have no value. */
        if (type->kind == CX_VOID) {
            o->attrs.state |= CX_DEFINED;
        }

        cx_claim(type);

        if (!(attrs & CX_ATTR_SCOPED)) {
            if (attrs & CX_ATTR_OBSERVABLE) {
                cx__initObservable(CX_OFFSET(o, sizeof(cx__object)));
            }

            /* Call framework initializer */
            if (!cx_init(CX_OFFSET(o, sizeof(cx__object)))) {
                o->attrs.state |= CX_VALID;
            } else {
                cx_id id;
                cx_string err = cx_lasterr();
                if (err) {
                    cx_seterr("%s::init failed: %s", cx_fullname(type, id), err);
                } else {
                    cx_seterr("%s::init failed", cx_fullname(type, id));
                }
                goto error_init;
            }

            /* Add object to anonymous cache */
            cx_mutexLock(&cx_adminLock);
            if (!cx_anonymousObjects) {
                cx_anonymousObjects = cx_llNew();
            }
            cx_llInsert(cx_anonymousObjects, CX_OFFSET(o, sizeof(cx__object)));
            cx_mutexUnlock(&cx_adminLock);
        }
    }

    return CX_OFFSET(o, sizeof(cx__object));
error_init:
    cx_release(type);
error:
    if (o) cx_dealloc(cx__objectObservable(o));
    return NULL;
}

/* Declare object */
cx_object _cx_declareChild(cx_object parent, cx_string name, cx_type type) {
    cx_object o = NULL;

    if (!parent) {
        parent = root_o;
    }

    /* Create new object */
    cx_attr oldAttr = cx_setAttr(cx_getAttr()|CX_ATTR_SCOPED);
    o = cx_declare(type);
    cx_setAttr(oldAttr);

    if (o) {
        cx_object o_ret = NULL;
        cx__object *_o = CX_OFFSET(o, -sizeof(cx__object));
        
        /* Initialize object parameters. */
        if ((o_ret = cx__initScope(o, name, parent))) {

            if (o_ret == o) {
                /* Observable administration needs to be initialized after the
                 * scope administration because it needs to setup the correct
                 * chain of parents to notify on an event. */
                if (cx_checkAttr(o, CX_ATTR_OBSERVABLE)) {
                    cx__initObservable(o);
                }

                /* Initially, an object is valid and declared */
                _o->attrs.state |= CX_VALID;
            } else {
                cx_release(type);
                cx_dealloc(cx__objectStartAddr(CX_OFFSET(o,-sizeof(cx__object))));
                o = o_ret;
                goto ok;
            }
        } else {
            cx__deinitScope(o);
            cx_release(type);
            cx_dealloc(cx__objectStartAddr(CX_OFFSET(o,-sizeof(cx__object))));
            o = NULL;
            goto error;
        }

        /* Notify parent of new object */
        if (cx__objectObservable(_o)) {
            cx_notify(cx__objectObservable(_o), o, CX_ON_DECLARE);
        }
    }

ok:
    return o;
error:
    if (o) {
        cx_delete(o);
    }
    return NULL;
}

cx_object _cx_create(cx_type type) {
    cx_object result = cx_declare(type);
    if (result && cx_checkState(result, CX_VALID)) {
        cx_define(result);
    }
    return result;
}

cx_object _cx_createChild(cx_object parent, cx_string name, cx_type type) {
    cx_object result = cx_declareChild(parent, name, type);
    if (result && cx_checkState(result, CX_VALID)) {
        cx_define(result);
    }
    return result;
}

/* Find the right constructor to call */
cx_int16 cx_delegateConstruct(cx_type t, cx_object o) {
    cx_function delegate = NULL;
    cx_int16 result = 0;

    if (t->kind == CX_COMPOSITE) {
        if (cx_interface(t)->kind == CX_CLASS) {
            cx_interface i = cx_interface(t);
            do {
                delegate = cx_class(i)->construct._parent.procedure;
                i = i->base;
            } while(i && !delegate);
        } else if (cx_interface(t)->kind == CX_PROCEDURE) {
            cx_interface i = cx_interface(t);
            do {
                delegate = cx_procedure(i)->bind._parent.procedure;
                i = i->base;
            } while(i && !delegate);
        }
    }

    if (delegate) {
        if (delegate->kind == CX_PROCEDURE_CDECL) {
            ((cx_int16 ___ (*)(cx_function f, void *result, void *args))delegate->impl)(delegate, &result, &o);
        } else {
            cx_call(delegate, &result, o);
        }
    }

    return result;
}

/* Define object */
cx_int16 cx_define(cx_object o) {
    cx_int16 result = 0;

    /* Only define valid, undefined objects */
    if (cx_checkState(o, CX_DECLARED)) {
        cx__object *_o = CX_OFFSET(o, -sizeof(cx__object));
        cx__persistent *_p = NULL;
        if (!cx_checkState(o, CX_DEFINED)) {
            cx_type t = cx_typeof(o);
            /* If object is instance of a class, call the constructor */
            if (cx_class_instanceof(cx_class_o, t)) {
                /* Attach observers to object */
                cx_class_attachObservers(cx_class(t), o);
                /* Call constructor - will potentially override observer params */
                result = cx_delegateConstruct(t, o);
            } else if (cx_class_instanceof(cx_procedure_o, t)) {
                result = cx_delegateConstruct(t, o);
            }

            if ((_p = cx__objectPersistent(_o))) {
                _p->owner = cx_getOwner();
            }

            if (!result) {
                _o->attrs.state |= CX_DEFINED;
                if (!cx_checkState(o, CX_VALID)) {
                    _o->attrs.state |= CX_VALID;
                }

                /* Notify observers of defined object */
                cx_notify(cx__objectObservable(_o), o, CX_ON_DEFINE);

                if (cx_class_instanceof(cx_class_o, t)) {
                    /* Start listening with final observer params */
                    cx_class_listenObservers(cx_class(t), o);
                }
            } else {
                /* Remove valid state */
                cx_invalidate(o);
            }
        } else {
            cx_notify(cx__objectObservable(_o), o, CX_ON_UPDATE);
        }
    }

    return result;
}

/* Destruct object */
void cx_delete(cx_object o) {
    cx__object* _o;
    cx__scope* scope;

    if (cx_checkAttr(o, CX_ATTR_SCOPED)) {
        _o = CX_OFFSET(o, -sizeof(cx__object));
        scope = cx__objectScope(_o);

        if (cx_adec(&scope->declared) == 0) {
            cx_drop(o);
            cx_notify(cx__objectObservable(_o), o, CX_ON_DELETE);
            cx__orphan(o);
            cx_release(o);
        }
    } else {
        cx_release(o);
    }
}

/* Invalidate object by removing valid flag */
void cx_invalidate(cx_object o) {
    cx__object* _o;
    _o = CX_OFFSET(o, -sizeof(cx__object));
    _o->attrs.state &= ~CX_VALID;
    /* Notify observers */
    cx_notify(cx__objectObservable(CX_OFFSET(o,-sizeof(cx__object))), o, CX_ON_INVALIDATE);
}

/* Get type */
cx_type cx_typeof(cx_object o) {
    cx__object* _o = CX_OFFSET(o, -sizeof(cx__object));
    return _o->type;
}

/* Get refcount */
cx_int32 cx_countof(cx_object o) {
    cx__object* _o;
    _o = CX_OFFSET(o, -sizeof(cx__object));
    return _o->refcount;
}

/* Get state */
cx_int8 cx_stateof(cx_object o) {
    cx__object* _o;
    _o = CX_OFFSET(o, -sizeof(cx__object));
    cx_int8 state = _o->attrs.state;
    return state;
}

/* Check for a state */
cx_bool cx_checkState(cx_object o, cx_int8 state) {
    cx__object* _o;
    _o = CX_OFFSET(o, -sizeof(cx__object));
    return (_o->attrs.state & state) == state;
}

/* Check for an attribute */
cx_bool cx_checkAttr(cx_object o, cx_int8 attr) {
    cx_bool result;
    cx__object* _o;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    result = TRUE;

    if (attr & CX_ATTR_SCOPED) {
        if (!_o->attrs.scope) result = FALSE;
    }
    if (attr & CX_ATTR_WRITABLE) {
        if (!_o->attrs.write) result = FALSE;
    }
    if (attr & CX_ATTR_OBSERVABLE) {
        if (!_o->attrs.observable) result = FALSE;
    }
    if (attr & CX_ATTR_PERSISTENT) {
        if (!_o->attrs.persistent) result = FALSE;
    }
    return result;
}

cx_object cx_assertType(cx_type type, cx_object o) {
    if (o && (o != type)) {
        if (!cx_instanceof(type, o)) {
            cx_id id1, id2;
            cx_error("object '%s' is not of type '%s'",
                cx_fullname(o, id1),
                cx_fullname(type, id2));
            cx_error("    type = %s\n", cx_fullname(cx_typeof(o), id1));
            cx_backtrace(stdout);
            abort();
        }
    }
    return o;
}

cx_bool _cx_instanceof(cx_type type, cx_object o) {
    cx_type objectType = cx_typeof(o);
    cx_bool result = TRUE;

    if (type != objectType) {
        cx_type t;

        result = FALSE;
        t = cx_typeof(o);

        if (t->kind == type->kind) {
            switch(type->kind) {
            case CX_COMPOSITE: {
                if (((cx_interface)type)->kind == CX_DELEGATE) {
                    /*result = cx_delegate_instanceof(cx_delegate(type), o);*/
                } else if (((cx_interface)type)->kind == CX_INTERFACE) {
                    if (cx_interface(t)->kind == CX_CLASS) {
                        cx_interface base = (cx_interface)t;
                        while (!result && base) {
                            cx_int32 i;
                            for (i = 0; i < ((cx_class)base)->implements.length; i++) {
                                if (_cx_interface_baseof(
                                    (cx_interface)((cx_class)base)->implements.buffer[i], (cx_interface)type)) {
                                    result = TRUE;
                                    break;
                                }
                            }
                            base = base->base;
                        }
                    }
                } else {
                    result = _cx_interface_baseof((cx_interface)t, (cx_interface)type);
                }
                break;
            }
            case CX_PRIMITIVE:
                switch(cx_primitive(type)->kind) {
                case CX_ENUM:
                case CX_BITMASK:
                    if (cx_parentof(o) == type) {
                        result = TRUE;
                    }
                    break;
                default:
                    break;
                }
                break;
            default:
                break;
            }
        }
    }

    return result;
}

/* Get parent (requires scoped object) */
cx_object cx_parentof(cx_object o) {
    cx__object* _o;
    cx__scope* scope;
    cx_object result;

    result = NULL;
    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    if (scope) {
        result = scope->parent;
    } else {
        goto err_not_scoped;
    }

    return result;
err_not_scoped:
    cx_critical("cx_parentof: object %p is not scoped.", o);
    return NULL;
}

/* Get name (requires scoped object) */
cx_string cx_nameof(cx_object o) {
    cx__object* _o;
    cx__scope* scope;
    cx_string result;

    result = NULL;
    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    if (scope) {
        result = scope->name;
    } else {
        goto err_not_scoped;
    }

    return result;
err_not_scoped:
    cx_critical("cx_nameof: object %p is not scoped.", o);
    return NULL;
}

cx_uint32 cx_scopeSize(cx_object o) {
    cx__object* _o;
    cx__scope* scope;
    cx_rbtree tree;
    cx_uint32 result;

    result = 0;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    if (scope) {
        tree = scope->scope;
        if (tree) {
            result = cx_rbtreeSize(tree);
        }
    } else {
        goto err_not_scoped;
    }

    return result;
err_not_scoped:
    cx_error("cx_scopeof: object <%p> is not scoped", o);
    return 0;
}

/* Walk objects in scope */
cx_int32 cx_scopeWalk(cx_object o, cx_scopeWalkAction action, void* userData) {
    cx_int32 result;
    cx__scope* scope;

    if (!o) {
        o = root_o;
    }

    result = 1;
    scope = cx__objectScope(CX_OFFSET(o, -sizeof(cx__object)));
    if (scope) {
        if (scope->scope) {
            cx_rwmutexRead(&scope->scopeLock);
            result = cx_rbtreeWalk(scope->scope, (cx_walkAction)action, userData);
            cx_rwmutexUnlock(&scope->scopeLock);
        }
    }

    return result;
}

/* Obtain scoped identifier (serves as global unique identifier) */
cx_string cx_fullname(cx_object o, cx_id buffer) {
    cx_object stack[CX_MAX_SCOPE_DEPTH];
    cx_uint32 depth;
    cx_string name;
    cx_char* ptr;
    cx_uint32 len;

    depth = 0;

    if (!o) {
        cx_seterr("no object provided");
        return NULL;
    }

    if (!buffer) {
        cx_seterr("no buffer provided");
        return NULL;
    }

    if (!cx_checkAttr(o, CX_ATTR_SCOPED)) {
        sprintf(buffer, "<%p>", o);
    } else {
        do {
            stack[depth++] = o;
        }while((o = cx_parentof(o)));

        ptr = buffer;
        if (depth == 1) {
            *(cx_uint16*)ptr = CX_SCOPE_HEX;
            ptr += 2;
        } else {
            while(depth) {
                depth--;
                o = stack[depth];

                if ((name = cx_nameof(o))) {
                    /* Copy scope operator */
                    *(cx_uint16*)ptr = CX_SCOPE_HEX;
                    ptr += 2;

                    /* Copy name */
                    len = strlen(name);
                    memcpy(ptr, name, len + 1);
                    ptr += len;
                }
            }
        }
        *ptr = '\0';
    }

    return buffer;
}

static cx_object* cx_scopeStack(cx_object o, cx_object scopeStack[], cx_uint32 *length) {
    cx_object ptr;
    cx_uint32 i;

    ptr = o;
    i = 0;
    while(ptr) {
        scopeStack[i] = ptr;
        ptr = cx_parentof(ptr);
        i++;
    }
    scopeStack[i] = NULL;
    if (length) {
        *length = i;
    }

    return scopeStack;
}

cx_string cx_relname(cx_object from, cx_object o, cx_id buffer) {
    cx_object from_s[CX_MAX_SCOPE_DEPTH];
    cx_object o_s[CX_MAX_SCOPE_DEPTH];
    cx_uint32 from_i, o_i;
    cx_char* ptr;
    cx_uint32 length;

    cx_assert(from != NULL, "relname called with NULL for parameter 'from'.");
    cx_assert(o != NULL, "relname called with NULL for parameter 'to'.");

    if (from == o) {
        strcpy(buffer, ".");
    } else if (from == root_o) {
        cx_id buff;
        cx_fullname(o, buff);
        strcpy(buffer, buff + 2);
    } else {
        cx_scopeStack(from, from_s, &from_i);
        cx_scopeStack(o, o_s, &o_i);

        if (from_i > o_i) {
            cx_seterr("origin is not in path of object");
            return NULL;
        }

        from_i--;
        o_i--;
        while(from_s[from_i] == o_s[o_i]) {
            from_i--;
            o_i--;
            if (!o_i || !from_i) {
                break;
            }
        }

        if (from_s[from_i] == o_s[o_i] && o_i) {
            o_i--;
        }

        ptr = buffer;
        while(o_i) {
            length = strlen(cx_nameof(o_s[o_i]));
            memcpy(ptr, cx_nameof(o_s[o_i]), length);
            ptr += length;
            *(cx_uint16*)ptr = CX_SCOPE_HEX;
            ptr += 2;
            o_i--;
        }
        length = strlen(cx_nameof(o_s[o_i]));
        memcpy(ptr, cx_nameof(o_s[o_i]), length);
        ptr += length;
        *ptr = '\0';
    }
    return buffer;
}

/* Get timestamp (requires persistent object) */
cx_time cx_timestampof(cx_object o) {
    cx__object* _o;
    cx__persistent* persistent;
    cx_time result = {0, 0};

    _o = CX_OFFSET(o, -sizeof(cx__object));
    persistent = cx__objectPersistent(_o);
    if (persistent) {
        result = persistent->timestamp;
    } else {
        goto err_not_persistent;
    }

    return result;
err_not_persistent:
    cx_critical("cx_timestampof: object %p is not persistent.", o);
    return result;
}

cx_object cx_ownerof(cx_object o) {
    cx__object* _o;
    cx__persistent* persistent;
    cx_object result = NULL;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    persistent = cx__objectPersistent(_o);
    if (persistent) {
        result = persistent->owner;
    } else {
        goto err_not_persistent;
    }

    return result;
err_not_persistent:
    cx_critical("cx_ownerof: object %p is not persistent.", o);
    return result;   
}

/* Destruct object. */
cx_uint16 cx__destruct(cx_object o) {
    cx__object* _o;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    cx_ainc(&_o->refcount);

    /* Only the following steps if the object is valid. */
    if (!cx_checkState(o, CX_DESTRUCTED)) {
        cx_vtable *ot;

        ot = cx_class_getObserverVtable(o);

        /* Prevents from calling destructor nested */
        _o->mm.cycles = -1;

        /* Only do the following steps if the object is defined */
        if (cx_checkState(o, CX_DEFINED)) {
            /* From here, object is marked as destructed. */
            _o->attrs.state |= CX_DESTRUCTED;

            /* Integrity check */
            if (cx_checkAttr(o, CX_ATTR_SCOPED)) {
                if (cx_parentof(o) == corto_lang_o) {
                    cx_id id;
                    cx_critical("illegal attempt to destruct builtin-object '%s'.", cx_fullname(o, id));
                }
            }

            /* Call object destructor */
            if (cx__destructor(o)) {
                return -1;
            }
        }

        /* Notify destruct (scoped objects are destructed by cx_delete) */
        if (!cx_checkAttr(o, CX_ATTR_SCOPED)) {
            cx_notify(cx__objectObservable(CX_OFFSET(o,-sizeof(cx__object))), o, CX_ON_DELETE);
        }

        cx_deinit(o);

        /* Deinit observable */
        if (cx_checkAttr(o, CX_ATTR_OBSERVABLE)) {
            cx__deinitObservable(o);
        }

        /* Free type of object */
        cx_release(cx_typeof(o));

        /* Deinit writable */
        if (cx_checkAttr(o, CX_ATTR_WRITABLE)) {
            cx__deinitWritable(o);
        }

        if (cx_checkAttr(o, CX_ATTR_SCOPED)) {
            cx__deinitScope(o);
        } else {
            /* Remove from anonymous cache */
            cx_mutexLock(&cx_adminLock);
            cx_llRemove(cx_anonymousObjects, o);
            cx_mutexUnlock(&cx_adminLock);
        }

        /* Reset template observable table */
        if (ot && ot->buffer) {
            ot->buffer = NULL; /* Buffer is allocated as part of object - so no leakage */
            ot->length = 0;
        }
    }

    /* Although after the destruct-operation it is ensured that this object no longer participates in any cycles, it cannot be assumed
     * that all objects using this are free'd. For example, another object that has multiple reference cycles might still be
     * referencing this object, but can itself not yet be free'd because of the other cycles, which cannot be solved by the destruction
     * of this object. Therefore when the reference count of this object is non-zero, it cannot yet be free'd.
     */

    cx_adec(&_o->refcount);

    if (!cx_countof(o)) {
        cx_dealloc(cx__objectStartAddr(_o));
        return 0;
    } else {
        return cx_countof(o);
    }
}

/* Name-based tracing */
/*#define CX_TRACE_DEBUG
#define CX_TRACE "48"*/
#ifdef CX_TRACE
#define CX_TRACE_KEEP(o)\
    {\
        cx__object* _o;\
        _o = CX_OFFSET(o, -sizeof(cx__object));\
        if (cx__objectScope(_o)) {\
            if ((cx_nameof(o) == CX_TRACE) || (CX_TRACE && (cx_nameof(o) && !strcmp(cx_nameof(o), CX_TRACE)))) {\
                CX_TRACE_ADDR = o;\
                cx_trace("keep (%p)'%s' -> %d (context = \"%s\")", o, cx_nameof(o), _o->refcount, context);\
                if (src) {\
                    cx_id id;\
                    cx_trace("    source: %s", cx_fullname(src, id));\
                }\
                cx_backtrace(stdout);\
            }\
        }\
    }
#define CX_TRACE_FREE(object)\
    {\
        cx__object* _o;\
        _o = CX_OFFSET(o, -sizeof(cx__object));\
        if (cx__objectScope(_o)) {\
            if ((cx_nameof(o) == CX_TRACE) || (CX_TRACE && (cx_nameof(o) && !strcmp(cx_nameof(o), CX_TRACE)))) {\
                CX_TRACE_ADDR = o;\
                cx_trace("free (%p)'%s' -> %d (context = \"%s\", cycles=%d)", o, cx_nameof(o), _o->refcount, context, _o->mm.cycles);\
                if (src) {\
                    cx_id id;\
                    cx_trace("    source: %s", cx_fullname(src, id));\
                }\
                cx_backtrace(stdout);\
            }\
        }\
    }
#endif

/* Type-based tracing */
/* #define CX_TRACE_DEBUG
#define CX_TRACE_TYPE "Local"*/
#ifdef CX_TRACE_TYPE
#define CX_TRACE_KEEP(o)\
    {\
        cx__object* _o;\
        _o = CX_OFFSET(o, -sizeof(cx__object));\
        if ((cx_nameof(cx_typeof(o)) == CX_TRACE_TYPE) || (CX_TRACE_TYPE && (cx_nameof(cx_typeof(o)) && !strcmp(cx_nameof(cx_typeof(o)), CX_TRACE_TYPE)))) {\
            cx_id id;\
            cx_trace("keep (%p)'%s' -> %d (context = \"%s\")", o, cx_fullname(o, id), cx_countof(o), context);\
            if (src) {\
                cx_id id;\
                cx_trace("    source: %s", cx_fullname(src, id));\
            }\
            cx_backtrace(stdout);\
        }\
    }
#define CX_TRACE_FREE(object)\
    {\
        cx__object* _o;\
        _o = CX_OFFSET(o, -sizeof(cx__object));\
        if ((cx_nameof(cx_typeof(o)) == CX_TRACE_TYPE) || (CX_TRACE_TYPE && (cx_nameof(cx_typeof(o)) && !strcmp(cx_nameof(cx_typeof(o)), CX_TRACE_TYPE)))) {\
            cx_id id;\
            cx_trace("free (%p)'%s' -> %d (context = \"%s\", cycles=%d)", o, cx_fullname(o, id), cx_countof(o), context, _o->mm.cycles);\
            if (src) {\
                cx_id id;\
                cx_trace("    source: %s", cx_fullname(src, id));\
            }\
            cx_backtrace(stdout);\
        }\
    }
#endif

/* Parent based tracing */
/*#define CX_TRACE_DEBUG
#define CX_TRACE_TYPEPARENT "ast"*/
#ifdef CX_TRACE_TYPEPARENT
#define CX_TRACE_KEEP(o)\
    {\
        cx__object* _o;\
        _o = CX_OFFSET(cx_typeof(o), -sizeof(cx__object));\
        if (cx__objectScope(_o)) {\
            if ((cx_nameof(cx_parentof(cx_typeof(o))) == CX_TRACE_TYPEPARENT) || (CX_TRACE_TYPEPARENT && (cx_nameof(cx_parentof(cx_typeof(o))) && !strcmp(cx_nameof(cx_parentof(cx_typeof(o))), CX_TRACE_TYPEPARENT)))) {\
                cx_id id;\
                cx_trace("keep (%p)'%s' -> %d (context = \"%s\")", o, cx_fullname(o, id), cx_countof(o), context);\
                if (src) {\
                    cx_id id;\
                    cx_trace("    source: %s", cx_fullname(src, id));\
                }\
                /*cx_backtrace(stdout);*/\
            }\
        }\
    }
#define CX_TRACE_FREE(object)\
    {\
        cx__object* _o;\
        _o = CX_OFFSET(cx_typeof(o), -sizeof(cx__object));\
        if (cx__objectScope(_o)) {\
            if ((cx_nameof(cx_parentof(cx_typeof(o))) == CX_TRACE_TYPEPARENT) || (CX_TRACE_TYPEPARENT && (cx_nameof(cx_parentof(cx_typeof(o))) && !strcmp(cx_nameof(cx_parentof(cx_typeof(o))), CX_TRACE_TYPEPARENT)))) {\
                cx_id id;\
                cx_trace("free (%p)'%s' -> %d (context = \"%s\", cycles=%d)", o, cx_fullname(o, id), cx_countof(o), context, _o->mm.cycles);\
                if (src) {\
                    cx_id id;\
                    cx_trace("    source: %s", cx_fullname(src, id));\
                }\
                /*cx_backtrace(stdout);\*/\
            }\
        }\
    }
#endif


#ifndef CX_TRACE
#ifndef CX_TRACE_TYPE
#ifndef CX_TRACE_TYPEPARENT
#define CX_TRACE_KEEP(o)
#define CX_TRACE_FREE(o)
#endif
#endif
#endif

#ifdef CX_TRACE_DEBUG
cx_object CX_TRACE_ADDR = NULL;
#endif
cx_int32 cx_claim_ext(cx_object src, cx_object o, cx_string context) {
    cx__object* _o;
    cx_uint32 i;
    CX_UNUSED(src);
    CX_UNUSED(context);

    _o = CX_OFFSET(o, -sizeof(cx__object));
    i = cx_ainc(&_o->refcount);

#ifdef CX_TRACE_DEBUG
    if (CX_TRACE_ADDR == o) {
        cx_id id, id2;
        cx_trace("keep %s of type %s -> %d (cycles=%d, source=%p, context='%s')", cx_fullname(o, id), cx_fullname(cx_typeof(o), id2), i, _o->mm.cycles, src, context);
        /*cx_backtrace(stdout);*/
    }

    CX_TRACE_KEEP(o);

    if (i == 0) {
        cx_id id1, id2;
        cx_critical("keep resulted in refcount of 0 for object '%s' of type '%s'",
                cx_fullname(o, id1), cx_fullname(cx_typeof(o), id2));
    }
#endif

    return i;
}

cx_int32 cx_release_ext(cx_object src, cx_object o, cx_string context) {
    cx_int32 i;
    cx__object* _o;
    CX_UNUSED(src);
    CX_UNUSED(context);

    _o = CX_OFFSET(o, -sizeof(cx__object));
    i = cx_adec(&_o->refcount);

#ifdef CX_TRACE_DEBUG
    if ((CX_TRACE_ADDR == o)) {
        cx_id id, id2;
        cx_trace("free (%p)%s of type %s -> %d (cycles=%d, valid=%d, destructed = %d, source=%p, context='%s')",
                o, cx_fullname(o, id), cx_fullname(cx_typeof(o), id2), i, _o->mm.cycles, cx_checkState(o, CX_VALID), cx_checkState(o, CX_DESTRUCTED), src, context);
        /*cx_backtrace(stdout);*/
    }

    CX_TRACE_FREE(o);
#endif

    if (i == _o->mm.cycles) {
        cx__destruct(o);

    /* If an invalid scoped object doesn't have a name, it must still be free'd - can occur when objects are
     * dangling because of double cycles. */
    } else if (!i && cx_checkState(o, CX_DESTRUCTED) && (_o->mm.cycles == 65535)) {
        cx__destruct(o);
    }
    if (i < 0) {
        cx_id id, typeId;
        cx_critical("negative reference count of object (%p) '%s' of type '%s'", o, cx_fullname(o, id), cx_fullname(cx_typeof(o), typeId));
        cx_backtrace(stdout);
    }

    return i;
}

cx_int32 cx_claim(cx_object o) {
    return cx_claim_ext(NULL, o, NULL);
}

cx_int32 cx_release(cx_object o) {
    return cx_release_ext(NULL, o, NULL);
}

typedef struct cx_dropWalk_t {
    cx_ll objects;
}cx_dropWalk_t;

/* Collect objects in scope, so they can be removed outside of scopeLock. */
static int cx_dropWalk(void* o, void* userData) {
    cx__object* _o;
    cx__scope* scope;
    cx_dropWalk_t* data;

    data = userData;

    /* Drops are recursive */
    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    if (scope) {
        cx_rwmutexRead(&scope->scopeLock);
        if (scope->scope) {
            cx_rbtreeWalk(scope->scope, cx_dropWalk, data);
        }
        if (scope->attached) {
            cx_llWalk(scope->attached, cx_dropWalk, data);
            cx_llFree(scope->attached);
            scope->attached = NULL;
        }
        cx_rwmutexUnlock(&scope->scopeLock);
    }

    /* Prevent object from being deleted when scopeLock is released, which
     * would result in invalid reference in list. */
    cx_claim(o);

    /* Insert object in list */
    if (!data->objects) {
        data->objects = cx_llNew();
    }
    cx_llInsert(data->objects, o);

    return 1;
}

void cx_drop(cx_object o) {
    cx__object* _o;
    cx__scope* scope;
    cx_dropWalk_t walkData;

    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);
    if (scope) {
        cx_iter iter;
        cx_object collected;

        /* Because object refcounts can reach zero after a free, a
         * walk in which objects are collected is needed first. During
         * destruction of an object, this scopeLock is also required,
         * which would result in deadlocks. */

        cx_rwmutexRead(&scope->scopeLock);
        walkData.objects = NULL;
        if (scope->scope) {
            cx_rbtreeWalk(scope->scope, cx_dropWalk, &walkData);
        }
        if (scope->attached) {
            cx_llWalk(scope->attached, cx_dropWalk, &walkData);
            cx_llFree(scope->attached);
            scope->attached = NULL;
        }
        cx_rwmutexUnlock(&scope->scopeLock);

        /* Free objects outside scopeLock */
        if (walkData.objects) {
            iter = cx_llIter(walkData.objects);
            while(cx_iterHasNext(&iter)) {
                collected = cx_iterNext(&iter);
                cx_delete(collected);
                /* Double free - because cx_drop itself introduced a keep. */
                cx_release(collected);
            }
            cx_llFree(walkData.objects);
        }
    } else {
        cx_critical("cx_drop: object <%p> is not scoped.", o);
    }
}

cx_object cx_lookupLowercase(cx_object o, cx_string name) {
    cx_object result;
    cx__object *_o, *_result;
    cx__scope* scope;
    cx_rbtree tree;

    if (!o) {
        o = root_o;
    }

    _o = CX_OFFSET(o, -sizeof(cx__object));
    scope = cx__objectScope(_o);

    if (scope) {
        cx_rwmutexRead(&scope->scopeLock);
        if ((tree = scope->scope)) {
            if ((!cx_rbtreeHasKey_w_cmp(tree, name, (void**)&result, cx_objectCompareLookup))) {
                result = NULL;
            } else {
                /* Keep object. If the refcount was zero, this object will be deleted soon, so prevent the object from being referenced again. */
                if (cx_claim(result) == 1) {
                     /* Set the refcount to zero again. There can be no more objects that are looking up this object right now because
                      * we have the scopeLock of the parent. Additionally, the object will not yet have been free'd because the destruct
                      * function also needs the parent's scopelock to remove the object from the scope.
                      *
                      * The refcount needs to be re-set to zero, because after the scopeLock is released, other threads - other than the destruct
                      * thread might try to acquire this object. Setting the refcount back to zero will enable these lookups to also detect
                      * that the object is being deleted.
                      */
                    _result = CX_OFFSET(result, -sizeof(cx__object));
                    _result->refcount = 0;
                    result = NULL;
                }
            }
        } else {
            result = NULL;
        }
        cx_rwmutexUnlock(&scope->scopeLock);
    } else {
        cx_id id;
        cx_error("cx_lookup: object '%s' has no scope", cx_fullname(o, id));
        goto error;
    }

    return result;
error:
    return NULL;
}

cx_object cx_lookup(cx_object o, cx_string name) {
    cx_id lower;
    char *ptr = name, ch;
    char *bptr = lower;
    for(; (ch = *ptr); ptr++) *(bptr++) = tolower(ch);
    *bptr = '\0';
    return cx_lookupLowercase(o, lower);
}

/* Event handling. */
static cx__observer* cx_observerFind(cx_ll on, cx_observer observer, cx_object this) {
    cx__observer* result;
    cx_iter iter;

    result = NULL;

    if (on) {
        iter = cx_llIter(on);
        while(cx_iterHasNext(&iter)) {
            result = cx_iterNext(&iter);
            if ((result->observer == observer) && (result->_this == this)) {
                break;
            } else {
                result = NULL;
            }
        }
    }

    return result;
}

/* Copyout observers */
static void cx_observersCopyOut(cx_ll list, cx__observer** observers) {
    cx_iter iter;
    cx_uint32 i;

    iter = cx_llIter(list);
    i = 0;
    while(cx_iterHasNext(&iter)) {
        observers[i] = cx_iterNext(&iter);
        observers[i]->count++;
        i++;
    }
    observers[i] = NULL;
}

/* Free observer-array */
static void cx_observersFree(cx__observer** observers) {
    cx__observer* observer;
    while((observer = *observers)) {
        if (!cx_adec(&observer->count)) {
            cx_dealloc(observer);
        }
        ++observers;
    }
}

/* Create observer-array */
static cx__observer** cx_observersArrayNew(cx_ll list) {
    cx__observer** array;

    array = cx_alloc((cx_llSize(list) + 1) * sizeof(cx__observer*));
    cx_observersCopyOut(list, array);

    /* Observers start from the second element */
    return array;
}

/* There is a small chance that a thread simultaneously with cx_listen obtains a pointer
 * to an old observers-array, then gets scheduled out and this function in another thread
 * deletes that array. Although unlikely this scenario must be addressed. */
static void cx_observersArrayFree(cx__observer** array) {
    if (array) {
        cx_observersFree(array);
        cx_dealloc(array);
    }
}

/* Walk childs recursively, set whether a lock is required when updating a
 * child object because a parent has an observer interested in childs that
 * requires locking. */
void cx_setrefChildLockRequired(cx_object observable) {
    if (cx_checkAttr(observable, CX_ATTR_SCOPED)) {
        cx_uint32 i;
        cx_object child;
        cx__observable *childObservable;
        cx_objectseq scope = cx_scopeClaim(observable);

        for (i = 0; i < scope.length; i++) {
            child = scope.buffer[i];
            if ((childObservable = cx__objectObservable(CX_OFFSET(child, -sizeof(cx__object))))) {
                cx_rwmutexWrite(&childObservable->childLock);
                if (cx_checkAttr(child,CX_ATTR_WRITABLE)) {
                    childObservable->lockRequired = TRUE;
                }
                cx_setrefChildLockRequired(child);
                cx_rwmutexUnlock(&childObservable->childLock);
            }
        }

        cx_scopeRelease(scope);
    }
}

/* #define CX_TRACE_NOTIFICATIONS */

#ifdef CX_TRACE_NOTIFICATIONS
static cx_uint32 indent = 0;
#endif

/* Notify one observer */
static void cx_notifyObserver(cx__observer *data, cx_object observable, cx_object source, cx_uint32 mask, int depth) {
    cx_observer observer = data->observer;
    cx_eventMask observerMask = data->mask;

    if ((mask & observerMask) && (!depth || (observerMask & CX_ON_TREE))) {
        cx_dispatcher dispatcher = data->dispatcher;
#ifdef CX_TRACE_NOTIFICATIONS
{
    cx_id id1, id2, id3;
    printf("%*s [notify] observable '%s' observer '%s' me '%s'\n",
            indent * 3, "",
            cx_fullname(observable, id1),
            cx_fullname(observer, id2),
            cx_fullname(data->_this, id3));
}
indent++;
#endif

        if (!dispatcher) {
            data->notify(data, data->_this, observable, mask);
        } else {
            if (!data->_this || (data->_this != source)) {
                cx_attr oldAttr = cx_setAttr(0);
                cx_observableEvent event = cx_declare(cx_type(cx_observableEvent_o));
                cx_setAttr(oldAttr);

                cx_setref(&event->observer, observer);
                cx_setref(&event->me, data->_this);
                cx_setref(&event->observable, observable);
                cx_setref(&event->source, source);

                cx_dispatcher_post(dispatcher, cx_event(event));
            }
        }
#ifdef CX_TRACE_NOTIFICATIONS
        indent--;
#endif
    }
}

/* If an observer is subscribed to NEW events align it with existing objects so that
 * an observer doesn't need to do an object-walk over the observable manually to discover
 * all objects.
 */
typedef struct cx_observerAlignData {
    cx__observer *observer;
    cx_object observable;
    int mask;
    int depth;
} cx_observerAlignData;

int cx_observerAlignScope(cx_object o, void *userData) {
    cx_observerAlignData *data = userData;

    if (((data->mask & CX_ON_DECLARE) && (data->mask & CX_ON_SCOPE)) ||
        ((data->mask & CX_ON_DECLARE) && (data->mask & CX_ON_SCOPE))) {
        cx_notifyObserver(data->observer, data->observable, o, CX_ON_DECLARE, data->depth);
    }

    if (cx_checkAttr(o, CX_ATTR_OBSERVABLE) && cx_checkAttr(o, CX_ATTR_PERSISTENT)) {
        if ((data->mask & CX_ON_DEFINE) && (data->mask & (CX_ON_SCOPE|CX_ON_TREE)) && cx_checkState(o, CX_DEFINED)) {
            cx_notifyObserver(data->observer, o, o, CX_ON_DEFINE, data->depth);
        }
    }

    if (data->mask & CX_ON_TREE) {
        int result = 1;
        cx_objectseq scope;
        cx_uint32 i;
        data->depth++;
        scope = cx_scopeClaim(o);
        for(i = 0; i < scope.length; i++) {
            result = cx_observerAlignScope(scope.buffer[i], userData);
        }
        cx_scopeRelease(scope);
        data->depth--;
        return result;
    } else {
        return 1;
    }
}

void cx_observerAlign(cx_object observable, cx__observer *observer, int mask) {
    cx_observerAlignData walkData;
    cx_objectseq scope;

    /* Do recursive walk over scope */
    walkData.observable = observable;
    walkData.observer = observer;
    walkData.mask = mask;
    walkData.depth = 0;

    if (((mask & CX_ON_DECLARE) && (mask & CX_ON_SELF) && cx_checkState(observable, CX_DECLARED)) || 
        ((mask & CX_ON_DEFINE) && (mask & CX_ON_SELF) && cx_checkState(observable, CX_DEFINED))) {
        cx_notifyObserver(observer, observable, observable, mask, 0);
    }

    scope = cx_scopeClaim(observable);
    cx_objectseqForeach(scope, o) {
        cx_observerAlignScope(o, &walkData);
    }
    cx_scopeRelease(scope);
}

/* Add observer to observable */
cx_int32 cx_listen(cx_object this, cx_observer observer, cx_eventMask mask, cx_object observable, cx_dispatcher dispatcher) {
    cx__observer* _observerData = NULL;
    cx__observable* _o;
    cx_bool added;
    cx__observer **oldSelfArray = NULL, **oldChildArray = NULL;

    if (!observable) {
        observable = root_o;
    }

    /* If the observer is a template observer and 'this' is not yet defined,
     * don't start listening right away but set the observable in the list of
     * class observables */
    if (observer->_template && this && !cx_checkState(this, CX_DEFINED)) {
        cx_any thisAny = {cx_typeof(this), this, FALSE};
        cx_class_listen(thisAny, observer, mask, observable, dispatcher);
    } else {

        /* Test for error conditions before making changes */
        if (mask & (CX_ON_SCOPE|CX_ON_TREE)) {
            if (!cx_checkAttr(observable, CX_ATTR_SCOPED)) {
                cx_id id, id2;
                cx_seterr("corto::listen: cannot listen to childs of non-scoped observable '%s' (observer %s)",
                        cx_fullname(observable, id),
                        cx_fullname(observer, id2));
                goto error;
            }
        }

        if (!cx_checkAttr(observable, CX_ATTR_OBSERVABLE)) {
            cx_id id;
            cx_assert(0, "corto::listen: object '%s' is not an observable", cx_fullname(observable, id));
            goto error;
        }

        _o = cx__objectObservable(CX_OFFSET(observable, -sizeof(cx__object)));

    #ifdef CX_TRACE_NOTIFICATIONS
        {
            cx_id id1, id2, id3;
            printf("%*s [listen] observable '%s' observer '%s' me '%s'%s%s%s%s%s%s\n",
                    indent * 3, "",
                    cx_fullname(observable, id1),
                    cx_fullname(observer, id2),
                    cx_fullname(this, id3),
                    mask & CX_ON_SELF ? " self" : "",
                    mask & CX_ON_SCOPE ? " scope" : "",
                    mask & CX_ON_TREE ? " tree" : "",
                    mask & CX_ON_DECLARE ? " declare" : "",
                    mask & CX_ON_DEFINE ? " define" : "",
                    mask & CX_ON_UPDATE ? " update" : "");
        }
    #endif

        /* Create observerData */
        _observerData = cx_alloc(sizeof(cx__observer));
        _observerData->observer = observer;
        _observerData->_this = this;
        _observerData->count = 0;
        _observerData->mask = mask;
        _observerData->dispatcher = dispatcher;

        /* Resolve the kind of the observer. This reduces the number of
         * conditions that need to be evaluated in the notifyObserver function. */
        if (cx_function(observer)->kind == CX_PROCEDURE_CDECL) {
            if (this) {
                _observerData->notify = cx_notifyObserverThis;
            } else {
                _observerData->notify = cx_notifyObserverDefault;
            }
        } else {
            if (this) {
                _observerData->notify = cx_notifyObserverThis;
            } else {
                _observerData->notify = cx_notifyObserverDefault;
            }
        }

        added = FALSE;

        /* If observer must trigger on updates of me, add it to onSelf list */
        if (mask & CX_ON_SELF) {
            cx_rwmutexWrite(&_o->selfLock);
            if (!cx_observerFind(_o->onSelf, observer, this)) {
                if (!_o->onSelf) {
                    _o->onSelf = cx_llNew();
                }
                cx_llAppend(_o->onSelf, _observerData);
                _observerData->count++;
                added = TRUE;

                /* Build new observer array. This array can be accessed without locking and is
                 * faster than walking the linked list. */
                oldSelfArray = _o->onSelfArray;
                _o->onSelfArray = cx_observersArrayNew(_o->onSelf);
            }
            if (mask & CX_ON_VALUE) {
                if (cx_checkAttr(observable, CX_ATTR_WRITABLE)) {
                    _o->lockRequired = TRUE;
                }
            }
            cx_rwmutexUnlock(&_o->selfLock);
        }

        /* If observer must trigger on updates of childs, add it to onChilds list */
        if (mask & (CX_ON_SCOPE|CX_ON_TREE)) {
            cx_rwmutexWrite(&_o->childLock);
            if (!cx_observerFind(_o->onChild, observer, this)) {
                if (!_o->onChild) {
                    _o->onChild = cx_llNew();
                }

                cx_llAppend(_o->onChild, _observerData);
                _observerData->count++;
                added = TRUE;

                /* Build new observer array. This array can be accessed without locking and is
                 * faster than walking a linked list. */
                oldChildArray = _o->onChildArray;
                _o->onChildArray = cx_observersArrayNew(_o->onChild);
            }

            if (mask & CX_ON_VALUE) {
                if (!_o->childLockRequired) {
                    _o->childLockRequired = TRUE;
                    cx_setrefChildLockRequired(observable);
                }
            }
            cx_rwmutexUnlock(&_o->childLock);
        }

        if (!added) {
            cx_dealloc(_observerData);
        } else {
            /* If observer is subscribed to declare/define events, align observer with existing */
            if ((mask & CX_ON_DECLARE) || (mask & CX_ON_DEFINE)) {
                cx_observerAlign(observable, _observerData, mask);
            }
        }

        /* From this point onwards the old observer arrays are no longer accessible. However, since notifications can
         * still be in progress these arrays can't be deleted yet. Therefore wait until the arrays are no longer being
         * used.
         *
         * The administration where this information is stored is not protected by locking so that notifying objects can
         * remain lock-free. There is however a slight chance that a notification pushed the old array to the administration
         * but that this change is not yet visible due to a number of issues w.r.t. concurrency. In this case the functions
         * below will assume the array is unused, which is incorrect.
         *
         * To be absolutely sure that the observed administration is up to date a memory barrier is required here. A simple
         * mutex will not do since this would encumber the notifications too much.
         */
         /*__atomic_thread_fence (__ATOMIC_SEQ_CST);*/

        if (cx_observersWaitForUnused(oldSelfArray)) {
            cx_observersArrayFree(oldSelfArray);
        }
        if (cx_observersWaitForUnused(oldChildArray)) {
            cx_observersArrayFree(oldChildArray);
        }
    }

    return 0;
error:
    return -1;
}

/* Remove observer from observable - TODO update lockRequired and parentObserves. */
cx_int32 cx_silence(cx_object this, cx_observer observer, cx_eventMask mask, cx_object observable) {
    cx__observer* observerData;
    cx__observable* _o;
    cx__observer **oldSelfArray = NULL, **oldChildArray = NULL;

    if (cx_checkAttr(observable, CX_ATTR_OBSERVABLE)) {
        _o = cx__objectObservable(CX_OFFSET(observable, -sizeof(cx__object)));
        observerData = NULL;

        /* If observer triggered on updates of me, remove from onSelf list */
        if (mask & CX_ON_SELF) {
            cx_rwmutexWrite(&_o->selfLock);
            observerData = cx_observerFind(_o->onSelf, observer, this);
            if (observerData) {
                cx_llRemove(_o->onSelf, observerData);
                observerData->count--;

                /* Build new observer array */
                oldSelfArray = _o->onSelfArray;
                _o->onSelfArray = cx_observersArrayNew(_o->onSelf);
#ifdef CX_TRACE_NOTIFICATIONS
            {
                cx_id id1, id2, id3;
                printf("%*s [silence] observable '%s' observer '%s' me '%s'\n",
                        indent * 3, "",
                        cx_fullname(observable, id1),
                        cx_fullname(observer, id2),
                        cx_fullname(this, id3));
            }
#endif
            }
            cx_rwmutexUnlock(&_o->selfLock);
        }

        /* If observer triggered on updates of childs, remove from onChilds list */
        if (mask & (CX_ON_SCOPE|CX_ON_TREE)) {
            if (cx_checkAttr(observable, CX_ATTR_SCOPED)) {
                cx_rwmutexWrite(&_o->childLock);
                observerData = cx_observerFind(_o->onChild, observer, this);
                if (observerData) {
                    cx_llRemove(_o->onChild, observerData);
                    observerData->count--;

                    /* Build new observer array */
                    oldChildArray = _o->onChildArray;
                    _o->onChildArray = cx_observersArrayNew(_o->onChild);
                }
                cx_rwmutexUnlock(&_o->childLock);
            } else {
                cx_error(0, "corto::listen: observer subscribed on childs of non-scoped object");
                goto error;
            }
        }
    } else {
        cx_id id;
        cx_error("object '%s' is not an observable", cx_fullname(observable, id));
        goto error;
    }

    /* See comments in cx_listen */
    /*__atomic_thread_fence (__ATOMIC_SEQ_CST);*/

    if (cx_observersWaitForUnused(oldSelfArray)) {
        cx_observersArrayFree(oldSelfArray);
    }
    if (cx_observersWaitForUnused(oldChildArray)) {
        cx_observersArrayFree(oldChildArray);
    }
    /*if (observerData) {
        TODO: cx_dealloc(observerData);
    }*/

    return 0;
error:
    return -1;
}

cx_bool cx_listening(cx_object observable, cx_observer observer, cx_object this) {
    cx__observer* observerData;
    cx__observable* _o;
    cx_bool result = FALSE;

    _o = cx__objectObservable(CX_OFFSET(observable, -sizeof(cx__object)));
    observerData = NULL;

    /* If observer triggered on updates of me, remove from onSelf list */
    if (_o) {
        if (observer->mask & CX_ON_SELF) {
            cx_rwmutexWrite(&_o->selfLock);
            observerData = cx_observerFind(_o->onSelf, observer, this);
            if (observerData) {
                result = TRUE;
            }
            cx_rwmutexUnlock(&_o->selfLock);
        }

        if (!result) {
            if (cx_checkAttr(observable, CX_ATTR_OBSERVABLE)) {
                if (observer->mask & CX_ON_SCOPE) {
                    if (cx_checkAttr(observable, CX_ATTR_SCOPED)) {
                        cx_rwmutexWrite(&_o->childLock);
                        observerData = cx_observerFind(_o->onChild, observer, this);
                        if (observerData) {
                            result = TRUE;
                        }
                        cx_rwmutexUnlock(&_o->childLock);
                    }
                }
            } else {
                cx_id id;
                cx_error("object '%s' is not an observable", cx_fullname(observable, id));
                goto error;
            }
        }
    }

    return result;
error:
    return FALSE;
}

static void cx_notifyObserverDefault(cx__observer* data, cx_object this, cx_object observable, cx_uint32 mask) {
    cx_function f = cx_function(data->observer);
    CX_UNUSED(this);
    CX_UNUSED(mask);
    cx_call(f, NULL, NULL, observable);
}

static void cx_notifyObserverThis(cx__observer* data, cx_object this, cx_object observable, cx_uint32 mask) {
    CX_UNUSED(mask);

    if (!this || (this != cx_getOwner())) {
        cx_function f = cx_function(data->observer);
        cx_call(f, NULL, this, observable);
    }
}

static void cx_notifyObservers(cx__observer** observers, cx_object observable, cx_object source, cx_uint32 mask, int depth) {
    cx__observer* data;
    cx_uint32 i = 0;

    if (!observers) {
        return;
    }

    while((data = *observers)) {
        i++;
        cx_notifyObserver(data, observable, source, mask, depth);
        observers++;
    }
}

cx_object cx_setOwner(cx_object source) {
    cx_object result = NULL;
    cx_observerAdmin *admin = cx_observerAdminGet();
    result = admin->from;
    admin->from = source;
    return result;
}

cx_object cx_getOwner() {
    cx_object result = NULL;
    cx_observerAdmin *admin = cx_observerAdminGet();
    result = admin->from;
    return result;
}

static cx_int32 cx_notify(cx__observable* _o, cx_object observable, cx_uint32 mask) {
    cx_object *parent;
    cx_object this = NULL;
    cx__observer **observers;
    int depth = 0;

    /* Notify direct observers */
    if (_o) {

        /* Notify observers of observable */
        observers = cx_observersPush(&_o->onSelfArray, &this);
        cx_notifyObservers(observers, observable, this, mask, 0);
        if (cx_observersPop()) {
            cx_observersArrayFree(observers);
        }

        /* Bubble event up in hierarchy */
        if (cx_checkAttr(observable, CX_ATTR_SCOPED)) {
            parent = observable;
            while((parent = cx_parentof(parent))) {
                cx__observable *_parent = cx__objectObservable(CX_OFFSET(parent, -sizeof(cx__object)));

                /* Notify observers of parent */
                if (_parent) {
                    observers = cx_observersPush(&_parent->onChildArray, NULL);
                    cx_notifyObservers(observers, observable, this, mask, depth);
                    if (cx_observersPop()) {
                        cx_observersArrayFree(observers);
                    }
                }
                depth++;
            }
        }
    }

    return 0;
}

/* Update object */
cx_int32 cx_update(cx_object observable) {
    cx__observable *_o;
    cx__writable* _wr;
    cx__persistent* _ps;

    if (cx_typeof(observable)->kind != CX_VOID) {
        cx_seterr("use updateBegin/updateEnd for non-void objects");
        goto error;
    }

    if (cx_checkAttr(observable, CX_ATTR_PERSISTENT)) {
        cx_object owner = cx_ownerof(observable);
        if (owner && cx_instanceof(cx_replicator_o, owner)) {
            cx_seterr("cannot update '%s', process does not own object", cx_nameof(observable));
            goto error;
        }
    }

    _o = cx__objectObservable(CX_OFFSET(observable, -sizeof(cx__object)));
    if (_o->lockRequired) {
        _wr = cx__objectWritable(CX_OFFSET(observable, -sizeof(cx__object)));
        _ps = cx__objectPersistent(CX_OFFSET(observable, -sizeof(cx__object)));
        if (_ps) {
            cx_timeGet(&_ps->timestamp);
        }
        cx_rwmutexRead(&_wr->lock);
        if (cx_notify(_o, observable, CX_ON_UPDATE)) {
            goto error;
        }
        cx_rwmutexUnlock(&_wr->lock);
    } else {
        if (cx_notify(_o, observable, CX_ON_UPDATE)) {
            goto error;
        }
    }

    return 0;
error:
    return -1;
}

cx_int32 cx_updateBegin(cx_object observable) {
    cx__observable *_o;
    cx__writable* _wr;

    if (!cx_checkState(observable, CX_DEFINED)) {
        cx_seterr("cannot update undefined object");
        goto error;
    }

    if (cx_checkAttr(observable, CX_ATTR_PERSISTENT)) {
        cx_object owner = cx_ownerof(observable);
        if (owner && cx_instanceof(cx_replicator_o, owner)) {
            cx_seterr("cannot update '%s', process does not own object", cx_nameof(observable));
            goto error;
        }
    }

    _o = cx__objectObservable(CX_OFFSET(observable, -sizeof(cx__object)));

    if (_o->lockRequired) {
        _wr = cx__objectWritable(CX_OFFSET(observable, -sizeof(cx__object)));
        if (_wr) {
            if (cx_rwmutexWrite(&_wr->lock)) {
                cx_id id;
                cx_seterr("writelock on object '%s' failed", cx_fullname(observable, id));
                goto error;
            }
        } else {
            cx_id id;
            cx_warning("calling updateBegin for non-writable object '%s' is useless", cx_fullname(observable, id));
        }
    }

    return 0;
error:
    return -1;
}

cx_int32 cx_updateTry(cx_object observable) {
    cx__writable* _wr;

    _wr = cx__objectWritable(CX_OFFSET(observable, -sizeof(cx__object)));
    if (_wr) {
        if (cx_rwmutexTryWrite(&_wr->lock) == CX_LOCK_BUSY) {
            goto busy;
        }
    } else {
        cx_id id;
        cx_warning("calling updateTry for non-writable object '%s' is useless.", cx_fullname(observable, id));
    }

    return 0;
busy:
    return CX_LOCK_BUSY;
}

cx_int32 cx_updateEnd(cx_object observable) {
    cx__writable* _wr;
    cx__observable *_o;
    cx__persistent* _ps;

    _o = cx__objectObservable(CX_OFFSET(observable, -sizeof(cx__object)));
    _ps = cx__objectPersistent(CX_OFFSET(observable, -sizeof(cx__object)));
    if (_ps) {
        cx_timeGet(&_ps->timestamp);
    }

    if (cx_notify(_o, observable, CX_ON_UPDATE)) {
        goto error;
    }

    if (_o->lockRequired) {
        _wr = cx__objectWritable(CX_OFFSET(observable, -sizeof(cx__object)));
        if (_wr) {
            cx_rwmutexUnlock(&_wr->lock);
        }
    }

    return 0;
error:
    return -1;
}

cx_int32 cx_updateCancel(cx_object observable) {

    if (cx_checkAttr(observable, CX_ATTR_OBSERVABLE)) {
        cx__writable* _wr;

        _wr = cx__objectWritable(CX_OFFSET(observable, -sizeof(cx__object)));

        cx_rwmutexUnlock(&_wr->lock);
    } else {
        cx_id id;
        cx_error("object '%s' is not an observable", cx_fullname(observable, id));
        goto error;
    }

    return 0;
error:
    return -1;
}

static void cx_waitObserver(cx_object me, cx_object observable, cx_object source) {
    cx_waitForObject *waitAdmin;
    CX_UNUSED(source);

    waitAdmin = (cx_waitForObject*)me;

    if (cx_ainc(&waitAdmin->triggerCount) == 1) {
        cx_uint32 i;

        waitAdmin->triggered = observable;
        cx_semPost(waitAdmin->semaphore);

        /* Silence this observer */
        for(i=0; i<waitAdmin->count; i++) {
            cx_silence(me, waitAdmin->observer, CX_ON_UPDATE, waitAdmin->objects[i]);
        }
    }
    cx_adec(&waitAdmin->triggerCount);
}

static void __cx_waitObserver(cx_function f, void* result, void* args) {
    CX_UNUSED(f);
    CX_UNUSED(result);
    cx_waitObserver(
        *(cx_object*)args,
        *(cx_object*)((intptr_t)args + sizeof(cx_object)),
        *(cx_object*)((intptr_t)args + sizeof(cx_object) * 2));
}

cx_int32 cx_waitfor(cx_object observable) {
    cx_waitForObject *waitAdmin;

    /* Obtain waitadministration */
    waitAdmin = cx_threadTlsGet(CX_KEY_WAIT_ADMIN);
    if (!waitAdmin) {
        cx_observer observer;

        /* Create thread-specific waitadministration */
        waitAdmin = cx_alloc(sizeof(cx_waitForObject));
        memset(waitAdmin, 0, sizeof(cx_waitForObject));
        waitAdmin->semaphore = cx_semNew(0);

        /* Create observer */
        observer = cx_declare(cx_type(cx_observer_o));
        cx_function(observer)->impl = (cx_word)__cx_waitObserver;
        cx_function(observer)->implData = (cx_word)cx_waitObserver;
        cx_function(observer)->kind = CX_PROCEDURE_CDECL;
        observer->mask = CX_ON_UPDATE;

        cx_define(observer);
        waitAdmin->observer = observer;
        cx_threadTlsSet(CX_KEY_WAIT_ADMIN, waitAdmin);
    }

    /* Add object to waitadministration */
    waitAdmin->objects[waitAdmin->count] = observable;
    waitAdmin->count++;

    return 0;
}

cx_object cx_wait(cx_int32 timeout_sec, cx_int32 timeout_nanosec) {
    cx_waitForObject *waitAdmin;
    cx_object result = NULL;
    CX_UNUSED(timeout_sec);
    CX_UNUSED(timeout_nanosec);

    /* Obtain waitadministration */
    waitAdmin = cx_threadTlsGet(CX_KEY_WAIT_ADMIN);
    if (waitAdmin) {
        cx_uint32 i;

        /* Setup observer for observables */
        for(i=0; i<waitAdmin->count; i++) {
            cx_listen((cx_object)waitAdmin, waitAdmin->observer, CX_ON_UPDATE, waitAdmin->objects[i], NULL);
        }

        /* Do the wait */
        cx_semWait(waitAdmin->semaphore);

        result = waitAdmin->triggered;
    } else {
        cx_error("wait called without objects that are being waited for");
    }

    return result;
}

/* REPL functionality */
cx_int16 cx_expr(cx_object scope, cx_string expr, cx_value *value) {
    cx_int16 result = 0;
    static cx_function parseLine = NULL;
    static cx_bool searchedForParser = FALSE;

    if (!parseLine && !searchedForParser) {
        parseLine = cx_resolve(NULL, "::corto::ast::Parser::parseLine");
        searchedForParser = TRUE;
    }

    /* Load parser */
    if (parseLine) {
        /* Parse expression */
        cx_call(parseLine, &result, expr, scope, value);
    /* Parser cannot be loaded, revert to plain object resolving */
    } else {
        cx_object o = cx_resolve(scope, expr);
        if (!o) {
            cx_error("'%s' does not resolve to a valid object", expr);
            goto error;
        }

        cx_valueObjectInit(value, o, NULL);
    }

    return result;
error:
    return -1;
}

/* Thread-safe reading */
cx_int32 cx_readBegin(cx_object object) {
    if (cx_checkAttr(object, CX_ATTR_WRITABLE)) {
        cx__writable* _o;

        _o = cx__objectWritable(CX_OFFSET(object, -sizeof(cx__object)));
        if (cx_rwmutexRead(&_o->lock)) {
            goto error;
        }
    }

    return 0;
error:
    return -1;
}

cx_int32 cx_readEnd(cx_object object) {
    return cx_unlock(object);
}

/* Thread-safe writing */
cx_int32 cx_lock(cx_object object) {
    if (cx_checkAttr(object, CX_ATTR_WRITABLE)) {
        cx__writable* _o;

        _o = cx__objectWritable(CX_OFFSET(object, -sizeof(cx__object)));
        if (cx_rwmutexWrite(&_o->lock)) {
            goto error;
        }
    }

    return 0;
error:
    return -1;
}

cx_int32 cx_unlock(cx_object object) {
    if (cx_checkAttr(object, CX_ATTR_WRITABLE)) {
        cx__writable* _o;

        _o = cx__objectWritable(CX_OFFSET(object, -sizeof(cx__object)));
        if (cx_rwmutexUnlock(&_o->lock)) {
            goto error;
        }
    }

    return 0;
error:
    return -1;
}


/* Obtain function name from signature */
cx_int32 cx_signatureName(cx_string signature, cx_id buffer) {
    cx_char ch, *srcptr, *bptr;

    /* Obtain name from function (without argumentlist) */
    srcptr = signature; bptr = buffer;
    while ((ch = *srcptr) && (ch != '(')) {
        *bptr = ch;
        bptr++;
        srcptr++;
    }
    *bptr = '\0';

    return 0;
}

/* Count number of parameters */
cx_int32 cx_signatureParamCount(cx_string signature) {
    cx_char *ptr, ch;
    cx_uint32 count;

    count = 0;

    /* Look for start of argumentlist */
    ptr = strchr(signature, '(');
    if (!ptr) {
        goto error;
    }

    /* Check if there are zero arguments */
    ptr++;
    if (*ptr != ')') {
        count = 1;
        /* Count arguments */
        while((ch = *ptr)) {
            if (ch == ',') {
                count++;
            }
            ptr++;

            if (ch == '{') {
                cx_uint32 nesting = 1;
                while((ch = *ptr) && nesting) {
                   ptr++;
                   switch(ch) {
                   case '{':
                       nesting++;
                       break;
                   case '}':
                       nesting--;
                       break;
                   }
               }
            }
        }
    }

    return count;
error:
    return -1;
}

/* Obtain function parameter types from signature */
cx_int32 cx_signatureParamType(cx_string signature, cx_uint32 id, cx_id buffer, int* flags) {
    cx_char ch, *srcptr, *bptr;
    cx_uint32 i;
    cx_bool parsed, parsing;

    if (flags) {
        *flags = 0;
    }

    srcptr = strchr(signature, '(');
    if (!srcptr) {
        cx_error("missing argmentlist in signature '%s'", signature);
        goto error;
    }
    srcptr++;

    /* Find parameter-entry, copy to buffer */
    bptr = buffer;
    i = 0;
    parsed = FALSE;
    parsing = FALSE;
    while((ch = *srcptr) && !parsed) {

        /* Start parsing when argument is reached. */
        if (i == id) {
            if ((ch == ',') || (ch == ' ') || (ch == ')') || (ch == '&')) {
                if (parsing) {
                    parsed = TRUE;
                }
            } else {
                parsing = TRUE;
                *bptr = ch;
                bptr++;
            }
        }

        if (!parsed) {
            if (ch == ',') {
                i++;
            }

            srcptr++;
            if (ch == '{') {
                cx_uint32 count=1;
                while((ch = *srcptr) && count) {
                    if (i == id) {
                        *bptr = ch;
                        bptr++;
                    }
                    srcptr++;
                    switch(ch) {
                    case '{':
                        count++;
                        break;
                    case '}':
                        count--;
                        break;
                    }
                }
            }
        }
    }
    *bptr = '\0';

    if (flags && (ch == '&')) {
        *flags |= CX_PARAMETER_REFERENCE;
        if(*(srcptr+1) == '&') {
            *flags |= CX_PARAMETER_FORCEREFERENCE;
        }
    }
    if (flags && !strcmp(buffer, "?")) {
        *flags |= CX_PARAMETER_WILDCARD;
    }
    if (flags && !strcmp(buffer, "null")) {
        *flags |= CX_PARAMETER_NULL;
    }

    return 0;
error:
    return -1;
}

/* Obtain function parameter names from signature */
cx_int32 cx_signatureParamName(cx_string signature, cx_uint32 id, cx_id buffer) {
    cx_char ch, *srcptr, *bptr;
    cx_uint32 i;
    cx_bool parsed;

    srcptr = strchr(signature, '(');
    if (!srcptr) {
        cx_error("missing argmentlist in signature '%s'", signature);
        goto error;
    }
    srcptr++;

    /* Find parameter-entry, copy name to buffer */
    bptr = buffer;
    i = 0;
    parsed = FALSE;
    while((ch = *srcptr) && !parsed) {
        if (i == id) {
            switch(ch) {
            case '&':
                srcptr++;
                if(*srcptr == '&') { /* Forcereference */
                    srcptr++;
                }

                /* no break */
            case ' ':
                srcptr++;
                /* Location of parameter-name found, copy to buffer */
                while((ch = *srcptr) && (ch != ',') && (ch != ')')) {
                    *bptr = ch;
                    bptr++;
                    srcptr++;
                }
                parsed = TRUE;
                break;
            case ')':
            case ',':
                cx_error("missing name of parameter %d in signature '%s'", id, buffer);
                break;
            default:
                break;
            }
        }

        if (!parsed) {
            if (ch == ',') {
                i++;
            }

            srcptr++;
            if (ch == '{') {
                cx_uint32 count=1;
                while((ch = *srcptr) && count) {
                    srcptr++;
                    switch(ch) {
                    case '{':
                        count++;
                        break;
                    case '}':
                        count--;
                        break;
                    }
                }
            }
        }
    }
    *bptr = '\0';

    return 0;
error:
    return -1;
}

/* Helper functions for overloading */
cx_uint32 cx_overloadParamCount(cx_object o) {
    cx_uint32 result;
    if (cx_interface(cx_typeof(o))->kind == CX_PROCEDURE) {
        if (cx_procedure(cx_typeof(o))->kind != CX_OBSERVER) {
            result = cx_function(o)->parameters.length;
        } else {
            result = 0;
        }
    } else {
        result = cx_delegate(cx_typeof(o))->parameters.length;
    }
    return result;
}

/* Helper function that obtains type from signature */
cx_type cx_overloadParamType(cx_object object, cx_int32 i, cx_bool *reference) {
    cx_id buffer;
    cx_int32 flags = 0;
    cx_id signature;

    if (cx_signature(object, signature)) {
        goto error;
    }

    if (cx_signatureParamType(signature, i, buffer, &flags)) {
        goto error;
    }

    if (flags & CX_PARAMETER_REFERENCE) {
        if (reference) *reference = TRUE;
    } else {
        if (reference) *reference = FALSE;
    }

    return cx_type(cx_resolve(object, buffer));
error:
    cx_error("failed to obtain parameter %d from signature %s", i, signature);
    return NULL;
}


/* Compare parameter */
static cx_uint32 cx_overloadParamCompare(
    cx_type o_type,
    cx_type r_type,
    cx_bool o_reference,
    cx_bool r_reference,
    cx_bool r_forceReference,
    cx_bool r_wildcard,
    cx_bool r_null) {
    cx_int32 d = 0;

    /* Match wildcards */
    if (r_wildcard) {
        goto match;
    }

    /* Match reference modifiers */
    if (o_reference) {
        if (!r_reference && !r_null) {
            goto nomatch; /* Parameter accepts only references */
        } else {
            if (!r_forceReference) {
                if (!r_null) {
                    d++; /* Favor pass by value in case of implicit reference passing */
                }
            }
        }
    } else if (r_reference) {
        d++;
    }

    /* Match any */
    if ((o_type->kind == CX_ANY) || (r_type && (r_type->kind == CX_ANY))) {
        goto match;
    }

    /* Match null */
    if (r_null) {
        if ((o_type->kind == CX_PRIMITIVE) && (cx_primitive(o_type)->kind == CX_BOOLEAN)) {
            d++;
            goto match;
        } else if (!o_reference &&
            ((o_type->kind != CX_PRIMITIVE) || (cx_primitive(o_type)->kind != CX_TEXT))) {
            goto nomatch;
        } else {
            goto match;
        }
    }

    /* Match type compatibility */
    if (cx_checkState(o_type, CX_DEFINED) && (cx_checkState(r_type, CX_DEFINED))) {
        if (o_type == r_type) {
            goto match;
        }

        /* If an interface, increase distance for each level in inheritance tree  */
        if ((o_type->kind == CX_COMPOSITE) && (r_type->kind == CX_COMPOSITE)) {
            cx_interface base = cx_interface(r_type);
            while(base && (base != cx_interface(o_type))) {
                d++;
                base = base->base;
            }
            if (!base) {
                goto nomatch; /* o_type doesn't occur in the inheritance tree of r_type */
            }

        /* If a generic reference, increase distance with the max levels in r's inheritance tree. This
         * way, a more specific signature will always be favored */
        } else if (o_reference && o_type->kind == CX_VOID) {
            if (r_type->kind == CX_COMPOSITE) {
                cx_interface base = cx_interface(r_type);
                do { /* Always increase at least by one */
                    d++;
                } while((base = base->base));
            } else {
                d++;
            }
        /* If the requested type is a (forced) reference check if treating it as a generic
         * reference would result in a match - this is for example useful when casting from
         * references to a boolean or string type */
        } else if (r_forceReference && !cx_type_compatible(o_type, cx_object_o)) {
            d++;
        /* If types are not compatible, they won't match */
        } else if (!cx_type_compatible(o_type, r_type)) {
            goto nomatch;
        /* Types are compatible. Increase d by one if types are of a different primitive
         * kind. */
        } else if ((o_type->kind == CX_PRIMITIVE) && (r_type->kind == CX_PRIMITIVE)) {
            if (cx_primitive(o_type)->kind != cx_primitive(r_type)->kind) {
                if (!cx_primitive_isInteger(cx_primitive(o_type)) || 
                    !cx_primitive_isInteger(cx_primitive(r_type))) {
                    d++;
                }
            }
        }
    } else if (o_type != r_type) {
        goto nomatch;
    }

match:
    return d;
nomatch:
    return -1;
}

/* Create signature from delegate */
static void cx_signatureFromDelegate(cx_object o, cx_id buffer) {
    cx_delegate type = cx_delegate(cx_typeof(o));
    cx_uint32 i;

    /* Construct signature */
    cx_string signature = cx_signatureOpen(cx_nameof(o));
    for (i = 0; i < type->parameters.length; i++) {
        cx_parameter *p = &type->parameters.buffer[i];
        signature = cx_signatureAdd(signature, p->type, p->passByReference ? CX_PARAMETER_FORCEREFERENCE : 0);
    }
    signature = cx_signatureClose(signature);

    /* Copy signature to buffer */
    strcpy(buffer, signature);
    cx_dealloc(signature);
}

/* Obtain signature from object */
cx_int16 cx_signature(cx_object object, cx_id buffer) {
    cx_type t = cx_typeof(object);

    if (t->kind != CX_COMPOSITE) {
        goto error;
    }

    switch(cx_interface(t)->kind) {
    case CX_DELEGATE:
        cx_signatureFromDelegate(object, buffer);
        break;
    case CX_PROCEDURE:
        strcpy(buffer, cx_nameof(object));
        break;
    default:
        goto error;
    }

    return 0;
error:
    cx_error("cannot obtain signature from a non callable object");
    abort();
    return -1;
}

/* Check if argumentlist-expr matches function.
 *   The offered string (the name of the object) looks like:
 *      name(type1 arg1,type2 arg2,...)
 *   The requested string looks like:
 *      name(type1,type2,...)
 *
 *   No extra whitespaces are allowed in both. Type-names are relative
 *   to the scope of the function-object.
 */
cx_int16 cx_overload(cx_object object, cx_string requested, cx_int32* distance) {
    cx_id r_name, o_name;
    cx_int32 r_parameterCount, o_parameterCount;
    cx_int32 i = 0, d = 0;
    cx_id offered;

    /* Validate if function object is valid */
    if (!cx_checkState(object, CX_VALID)) {
        goto error;
    }

    /* Obtain offered singature */
    if (cx_signature(object, offered)) {
        goto error;
    }

    /* Obtain name of offered object */
    if (cx_signatureName(offered, o_name)) {
        goto error;
    }

    /* Obtain name of requested object */
    if (cx_signatureName(requested, r_name)) {
        goto error;
    }

    /* Validate if names of request and offered match */
    if (stricmp(o_name, r_name)) {
        goto nomatch;
    }

    /* Validate whether number of parameters is equal. A request is allowed to
     * omit parameters */
    r_parameterCount = cx_signatureParamCount(requested);
    o_parameterCount = cx_overloadParamCount(object);

    if ((r_parameterCount != -1) && (r_parameterCount != o_parameterCount)) {
        goto nomatch;
    }

    /* If request contains parameters, compare parameters of both */
    if (r_parameterCount == o_parameterCount) {
        for (i = 0; i < o_parameterCount; i++) {
            cx_bool o_reference = FALSE, r_reference = FALSE;
            cx_bool r_forceReference = FALSE, r_wildcard = FALSE, r_null = FALSE;
            cx_type o_type, r_type = NULL;
            cx_id r_typeName;
            int flags, paramDistance = 0;

            /* Obtain offered and requested type */
            if (!(o_type = cx_overloadParamType(object, i, &o_reference))) {
                goto error;
            }
            if (cx_signatureParamType(requested, i, r_typeName, &flags)) {
                goto error;
            } else if (!(flags & (CX_PARAMETER_WILDCARD | CX_PARAMETER_NULL))) {
                r_type = cx_resolve(object, r_typeName);
                if (r_type) {
                    r_type = cx_type(r_type);
                } else {
                    cx_seterr("unresolved type '%s' in signature '%s'", r_typeName, requested);
                    goto error;
                }
            }

            /* Obtain flags */
            o_reference |= o_type->reference;
            r_reference |= (flags & CX_PARAMETER_REFERENCE) | (r_type ? r_type->reference : 0);
            r_forceReference |= (flags & CX_PARAMETER_FORCEREFERENCE) | (r_type ? r_type->reference : 0);
            r_wildcard = flags & CX_PARAMETER_WILDCARD;
            r_null = flags & CX_PARAMETER_NULL;

            /* Evaluate whether parameter types are compatible */
            paramDistance += cx_overloadParamCompare(
                o_type,
                r_type,
                o_reference,
                r_reference,
                r_forceReference,
                r_wildcard,
                r_null);

            if (paramDistance == -1) {
                goto nomatch;
            } else {
                d += paramDistance;
            }
        }
    }

    *distance = d;

    return 0;
nomatch:
    *distance = -1;
    return 0;
error:
    if (!cx_lasterr()) {
        cx_seterr("invalid query '%s'", requested);
    }
    return -1;
}

static int cx_scopeCollectWalk(cx_object o, void* userData) {
    cx_objectseq *seq = userData;
    if (!seq->buffer) {
        /* Get scopesize within scope lock */
        cx_uint32 scopeSize = cx_scopeSize(cx_parentof(o));
        seq->buffer = cx_alloc(sizeof(cx_object) * scopeSize);
        /* Increment length with each object */
    }
    cx_claim(o);
    seq->buffer[seq->length++] = o;
    return 1;
}

cx_objectseq cx_scopeClaim(cx_object scope) {
    cx_objectseq result = {0, NULL};
    cx_scopeWalk(scope, cx_scopeCollectWalk, &result);
    return result;
}

void cx_scopeRelease(cx_objectseq seq) {
    cx_uint32 i = 0;
    for (i = 0; i < seq.length; i++) {
        cx_release(seq.buffer[i]);
    }
    cx_dealloc(seq.buffer);
}

typedef struct cx_lookupFunction_t {
    cx_string request;
    cx_function *result;
    cx_bool error;
    cx_int32 d;
    cx_int32 old_d;
}cx_lookupFunction_t;

/* Lookup function in scope */
int cx_lookupFunctionWalk(cx_object *ptr, void* userData) {
    cx_int32 d = -1;
    cx_lookupFunction_t* data;
    cx_object o = *ptr;

    data = userData;

    /* If current object is a function, match it */
    if ((cx_typeof(o)->kind == CX_COMPOSITE) &&
        ((cx_interface(cx_typeof(o))->kind == CX_PROCEDURE) ||
        (cx_interface(cx_typeof(o))->kind == CX_DELEGATE))) {
        if (strchr(data->request, '(')) {
            if (cx_overload(o, data->request, &d)) {
                data->error = TRUE;
                goto found;
            }
        } else {
            cx_id sigName; cx_signatureName(cx_nameof(o), sigName);
            if (!strcmp(sigName, data->request)) {
                if (!cx_function(o)->overloaded) {
                    data->d = 0;
                    data->result = (cx_function*)ptr;
                    goto found;
                } else {
                    data->error = TRUE;
                    cx_seterr("ambiguous reference '%s'", data->request);
                    goto found;
                }
            }
        }

        if (d != -1) {
            if (d <= data->d) {
                data->old_d = data->d;
            }
            if (d < data->d) {
                data->result = (cx_function*)ptr;
                data->d = d;
            }
        }
    }

    return 1;
found:
    return 0;
}

/* Lookup function with support for overloading */
cx_function* cx_lookupFunctionFromSequence(cx_objectseq scopeContents, cx_string requested, cx_int32* d) {
    cx_lookupFunction_t walkData;
    cx_uint32 i;

    /* Call the actual lookup function */
    walkData.request = requested;
    walkData.result = NULL;
    walkData.error = FALSE;
    walkData.d = INT_MAX;
    walkData.old_d = INT_MAX;

    for (i = 0; i < scopeContents.length; i++) {
        if (!cx_lookupFunctionWalk(&scopeContents.buffer[i], &walkData)) {
            break;
        }
    }

    if (walkData.d != INT_MAX && (walkData.d == walkData.old_d)) {
        cx_seterr("ambiguous reference '%s'", walkData.request);
        walkData.error = TRUE;
    }

    if (d) {
        if (walkData.error) {
            *d = -1;
        } else if (!walkData.result) {
            *d = 0;
        } else {
            *d = walkData.d;
        }
    }

    if (walkData.error) walkData.result = NULL;

    return walkData.result;
}

cx_function cx_lookupFunction(cx_object scope, cx_string requested, cx_int32* d) {
    cx_objectseq scopeContents = cx_scopeClaim(scope);
    cx_function result = NULL;
    cx_function *ptr = cx_lookupFunctionFromSequence(scopeContents, requested, d);
    if (ptr) {
        cx_claim(*ptr);
        result = *ptr;
    }
    cx_scopeRelease(scopeContents);
    return result;
}

/* Create request signature */
cx_string cx_signatureOpen(cx_string name) {
    cx_string result;

    result = cx_alloc(strlen(name) + 1 + 1);
    sprintf(result, "%s(", name);

    return result;
}

cx_string cx_signatureAdd(cx_string sig, cx_type type, int flags) {
    cx_uint32 len;
    cx_string result;
    cx_id id;
    cx_bool reference = flags & CX_PARAMETER_REFERENCE;
    cx_bool forceReference = flags & CX_PARAMETER_FORCEREFERENCE;
    cx_bool wildcard = flags & CX_PARAMETER_WILDCARD;

    if (type) {
        if (!cx_checkAttr(type, CX_ATTR_SCOPED) ||
           ((cx_parentof(type) != corto_o) &&
           (cx_parentof(type) != corto_lang_o))) {
            cx_fullname(type, id);
        } else {
            strcpy(id, cx_nameof(type));
        }
    } else if (wildcard) {
        strcpy(id, "?");
    } else {
        strcpy(id, "null");
    }

    len = strlen(sig);
    if (sig[len-1] == '(') {
        result = cx_realloc(sig, len + strlen(id) + 1 + ((reference|forceReference) ? 2 : 0));
        strcat(result, id);
        if (flags & (reference|forceReference)) strcat(result, "&");
        if (flags & (forceReference)) strcat(result, "&");
    } else {
        result = cx_realloc(sig, len + strlen(id) + 1 + 1 + ((reference|forceReference) ? 2 : 0));
        strcat(result, ",");
        strcat(result, id);
        if (flags & (reference|forceReference)) strcat(result, "&");
        if (flags & (forceReference)) strcat(result, "&");
    }

    return result;
}

cx_string cx_signatureAddWildcard(cx_string sig, cx_bool isReference) {
    cx_uint32 len;
    cx_string result;

    len = strlen(sig);
    if (sig[len-1] == '(') {
        result = cx_realloc(sig, len + 1 + 1 + (isReference ? 1 : 0));
        strcat(result, "?");
        if (isReference) strcat(result, "&");
    } else {
        result = cx_realloc(sig, len + 1 + 1 + 1 + (isReference ? 1 : 0));
        strcat(result, ",");
        strcat(result, "?");
        if (isReference) strcat(result, "&");
    }

    return result;
}

cx_string cx_signatureClose(cx_string sig) {
    cx_uint32 length = strlen(sig) + 1;
    sig = cx_realloc(sig, length + 1);
    sig[length-1] = ')';
    sig[length] = '\0';
    return sig;
}

/* Set reference field */
void cx_setref(void* ptr, cx_object value) {
    cx_object old;
    old = *(cx_object*)ptr;
    if (value) {
        cx_claim(value);
    }
    *(cx_object*)ptr = value;
    if (old) {
        cx_release(old);
    }
}

/* Set string field */
void cx_setstr(cx_string* ptr, cx_string value) {
    if (*ptr) {
        cx_dealloc(*ptr);
    }
    *ptr = value ? cx_strdup(value) : NULL;
}

cx_string cx_str(cx_object object, cx_uint32 maxLength) {
    cx_string_ser_t serData;
    struct cx_serializer_s s;
    serData.buffer = NULL;
    serData.length = 0;
    serData.maxlength = maxLength;
    serData.compactNotation = TRUE;
    serData.prefixType = FALSE;
    serData.enableColors = FALSE;

    s = cx_string_ser(CX_LOCAL, CX_NOT, CX_SERIALIZER_TRACE_NEVER);
    cx_serialize(&s, object, &serData);

    return serData.buffer;
}

cx_string cx_strv(cx_value* v, cx_uint32 maxLength) {
    cx_string_ser_t serData;
    struct cx_serializer_s s;
    serData.buffer = NULL;
    serData.length = 0;
    serData.maxlength = maxLength;
    serData.compactNotation = TRUE;
    serData.prefixType = FALSE;
    serData.enableColors = FALSE;

    s = cx_string_ser(CX_LOCAL, CX_NOT, CX_SERIALIZER_TRACE_NEVER);
    cx_serializeValue(&s, v, &serData);

    return serData.buffer;
}

cx_string _cx_strp(void *p, cx_type type, cx_uint32 maxLength) {
    cx_value v;
    cx_valueValueInit(&v, NULL, type, p);
    return cx_strv(&v, maxLength);
}

cx_string cx_stra(cx_any a, cx_uint32 maxLength) {
    cx_value v;
    cx_valueValueInit(&v, NULL, a.type, a.value);
    return cx_strv(&v, maxLength);
}

cx_int16 cx_fromStr(void *o, cx_string string) {
    cx_string_deser_t serData;

    serData.out = *(void**)o;
    serData.scope = NULL;
    serData.type = *(void**)o ? cx_typeof(*(void**)o) : NULL;
    if (!cx_string_deser(string, &serData)) {
        cx_assert(!serData.out, "deserializer failed but out is set");
    }

    if (serData.out) {
        *(void**)o = serData.out;
    } else {
        goto error;
    }

    return 0;
error:
    return -1;
}

cx_int16 cx_fromStrv(cx_value *v, cx_string string) {
    cx_string_deser_t serData;

    serData.out = cx_valueValue(v);
    serData.scope = NULL;
    serData.type = cx_valueType(v);
    if (!cx_string_deser(string, &serData)) {
        cx_assert(!serData.out, "deserializer failed but out is set");
    }

    if (serData.out) {
        cx_valueSetValue(v, serData.out);
    } else {
        goto error;
    }
    
    return 0;
error:
    return -1;
}

cx_int16 _cx_fromStrp(void* out, cx_type type, cx_string string) {
    cx_string_deser_t serData;

    serData.out = *(void**)out;
    serData.scope = NULL;
    serData.type = type;
    if (!cx_string_deser(string, &serData)) {
        cx_assert(!serData.out, "deserializer failed but out is set");
    }

    if (serData.out) {
        *(void**)out = serData.out;
    } else {
        goto error;
    }

    return 0;
error:
    return -1;
}

cx_int16 cx_fromStra(cx_any *a, cx_string string) {
    cx_string_deser_t serData;

    serData.out = a->value;
    serData.scope = NULL;
    serData.type = a->type;
    if (!cx_string_deser(string, &serData)) {
        cx_assert(!serData.out, "deserializer failed but out is set");
    }

    if (serData.out) {
        a->value = serData.out;
    } else {
        goto error;
    }

    return 0;
error:
    return -1;
}

cx_equalityKind cx_compare(cx_object o1, cx_object o2) {
    cx_any a1, a2;
    a1.value = o1;
    a1.type = cx_typeof(o1);
    a1.owner = FALSE;
    a2.value = o2;
    a2.type = cx_typeof(o2);
    a2.owner = FALSE;
    return cx_type_compare(a1, a2);
}

cx_equalityKind cx_comparev(cx_value *value1, cx_value *value2) {
    cx_void *v1, *v2;
    cx_any a1, a2;
    cx_type t1, t2;

    v1 = cx_valueValue(value1);
    v2 = cx_valueValue(value2);
    t1 = cx_valueType(value1);
    t2 = cx_valueType(value2);

    a1.value = v1;
    a1.type = t1;
    a1.owner = FALSE;
    a2.value = v2;
    a2.type = t2;
    a2.owner = FALSE;
    return cx_type_compare(a1, a2);
}

cx_equalityKind _cx_comparep(void *p1, cx_type type, void *p2) {
    cx_value vdst;
    cx_value vsrc;
    cx_valueValueInit(&vdst, NULL, type, p1);
    cx_valueValueInit(&vsrc, NULL, type, p2);
    return cx_comparev(&vdst, &vsrc);
}

cx_equalityKind cx_comparea(cx_any a1, cx_any a2) {
    cx_value vdst;
    cx_value vsrc;
    cx_valueValueInit(&vdst, NULL, a1.type, a1.value);
    cx_valueValueInit(&vsrc, NULL, a2.type, a2.value);
    return cx_comparev(&vdst, &vsrc);
}

cx_int16 cx_init(cx_object o) {
    cx_typeKind kind = cx_typeof(o)->kind;
    switch(kind) {
        case CX_COMPOSITE:
        case CX_COLLECTION: {
            struct cx_serializer_s s = cx_ser_init(0, CX_NOT, CX_SERIALIZER_TRACE_ON_FAIL);
            cx_serialize(&s, o, NULL);
            break;
        }
        default:
            break;
    }
    return cx_delegateInit(cx_typeof(o), o);;
}

cx_int16 cx_initv(cx_value *v) {
    struct cx_serializer_s s = cx_ser_init(0, CX_NOT, CX_SERIALIZER_TRACE_ON_FAIL);
    if (cx_serializeValue(&s, v, NULL)) {
        return -1;
    }
    return cx_delegateInit(cx_valueType(v), cx_valueValue(v));
}

cx_int16 _cx_initp(void *p, cx_type type) {
    cx_value v;
    cx_valueValueInit(&v, NULL, type, p);
    return cx_initv(&v);
}

cx_int16 cx_inita(cx_any a) {
    cx_value v;
    cx_valueValueInit(&v, NULL, a.type, a.value);
    return cx_initv(&v);
}

cx_int16 cx_deinit(cx_object o) {
    cx_typeKind kind = cx_typeof(o)->kind;
    switch(kind) {
        case CX_COMPOSITE:
        case CX_COLLECTION: {
            struct cx_serializer_s s = cx_ser_freeResources(0, CX_NOT, CX_SERIALIZER_TRACE_ON_FAIL);
            cx_serialize(&s, o, NULL);
            break;
        }
        default:
            break;
    }
    return 0;
}

cx_int16 cx_deinitv(cx_value *v) {
    struct cx_serializer_s s = cx_ser_freeResources(0, CX_NOT, CX_SERIALIZER_TRACE_ON_FAIL);
    return cx_serializeValue(&s, v, NULL);
}

cx_int16 _cx_deinitp(void *p, cx_type type) {
    cx_value v;
    cx_valueValueInit(&v, NULL, type, p);
    return cx_deinitv(&v);
}

cx_int16 cx_deinita(cx_any a) {
    cx_value v;
    cx_valueValueInit(&v, NULL, a.type, a.value);
    return cx_deinitv(&v);
}

cx_int16 cx_copy(cx_object *dst, cx_object src) {
    struct cx_serializer_s s = cx_copy_ser(CX_PRIVATE, CX_NOT, CX_SERIALIZER_TRACE_ON_FAIL);
    cx_copy_ser_t data;
    cx_int16 result;
    cx_bool newObject = FALSE;

    if (!*dst) {
        *dst = cx_declare(cx_typeof(src));
        newObject = TRUE;
    }

    cx_valueObjectInit(&data.value, *dst, NULL);
    result = cx_serialize(&s, src, &data);

    if (newObject) {
        cx_define(*dst);
    }

    return result;
}

cx_int16 cx_copyv(cx_value *dst, cx_value *src) {
    struct cx_serializer_s s = cx_copy_ser(CX_PRIVATE, CX_NOT, CX_SERIALIZER_TRACE_ON_FAIL);
    cx_copy_ser_t data;
    cx_int16 result;
    cx_bool newObject = FALSE;

    if (!cx_valueValue(dst)) {
        cx_valueValueInit(dst, NULL, cx_valueType(src), cx_declare(cx_valueType(src)));
        newObject = TRUE;
    }

    data.value = *dst;
    result = cx_serializeValue(&s, src, &data);

    if (newObject) {
        cx_define(cx_valueValue(dst));
    }

    return result;
}

cx_int16 _cx_copyp(void *dst, cx_type type, void *src) {
    cx_value vdst;
    cx_value vsrc;
    cx_int16 result;
    cx_valueValueInit(&vdst, NULL, type, dst);
    cx_valueValueInit(&vsrc, NULL, type, src);
    result = cx_copyv(&vdst, &vsrc);
    return result;
}

cx_int16 cx_copya(cx_any *dst, cx_any src) {
    cx_value vdst;
    cx_value vsrc;
    cx_int16 result;
    cx_valueValueInit(&vdst, NULL, src.type, dst->value);
    cx_valueValueInit(&vsrc, NULL, src.type, src.value);
    result = cx_copyv(&vdst, &vsrc);
    dst->value = cx_valueValue(&vdst);
    dst->type = src.type;
    return result;
}

