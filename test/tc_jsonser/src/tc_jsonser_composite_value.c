#include <string.h>
#include <stdio.h>

#include "cortex.h"
#include "json.h"
#include "tc_jsonser_fixture__api.h"
#include "tc_jsonser_fixture__meta.h"
#include "tc_jsonser_fixture__type.h"


#define _test_ser_composite_value(object, expected) \
{\
    cx_json_ser_t userData = {NULL, NULL, 0, 0, 0, FALSE, TRUE, FALSE};\
    cx_serialize(&serializer, tc_jsonser_fixture_##object##_o, &userData);\
    if (strcmp(userData.buffer, "{\"value\":"expected"}")) {\
        result = -1;\
        fprintf(\
            stderr,\
            "tc_jsonser_composite_value: FAIL:\nexpected:\n%s\nserialized:\n%s\n",\
            "{\"value\":"expected"}", userData.buffer);\
    }\
}

cx_int16 test_ser_composite_value(void) {
    cx_int16 result = 0;

    struct cx_serializer_s serializer = 
        cx_json_ser(CX_LOCAL, CX_NOT, CX_SERIALIZER_TRACE_NEVER);

    _test_ser_composite_value(p, "{\"x\":3,\"y\":4}");

    return result;
}

#undef _test_ser_composite_value