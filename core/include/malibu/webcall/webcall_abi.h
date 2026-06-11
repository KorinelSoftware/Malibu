#pragma once
// core/include/malibu/webcall/webcall_abi.h
// Public WebCall ABI header (C API).

#include <cstdint>

#ifndef MALIBU_WEBCALL_ABI_VERSION
#define MALIBU_WEBCALL_ABI_VERSION 1u
#endif

typedef uint64_t MalibuHandle;
typedef struct MalibuValue MalibuValue;
typedef int32_t MalibuErrorCode;

#define MALIBU_OK             0
#define MALIBU_ERR_DEAD      -1
#define MALIBU_ERR_TYPE      -2
#define MALIBU_ERR_SECURITY  -3

typedef enum MalibuWebCallId : uint32_t {
    WEBCALL_DOM_QUERY_SELECTOR        = 0,
    WEBCALL_DOM_QUERY_SELECTOR_ALL    = 1,
    WEBCALL_DOM_CREATE_ELEMENT        = 2,
    WEBCALL_DOM_APPEND_CHILD          = 3,
    WEBCALL_DOM_REMOVE_CHILD          = 4,
    WEBCALL_DOM_REMOVE                = 5,
    WEBCALL_DOM_SET_TEXT_CONTENT      = 6,
    WEBCALL_DOM_GET_TEXT_CONTENT      = 7,
    WEBCALL_DOM_SET_ATTRIBUTE         = 8,
    WEBCALL_DOM_GET_ATTRIBUTE         = 9,
    WEBCALL_CSS_SET_STYLE             = 10,
    WEBCALL_CSS_GET_COMPUTED_STYLE    = 11,
    WEBCALL_EVENT_ADD_LISTENER        = 12,
    WEBCALL_EVENT_REMOVE_LISTENER     = 13,
    WEBCALL_FETCH                     = 14,
    WEBCALL_WEBSOCKET_CONNECT         = 15,
    WEBCALL_CANVAS_GET_CONTEXT        = 16,
    WEBCALL_CANVAS_DRAW_COMMAND       = 17,
    WEBCALL_MAX                       = 18
} MalibuWebCallId;

#ifdef __cplusplus
extern "C" {
#endif

MalibuErrorCode malibu_webcall(
    MalibuWebCallId id,
    MalibuHandle target,
    const void* args,
    uint32_t args_size,
    MalibuValue* result_out
);

#ifdef __cplusplus
}
#endif