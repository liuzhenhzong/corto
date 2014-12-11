/* db_list.h
 *
 * This file contains generated code. Do not modify!
 */

#ifndef db_list_H
#define db_list_H

#include "hyve.h"
#include "db_collection.h"
#include "db__type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ::hyve::lang::list::append() */
db_any db_list_append_(db_any _this);

/* ::hyve::lang::list::append(lang::any element) */
db_void db_list_append_lang_any(db_any _this, db_any element);

/* ::hyve::lang::list::clear() */
db_void db_list_clear(db_any _this);

/* callback ::hyve::lang::class::construct(lang::object object) -> ::hyve::lang::list::construct(lang::list object) */
db_int16 db_list_construct(db_list object);

/* callback ::hyve::lang::type::init(lang::object object) -> ::hyve::lang::list::init(lang::list object) */
db_int16 db_list_init(db_list object);

/* ::hyve::lang::list::insert() */
db_any db_list_insert_(db_any _this);

/* ::hyve::lang::list::insert(lang::any element) */
db_void db_list_insert_lang_any(db_any _this, db_any element);

/* ::hyve::lang::list::reverse() */
db_void db_list_reverse(db_any _this);

#ifdef __cplusplus
}
#endif
#endif
