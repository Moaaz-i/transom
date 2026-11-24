#ifndef TRANSOM_PLUGIN_H
#define TRANSOM_PLUGIN_H

/* ============================================================================
 * Transom Native Plugin ABI — version 2
 * Full contract: docs/contracts/native-plugin-abi.md
 *
 * A native plugin is a shared library (.dylib / .dll / .so) shipped together with
 * a `plugin.tnative` manifest. The COMPILER only reads the manifest (functions
 * and widget schemas). The library is loaded at RUNTIME only, after the user's
 * trust policy (transom.lock) has verified its SHA-256.
 *
 * Rules for plugin authors:
 *  - Everything is called on the MAIN thread unless stated otherwise.
 *  - `const char*` arguments are valid only for the duration of the call; copy.
 *  - Strings you return must come from host->alloc_string (reclaimed by host).
 *  - Never keep pointers received from the host after the call returns.
 *  - Native plugins run with full process privileges; they are NOT sandboxed.
 * ========================================================================== */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRANSOM_PLUGIN_API_VERSION 2
#define TRANSOM_PLUGIN_API_MIN_VERSION 2

typedef enum TransomType {
    TRANSOM_TYPE_VOID   = 0,
    TRANSOM_TYPE_INT    = 1,
    TRANSOM_TYPE_FLOAT  = 2,
    TRANSOM_TYPE_BOOL   = 3,
    TRANSOM_TYPE_STRING = 4,
    TRANSOM_TYPE_ANY    = 5
} TransomType;

typedef enum TransomValKind {
    TRANSOM_VAL_NULL   = 0,
    TRANSOM_VAL_INT    = 1,
    TRANSOM_VAL_FLOAT  = 2,
    TRANSOM_VAL_BOOL   = 3,
    TRANSOM_VAL_STRING = 4
} TransomValKind;

typedef struct TransomValue {
    TransomValKind kind;
    int64_t i;
    double f;
    int b;
    const char* s;
} TransomValue;

typedef struct TransomHostAPI TransomHostAPI;

typedef TransomValue (*TransomFnCallback)(const TransomHostAPI* host,
                                          const TransomValue* args,
                                          int argc);

/* Runtime half of a widget (the compile-time half lives in plugin.tnative).
 * struct_size MUST be sizeof(TransomWidgetVTable) of the plugin's header. */
typedef struct TransomWidgetVTable {
    uint32_t struct_size;
    void* plugin_ctx;                        /* passed back to create() */
    void* (*create)(void* plugin_ctx);       /* returns an instance; NULL = failure */
    void  (*set_prop)(void* instance, const char* name, const TransomValue* value);
    /* Platform view owned by the instance: NSView* (macOS) / HWND (Windows). */
    void* (*native_handle)(void* instance);
    void  (*destroy)(void* instance);
} TransomWidgetVTable;

struct TransomHostAPI {
    uint32_t struct_size;   /* sizeof(TransomHostAPI) of the host */
    int api_version;        /* TRANSOM_PLUGIN_API_VERSION of the host */
    void* host_ctx;
    const char* platform;   /* "macos" | "windows" | "linux" */

    /* Register a function declared in plugin.tnative. Undeclared => rejected. */
    void (*register_fn)(void* host_ctx,
                        const char* fn_name,
                        const TransomType* param_types,
                        int param_count,
                        TransomType return_type,
                        TransomFnCallback cb);

    /* Allocate a host-managed UTF-8 string safe to return in TransomValue. */
    const char* (*alloc_string)(void* host_ctx, const char* utf8, size_t len);

    /* Register the runtime half of a widget declared in plugin.tnative.
     * Returns 0 on success. */
    int (*register_widget)(void* host_ctx,
                           const char* widget_name,
                           const TransomWidgetVTable* vtable);

    /* Report a human readable error; init should then return non-zero. */
    void (*set_error)(void* host_ctx, const char* message);

    /* Appended in P2 (still ABI v2: check host->struct_size before using).
     * Notify the app that a declared widget event fired; runs the widget's
     * trailing action block. `value` may be NULL. Main thread only. */
    void (*emit_widget_event)(void* host_ctx, void* instance,
                              const char* event, const TransomValue* value);
};

/* Inline value helpers for plugin authors */
static inline TransomValue transom_null(void) {
    TransomValue v = {TRANSOM_VAL_NULL, 0, 0.0, 0, NULL};
    return v;
}
static inline TransomValue transom_int(int64_t x) {
    TransomValue v = {TRANSOM_VAL_INT, x, 0.0, 0, NULL};
    return v;
}
static inline TransomValue transom_float(double x) {
    TransomValue v = {TRANSOM_VAL_FLOAT, 0, x, 0, NULL};
    return v;
}
static inline TransomValue transom_bool(int x) {
    TransomValue v = {TRANSOM_VAL_BOOL, 0, 0.0, x ? 1 : 0, NULL};
    return v;
}
static inline TransomValue transom_string(const TransomHostAPI* host,
                                          const char* utf8,
                                          size_t len) {
    TransomValue v = {TRANSOM_VAL_STRING, 0, 0.0, 0,
                      host->alloc_string(host->host_ctx, utf8, len)};
    return v;
}

/* Required export. Return 0 on success, non-zero on error (call set_error first).
 * Plugins MUST first check host->api_version >= TRANSOM_PLUGIN_API_MIN_VERSION. */
int transom_plugin_init(const TransomHostAPI* host);

/* Optional export, called once before the library is unloaded. */
void transom_plugin_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* TRANSOM_PLUGIN_H */
