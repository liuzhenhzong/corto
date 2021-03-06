/* $CORTO_GENERATED
 *
 * list.c
 *
 * Only code written between the begin and end tags will be preserved
 * when the file is regenerated.
 */

#include <corto/lang/lang.h>

corto_int16 _corto_list_construct(
    corto_list this)
{
/* $begin(corto/lang/list/construct) */
    corto_type(this)->hasResources = TRUE;
    corto_type(this)->size = sizeof(corto_ll);
    corto_type(this)->alignment = CORTO_ALIGNMENT(corto_ll);
    if (!corto_collection(this)->elementType) {
        corto_error("no elementtype provided for list");
        goto error;
    }
    return corto_type_construct(corto_type(this));
error:
    return -1;
/* $end */
}

corto_int16 _corto_list_init(
    corto_list this)
{
/* $begin(corto/lang/list/init) */
    corto_collection(this)->kind = CORTO_LIST;
    return corto_collection_init(corto_collection(this));
/* $end */
}
