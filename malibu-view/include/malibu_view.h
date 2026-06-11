#ifndef MALIBU_VIEW_H
#define MALIBU_VIEW_H
// malibu_view.h — C API for embedding the Malibu engine (Task 31).
// All strings are UTF-8. The opaque MalibuView owns one engine instance.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MalibuView MalibuView;

// Sandbox flags (all capabilities disabled when their bit is set).
#define MALIBU_SANDBOX_NONE          0u
#define MALIBU_SANDBOX_NO_NETWORK    (1u << 0)
#define MALIBU_SANDBOX_NO_STORAGE    (1u << 1)
#define MALIBU_SANDBOX_NO_NAVIGATION (1u << 2)

typedef void (*MalibuMessageHandler)(const char* message, void* user_data);

MalibuView* malibu_view_create(void);
void        malibu_view_destroy(MalibuView* view);

// Navigation. Return 1 on success, 0 on failure.
int  malibu_view_load_html(MalibuView* view, const char* html, const char* base_url);
int  malibu_view_load_file(MalibuView* view, const char* path);
int  malibu_view_load_url(MalibuView* view, const char* url);
void malibu_view_reload(MalibuView* view);
int  malibu_view_go_back(MalibuView* view);
int  malibu_view_go_forward(MalibuView* view);

// Evaluate JS; returns a newly-allocated UTF-8 string (JSON result or
// "error: ..."). Free with malibu_view_free_string.
char* malibu_view_eval_js(MalibuView* view, const char* source);
void  malibu_view_free_string(char* str);

// Bidirectional messaging.
void malibu_view_post_message(MalibuView* view, const char* message);  // native -> JS
void malibu_view_set_message_handler(MalibuView* view, MalibuMessageHandler handler, void* user_data);

void malibu_view_set_sandbox(MalibuView* view, uint32_t flags);

// Renders the current document into an RGBA8 buffer of width*height*4 bytes.
// Returns 1 on success, 0 if out_size is too small.
int malibu_view_render(MalibuView* view, int width, int height, uint8_t* out_rgba, size_t out_size);

// Pumps the event loop (timers, promises, animation frames).
void malibu_view_run_tasks(MalibuView* view);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MALIBU_VIEW_H
