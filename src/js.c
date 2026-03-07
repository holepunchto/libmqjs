#include <assert.h>
#include <intrusive.h>
#include <intrusive/list.h>
#include <js.h>
#include <limits.h>
#include <math.h>
#include <mquickjs.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define JS_CLASS_EXTERNAL (JS_CLASS_USER + 0)

#define JS_CLASS_COUNT (JS_CLASS_USER + 1)

#define JS_CFUNCTION_native_function_call (JS_CFUNCTION_USER + 0)

static JSValue
js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

static JSValue
js_external_constructor(JSContext *context, JSValue *receiver, int argc, JSValue *argv);

static void
js_external_finalizer(JSContext *context, void *opaque);

static JSValue
js_native_function_call(JSContext *ontext, JSValue *receiver, int argc, JSValue *argv, JSValue data);

#include "atoms.h"

typedef struct js_heap_s js_heap_t;
typedef struct js_callback_s js_callback_t;
typedef struct js_finalizer_s js_finalizer_t;
typedef struct js_finalizer_list_s js_finalizer_list_t;
typedef struct js_delegate_s js_delegate_t;
typedef struct js_threadsafe_queue_s js_threadsafe_queue_t;
typedef struct js_teardown_task_s js_teardown_task_t;
typedef struct js_teardown_queue_s js_teardown_queue_t;

struct js_deferred_teardown_s {
  js_env_t *env;
};

struct js_teardown_task_s {
  enum {
    js_immediate_teardown,
    js_deferred_teardown,
  } type;

  union {
    struct {
      js_teardown_cb cb;
    } immediate;

    struct {
      js_deferred_teardown_t handle;
      js_deferred_teardown_cb cb;
    } deferred;
  };

  void *data;
  intrusive_list_node_t list;
};

struct js_teardown_queue_s {
  intrusive_list_t tasks;
};

struct js_platform_s {
  js_platform_options_t options;
  uv_loop_t *loop;
};

struct js_heap_s {
  void *data;
  size_t size;
};

struct js_env_s {
  uv_loop_t *loop;
  uv_prepare_t prepare;
  uv_check_t check;
  uv_async_t teardown;
  int active_handles;

  js_platform_t *platform;
  js_handle_scope_t *scope;
  js_heap_t heap;

  uint32_t refs;
  uint32_t depth;

  JSContext *context;
  JSGCRef bindings;

  int64_t external_memory;

  bool destroying;

  js_teardown_queue_t teardown_queue;

  struct {
    js_uncaught_exception_cb uncaught_exception;
    void *uncaught_exception_data;
  } callbacks;
};

struct js_value_s {
  JSGCRef ref;
};

struct js_handle_scope_s {
  js_handle_scope_t *parent;
  js_value_t **values;
  size_t len;
  size_t capacity;
};

struct js_escapable_handle_scope_s {
  js_handle_scope_t *parent;
};

struct js_context_s {
  JSContext *context;
  JSContext *previous;
};

struct js_ref_s {
  JSGCRef ref;
  uint32_t count;
  bool finalized;
};

struct js_deferred_s {
  JSValue resolve;
  JSValue reject;
};

struct js_string_view_s {
  const char *value;
};

struct js_finalizer_s {
  void *data;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
};

struct js_finalizer_list_s {
  js_finalizer_t finalizer;
  js_finalizer_list_t *next;
};

struct js_delegate_s {
  js_delegate_callbacks_t callbacks;
  void *data;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
};

struct js_callback_s {
  js_function_cb cb;
  void *data;
};

struct js_callback_info_s {
  js_callback_t *callback;
  int argc;
  JSValue *argv;
  JSValue *receiver;
};

static const uint8_t js_threadsafe_function_idle = 0x0;
static const uint8_t js_threadsafe_function_running = 0x1;
static const uint8_t js_threadsafe_function_pending = 0x2;

struct js_threadsafe_queue_s {
  void **queue;
  size_t len;
  size_t capacity;
  size_t head;
  size_t tail;
  bool closed;
  uv_mutex_t lock;
};

struct js_threadsafe_function_s {
  JSGCRef function;
  js_env_t *env;
  uv_async_t async;
  js_threadsafe_queue_t queue;
  atomic_int state;
  atomic_int thread_count;
  void *context;
  js_finalize_cb finalize_cb;
  void *finalize_hint;
  js_threadsafe_function_cb cb;
};

static const char *js__platform_identifier = "mquickjs";

static const char *js__platform_version = "2025-12-22";

int
js_create_platform(uv_loop_t *loop, const js_platform_options_t *options, js_platform_t **result) {
  js_platform_t *platform = malloc(sizeof(js_platform_t));

  platform->loop = loop;
  platform->options = options ? *options : (js_platform_options_t){};

  *result = platform;

  return 0;
}

int
js_destroy_platform(js_platform_t *platform) {
  free(platform);

  return 0;
}

int
js_get_platform_identifier(js_platform_t *platform, const char **result) {
  *result = js__platform_identifier;

  return 0;
}

int
js_get_platform_version(js_platform_t *platform, const char **result) {
  *result = js__platform_version;

  return 0;
}

int
js_get_platform_limits(js_platform_t *platform, js_platform_limits_t *result) {
  result->arraybuffer_length = INT32_MAX;
  result->string_length = 0x3fffffff;

  return 0;
}

int
js_get_platform_loop(js_platform_t *platform, uv_loop_t **result) {
  *result = platform->loop;

  return 0;
}

static void
js__uncaught_exception(js_env_t *env, JSValue error) {
  int err;

  if (env->callbacks.uncaught_exception) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    env->callbacks.uncaught_exception(
      env,
      &(js_value_t){error},
      env->callbacks.uncaught_exception_data
    );

    err = js_close_handle_scope(env, scope);
    assert(err == 0);
  } else {
    JS_Throw(env->context, error);
  }
}

static void
js__on_prepare(uv_prepare_t *handle);

static inline void
js__on_check_liveness(js_env_t *env) {
  int err;

  if (true /* macrotask queue empty */) {
    err = uv_prepare_stop(&env->prepare);
  } else {
    err = uv_prepare_start(&env->prepare, js__on_prepare);
  }

  assert(err == 0);
}

static void
js__on_prepare(uv_prepare_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  js__on_check_liveness(env);
}

static void
js__on_check(uv_check_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  if (uv_loop_alive(env->loop)) return;

  js__on_check_liveness(env);
}

static void
js__on_handle_close(uv_handle_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  if (--env->active_handles == 0) {
#ifdef _WIN32
    VirtualFree(env->heap.data, env->heap.size, MEM_RELEASE);
#else
    munmap(env->heap.data, env->heap.size);
#endif

    free(env);
  }
}

static void
js__close_env(js_env_t *env) {
  JS_PopGCRef(env->context, &env->bindings);

  JS_FreeContext(env->context);

  uv_close((uv_handle_t *) &env->prepare, js__on_handle_close);
  uv_close((uv_handle_t *) &env->check, js__on_handle_close);
  uv_close((uv_handle_t *) &env->teardown, js__on_handle_close);
}

static void
js__on_teardown(uv_async_t *handle) {
  js_env_t *env = (js_env_t *) handle->data;

  if (env->refs == 0) js__close_env(env);
}

int
js_create_env(uv_loop_t *loop, js_platform_t *platform, const js_env_options_t *options, js_env_t **result) {
  int err;

  js_heap_t heap;

  if (options && options->memory_limit) {
    heap.size = options->memory_limit;
  } else {
    uint64_t constrained_memory = uv_get_constrained_memory();
    uint64_t total_memory = uv_get_total_memory();

    if (constrained_memory > 0 && constrained_memory < total_memory) {
      total_memory = constrained_memory;
    }

    if (total_memory > 0) {
      heap.size = total_memory;
    } else {
      heap.size = 512 * 1024;
    }
  }

  if (heap.size > 0x10000000) heap.size = 0x10000000;

#ifdef _WIN32
  heap.data = VirtualAlloc(NULL, heap.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  heap.data = mmap(NULL, heap.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (heap.data == MAP_FAILED) return -1;
#endif

  js_env_t *env = malloc(sizeof(js_env_t));

  env->loop = loop;
  env->active_handles = 3;

  env->platform = platform;
  env->scope = NULL;
  env->heap = heap;

  env->refs = 0;
  env->depth = 0;

  env->context = JS_NewContext(heap.data, heap.size, &js_stdlib);

  JS_PushGCRef(env->context, &env->bindings);

  env->bindings.val = JS_NewObject(env->context);

  env->external_memory = 0;

  env->destroying = false;

  intrusive_list_init(&env->teardown_queue.tasks);

  env->callbacks.uncaught_exception = NULL;
  env->callbacks.uncaught_exception_data = NULL;

  JS_SetContextOpaque(env->context, env);

  err = uv_prepare_init(loop, &env->prepare);
  assert(err == 0);

  err = uv_prepare_start(&env->prepare, js__on_prepare);
  assert(err == 0);

  env->prepare.data = (void *) env;

  err = uv_check_init(loop, &env->check);
  assert(err == 0);

  err = uv_check_start(&env->check, js__on_check);
  assert(err == 0);

  env->check.data = (void *) env;

  // The check handle should not on its own keep the loop alive; it's simply
  // used for running any outstanding tasks that might cause additional work
  // to be queued.
  uv_unref((uv_handle_t *) &env->check);

  err = uv_async_init(loop, &env->teardown, js__on_teardown);
  assert(err == 0);

  env->teardown.data = (void *) env;

  uv_unref((uv_handle_t *) &env->teardown);

  *result = env;

  return 0;
}

int
js_destroy_env(js_env_t *env) {
  env->destroying = true;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    js_teardown_task_t *task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_deferred_teardown) {
      task->deferred.cb(&task->deferred.handle, task->data);
    } else {
      task->immediate.cb(task->data);

      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      free(task);
    }
  }

  if (env->refs == 0) {
    js__close_env(env);
  } else {
    uv_ref((uv_handle_t *) &env->teardown);
  }

  return 0;
}

int
js_on_uncaught_exception(js_env_t *env, js_uncaught_exception_cb cb, void *data) {
  env->callbacks.uncaught_exception = cb;
  env->callbacks.uncaught_exception_data = data;

  return 0;
}

int
js_on_unhandled_rejection(js_env_t *env, js_unhandled_rejection_cb cb, void *data) {
  return 0;
}

int
js_on_dynamic_import(js_env_t *env, js_dynamic_import_cb cb, void *data) {
  return 0;
}

int
js_on_dynamic_import_transitional(js_env_t *env, js_dynamic_import_transitional_cb cb, void *data) {
  return 0;
}

int
js_get_env_loop(js_env_t *env, uv_loop_t **result) {
  *result = env->loop;

  return 0;
}

int
js_get_env_platform(js_env_t *env, js_platform_t **result) {
  *result = env->platform;

  return 0;
}

static inline int
js__error(js_env_t *env) {
  return JS_HasException(env->context) ? js_pending_exception : js_uncaught_exception;
}

int
js_open_handle_scope(js_env_t *env, js_handle_scope_t **result) {
  // Allow continuing even with a pending exception

  js_handle_scope_t *scope = malloc(sizeof(js_handle_scope_t));

  scope->parent = env->scope;
  scope->values = NULL;
  scope->len = 0;
  scope->capacity = 0;

  env->scope = scope;

  *result = scope;

  return 0;
}

int
js_close_handle_scope(js_env_t *env, js_handle_scope_t *scope) {
  // Allow continuing even with a pending exception

  for (size_t i = scope->len; i-- > 0;) {
    js_value_t *value = scope->values[i];

    JS_PopGCRef(env->context, &value->ref);

    free(value);
  }

  env->scope = scope->parent;

  if (scope->values) free(scope->values);

  free(scope);

  return 0;
}

int
js_open_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t **result) {
  return js_open_handle_scope(env, (js_handle_scope_t **) result);
}

int
js_close_escapable_handle_scope(js_env_t *env, js_escapable_handle_scope_t *scope) {
  return js_close_handle_scope(env, (js_handle_scope_t *) scope);
}

static inline void
js__attach_to_handle_scope(js_env_t *env, js_handle_scope_t *scope, js_value_t *value) {
  assert(scope);

  if (scope->len >= scope->capacity) {
    if (scope->capacity) scope->capacity *= 2;
    else scope->capacity = 4;

    scope->values = realloc(scope->values, scope->capacity * sizeof(js_value_t *));
  }

  scope->values[scope->len++] = value;
}

int
js_escape_handle(js_env_t *env, js_escapable_handle_scope_t *scope, js_value_t *escapee, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = escapee->ref.val;

  *result = wrapper;

  js__attach_to_handle_scope(env, scope->parent, wrapper);

  return 0;
}

int
js_create_context(js_env_t *env, js_context_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_destroy_context(js_env_t *env, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_enter_context(js_env_t *env, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_exit_context(js_env_t *env, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_bindings(js_env_t *env, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = env->bindings.val;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_run_script(js_env_t *env, const char *file, size_t len, int offset, js_value_t *source, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  size_t str_len;
  JSCStringBuf buf;
  const char *str = JS_ToCStringLen(env->context, &str_len, source->ref.val, &buf);

  env->depth++;

  if (file == NULL) file = "";

  JSValue value = JS_Eval(
    env->context,
    str,
    str_len,
    file,
    JS_EVAL_RETVAL
  );

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  if (result) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_create_module(js_env_t *env, const char *name, size_t len, int offset, js_value_t *source, js_module_meta_cb cb, void *data, js_module_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_synthetic_module(js_env_t *env, const char *name, size_t len, js_value_t *const export_names[], size_t names_len, js_module_evaluate_cb cb, void *data, js_module_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_delete_module(js_env_t *env, js_module_t *module) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_module_name(js_env_t *env, js_module_t *module, const char **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_module_namespace(js_env_t *env, js_module_t *module, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_set_module_export(js_env_t *env, js_module_t *module, js_value_t *name, js_value_t *value) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_instantiate_module(js_env_t *env, js_module_t *module, js_module_resolve_cb cb, void *data) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_run_module(js_env_t *env, js_module_t *module, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

static void
js__on_reference_finalize(js_env_t *env, void *data, void *finalize_hint) {
  js_ref_t *reference = (js_ref_t *) data;

  reference->ref.val = JS_NULL;
  reference->count = 0;
  reference->finalized = true;
}

static inline void
js__set_weak_reference(js_env_t *env, js_ref_t *reference) {
  if (reference->finalized) return;

  if (JS_IsObject(env->context, reference->ref.val)) {
    // TODO
  }
}

static inline void
js__clear_weak_reference(js_env_t *env, js_ref_t *reference) {
  if (reference->finalized) return;

  if (JS_IsObject(env->context, reference->ref.val)) {
    // TODO
  }
}

int
js_create_reference(js_env_t *env, js_value_t *value, uint32_t count, js_ref_t **result) {
  // Allow continuing even with a pending exception

  js_ref_t *reference = malloc(sizeof(js_ref_t));

  JS_AddGCRef(env->context, &reference->ref);

  reference->ref.val = value->ref.val;
  reference->count = count;
  reference->finalized = false;

  if (reference->count == 0) js__set_weak_reference(env, reference);

  *result = reference;

  return 0;
}

int
js_delete_reference(js_env_t *env, js_ref_t *reference) {
  // Allow continuing even with a pending exception

  if (reference->count == 0) js__clear_weak_reference(env, reference);

  JS_DeleteGCRef(env->context, &reference->ref);

  free(reference);

  return 0;
}

int
js_reference_ref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  // Allow continuing even with a pending exception

  reference->count++;

  if (reference->count == 1) js__clear_weak_reference(env, reference);

  if (result) *result = reference->count;

  return 0;
}

int
js_reference_unref(js_env_t *env, js_ref_t *reference, uint32_t *result) {
  // Allow continuing even with a pending exception

  if (reference->count > 0) {
    reference->count--;

    if (reference->count == 0) js__set_weak_reference(env, reference);
  }

  if (result) *result = reference->count;

  return 0;
}

int
js_get_reference_value(js_env_t *env, js_ref_t *reference, js_value_t **result) {
  // Allow continuing even with a pending exception

  if (reference->finalized) *result = NULL;
  else {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = reference->ref.val;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_define_class(js_env_t *env, const char *name, size_t len, js_function_cb constructor, void *data, js_property_descriptor_t const properties[], size_t properties_len, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_define_properties(js_env_t *env, js_value_t *object, js_property_descriptor_t const properties[], size_t properties_len) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_wrap(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_unwrap(js_env_t *env, js_value_t *object, void **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_remove_wrap(js_env_t *env, js_value_t *object, void **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_delegate(js_env_t *env, const js_delegate_callbacks_t *callbacks, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_add_finalizer(js_env_t *env, js_value_t *object, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_ref_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_add_type_tag(js_env_t *env, js_value_t *object, const js_type_tag_t *tag) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_check_type_tag(js_env_t *env, js_value_t *object, const js_type_tag_t *tag, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_int32(js_env_t *env, int32_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewInt32(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_uint32(js_env_t *env, uint32_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewUint32(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_int64(js_env_t *env, int64_t value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewInt64(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_double(js_env_t *env, double value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewFloat64(env->context, value);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_bigint_int64(js_env_t *env, int64_t value, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_bigint_uint64(js_env_t *env, uint64_t value, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_bigint_words(js_env_t *env, int sign, const uint64_t *words, size_t len, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_string_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue value;

  if (len == (size_t) -1) {
    value = JS_NewString(env->context, (char *) str);
  } else {
    value = JS_NewStringLen(env->context, (char *) str, len);
  }

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = value;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_string_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  if (len == (size_t) -1) len = wcslen((wchar_t *) str);

  size_t utf8_len = utf8_length_from_utf16le(str, len);

  utf8_t *utf8 = malloc(utf8_len);

  utf16le_convert_to_utf8(str, len, utf8);

  JSValue value = JS_NewStringLen(env->context, (char *) utf8, utf8_len);

  free(utf8);

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = value;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_string_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  if (len == (size_t) -1) len = strlen((char *) str);

  size_t utf8_len = utf8_length_from_latin1(str, len);

  utf8_t *utf8 = malloc(utf8_len);

  latin1_convert_to_utf8(str, len, utf8);

  JSValue value = JS_NewStringLen(env->context, (char *) utf8, utf8_len);

  free(utf8);

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = value;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_external_string_utf8(js_env_t *env, utf8_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  int err;
  err = js_create_string_utf8(env, str, len, result);
  if (err < 0) return err;

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

int
js_create_external_string_utf16le(js_env_t *env, utf16_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  int err;
  err = js_create_string_utf16le(env, str, len, result);
  if (err < 0) return err;

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

int
js_create_external_string_latin1(js_env_t *env, latin1_t *str, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result, bool *copied) {
  int err;
  err = js_create_string_latin1(env, str, len, result);
  if (err < 0) return err;

  if (copied) *copied = true;

  if (finalize_cb) finalize_cb(env, str, finalize_hint);

  return 0;
}

int
js_create_property_key_utf8(js_env_t *env, const utf8_t *str, size_t len, js_value_t **result) {
  return js_create_string_utf8(env, str, len, result);
}

int
js_create_property_key_utf16le(js_env_t *env, const utf16_t *str, size_t len, js_value_t **result) {
  return js_create_string_utf16le(env, str, len, result);
}

int
js_create_property_key_latin1(js_env_t *env, const latin1_t *str, size_t len, js_value_t **result) {
  return js_create_string_latin1(env, str, len, result);
}

int
js_create_symbol(js_env_t *env, js_value_t *description, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_symbol_for(js_env_t *env, const char *description, size_t len, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_object(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewObject(env->context);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static JSValue
js_native_function_call(JSContext *context, JSValue *receiver, int argc, JSValue *argv, JSValue data) {
  int err;

  js_env_t *env = JS_GetContextOpaque(context);

  js_callback_t *callback = JS_GetOpaque(context, data);

  js_callback_info_t callback_info = {
    .callback = callback,
    .argc = argc,
    .argv = argv,
    .receiver = receiver,
  };

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result = callback->cb(env, &callback_info);

  JSValue value;

  if (JS_HasException(env->context)) {
    value = JS_EXCEPTION;
  } else {
    if (result) value = result->ref.val;
    else value = JS_UNDEFINED;
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  return value;
}

int
js_create_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, void *data, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_callback_t *callback = malloc(sizeof(js_callback_t));

  callback->cb = cb;
  callback->data = data;

  JSValue external = JS_NewObjectClassUser(env->context, JS_CLASS_EXTERNAL);

  JS_SetOpaque(env->context, external, callback);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewCFunctionParams(env->context, JS_CFUNCTION_native_function_call, external);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_function_with_source(js_env_t *env, const char *name, size_t name_len, const char *file, size_t file_len, js_value_t *const args[], size_t args_len, int offset, js_value_t *source, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_typed_function(js_env_t *env, const char *name, size_t len, js_function_cb cb, const js_callback_signature_t *signature, const void *address, void *data, js_value_t **result) {
  return js_create_function(env, name, len, cb, data, result);
}

int
js_create_array(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewArray(env->context, 0);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_array_with_length(js_env_t *env, size_t len, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NewArray(env->context, 0);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static JSValue
js_external_constructor(JSContext *context, JSValue *receiver, int argc, JSValue *argv) {
  return JS_NewObjectClassUser(context, JS_CLASS_EXTERNAL);
}

static void
js_external_finalizer(JSContext *context, void *opaque) {
  js_env_t *env = JS_GetContextOpaque(context);

  js_finalizer_t *finalizer = opaque;

  if (finalizer == NULL) return;

  if (finalizer->finalize_cb) {
    finalizer->finalize_cb(env, finalizer->data, finalizer->finalize_hint);
  }

  free(finalizer);
}

int
js_create_external(js_env_t *env, void *data, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_finalizer_t *finalizer = malloc(sizeof(js_finalizer_t));

  finalizer->data = data;
  finalizer->finalize_cb = finalize_cb;
  finalizer->finalize_hint = finalize_hint;

  JSValue external = JS_NewObjectClassUser(env->context, JS_CLASS_EXTERNAL);

  JS_SetOpaque(env->context, external, finalizer);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = external;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

static JSValue
js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
  int err;

  uv_timeval64_t tv;
  err = uv_gettimeofday(&tv);
  assert(err == 0);

  return JS_NewInt64(ctx, (int64_t) tv.tv_sec * 1000 + (tv.tv_usec / 1000));
}

int
js_create_date(js_env_t *env, double time, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_type_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_range_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_syntax_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_reference_error(js_env_t *env, js_value_t *code, js_value_t *message, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_error_location(js_env_t *env, js_value_t *error, js_error_location_t *result) {
  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_UNDEFINED;

  result->name = wrapper;
  result->source = wrapper;
  result->line = 0;
  result->column_start = -1;
  result->column_end = -1;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_create_promise(js_env_t *env, js_deferred_t **deferred, js_value_t **promise) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_resolve_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_reject_deferred(js_env_t *env, js_deferred_t *deferred, js_value_t *resolution) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_promise_state(js_env_t *env, js_value_t *promise, js_promise_state_t *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_promise_result(js_env_t *env, js_value_t *promise, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_arraybuffer_with_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store, void **data, size_t *len, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_unsafe_arraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_external_arraybuffer(js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_detach_arraybuffer(js_env_t *env, js_value_t *arraybuffer) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_arraybuffer_backing_store(js_env_t *env, js_value_t *arraybuffer, js_arraybuffer_backing_store_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_sharedarraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_external_sharedarraybuffer(js_env_t *env, void *data, size_t len, js_finalize_cb finalize_cb, void *finalize_hint, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_sharedarraybuffer_with_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store, void **data, size_t *len, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_unsafe_sharedarraybuffer(js_env_t *env, size_t len, void **data, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_sharedarraybuffer_backing_store(js_env_t *env, js_value_t *sharedarraybuffer, js_arraybuffer_backing_store_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_release_arraybuffer_backing_store(js_env_t *env, js_arraybuffer_backing_store_t *backing_store) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_typedarray(js_env_t *env, js_typedarray_type_t type, size_t len, js_value_t *arraybuffer, size_t offset, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_create_dataview(js_env_t *env, size_t len, js_value_t *arraybuffer, size_t offset, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_coerce_to_boolean(js_env_t *env, js_value_t *value, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_coerce_to_number(js_env_t *env, js_value_t *value, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_coerce_to_string(js_env_t *env, js_value_t *value, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  JSValue string = JS_ToString(env->context, value->ref.val);

  if (JS_IsException(string)) return js__error(env);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = string;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_coerce_to_object(js_env_t *env, js_value_t *value, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_typeof(js_env_t *env, js_value_t *value, js_value_type_t *result) {
  // Allow continuing even with a pending exception

  if (JS_IsNumber(env->context, value->ref.val)) {
    *result = js_number;
  } else if (JS_IsString(env->context, value->ref.val)) {
    *result = js_string;
  } else if (JS_IsFunction(env->context, value->ref.val)) {
    *result = js_function;
  } else if (JS_IsObject(env->context, value->ref.val)) {
    *result = js_object;
  } else if (JS_IsBool(value->ref.val)) {
    *result = js_boolean;
  } else if (JS_IsUndefined(value->ref.val)) {
    *result = js_undefined;
  } else if (JS_IsNull(value->ref.val)) {
    *result = js_null;
  }

  return 0;
}

int
js_instanceof(js_env_t *env, js_value_t *object, js_value_t *constructor, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_is_undefined(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsUndefined(value->ref.val);

  return 0;
}

int
js_is_null(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsNull(value->ref.val);

  return 0;
}

int
js_is_boolean(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsBool(value->ref.val);

  return 0;
}

int
js_is_number(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsNumber(env->context, value->ref.val);

  return 0;
}

int
js_is_int32(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  if (JS_IsNumber(env->context, value->ref.val)) {
    double number;

    JS_ToNumber(env->context, &number, value->ref.val);

    double integral;

    *result = modf(number, &integral) == 0.0 && integral >= INT32_MIN && integral <= INT32_MAX;
  } else {
    *result = false;
  }

  return 0;
}

int
js_is_uint32(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  if (JS_IsNumber(env->context, value->ref.val)) {
    double number;

    JS_ToNumber(env->context, &number, value->ref.val);

    double integral;

    *result = modf(number, &integral) == 0.0 && integral >= 0.0 && integral <= UINT32_MAX;
  } else {
    *result = false;
  }

  return 0;
}

int
js_is_string(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsString(env->context, value->ref.val);

  return 0;
}

int
js_is_symbol(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_object(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsObject(env->context, value->ref.val);

  return 0;
}

int
js_is_function(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsFunction(env->context, value->ref.val);

  return 0;
}

int
js_is_async_function(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_generator_function(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_generator(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_arguments(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsArray(env->context, value->ref.val);

  return 0;
}

int
js_is_external(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_wrapped(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_delegate(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_bigint(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_date(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_regexp(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_error(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_IsError(env->context, value->ref.val);

  return 0;
}

int
js_is_promise(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_proxy(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_map(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_map_iterator(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_set(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_set_iterator(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_weak_map(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_weak_set(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_weak_ref(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_arraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_detached_arraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_sharedarraybuffer(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_typedarray(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_int8array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_uint8array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_uint8clampedarray(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_int16array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_uint16array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_int32array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_uint32array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_float16array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_float32array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_float64array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_bigint64array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_biguint64array(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_dataview(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_is_module_namespace(js_env_t *env, js_value_t *value, bool *result) {
  // Allow continuing even with a pending exception

  *result = false;

  return 0;
}

int
js_strict_equals(js_env_t *env, js_value_t *a, js_value_t *b, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_global(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_GetGlobalObject(env->context);

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_undefined(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_UNDEFINED;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_null(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NULL;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_boolean(js_env_t *env, bool value, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = value ? JS_TRUE : JS_FALSE;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_value_bool(js_env_t *env, js_value_t *value, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_value_int32(js_env_t *env, js_value_t *value, int32_t *result) {
  // Allow continuing even with a pending exception

  JS_ToInt32(env->context, result, value->ref.val);

  return 0;
}

int
js_get_value_uint32(js_env_t *env, js_value_t *value, uint32_t *result) {
  // Allow continuing even with a pending exception

  JS_ToUint32(env->context, result, value->ref.val);

  return 0;
}

int
js_get_value_int64(js_env_t *env, js_value_t *value, int64_t *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_value_double(js_env_t *env, js_value_t *value, double *result) {
  // Allow continuing even with a pending exception

  JS_ToNumber(env->context, result, value->ref.val);

  return 0;
}

int
js_get_value_bigint_int64(js_env_t *env, js_value_t *value, int64_t *result, bool *lossless) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_value_bigint_uint64(js_env_t *env, js_value_t *value, uint64_t *result, bool *lossless) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_value_bigint_words(js_env_t *env, js_value_t *value, int *sign, uint64_t *words, size_t len, size_t *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_value_string_utf8(js_env_t *env, js_value_t *value, utf8_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  size_t cstr_len;
  JSCStringBuf buf;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->ref.val, &buf);

  if (str == NULL) {
    *result = cstr_len;
  } else if (len != 0) {
    size_t written = cstr_len < len ? cstr_len : len;

    memcpy(str, cstr, written);

    if (written < len) str[written] = '\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  return 0;
}

int
js_get_value_string_utf16le(js_env_t *env, js_value_t *value, utf16_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  size_t cstr_len;
  JSCStringBuf buf;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->ref.val, &buf);

  size_t utf16_len = utf16_length_from_utf8((utf8_t *) cstr, cstr_len);

  if (str == NULL) {
    *result = utf16_len;
  } else if (len != 0) {
    size_t written = utf16_len < len ? utf16_len : len;

    utf8_convert_to_utf16le((utf8_t *) cstr, cstr_len, str);

    if (written < len) str[written] = L'\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  return 0;
}

int
js_get_value_string_latin1(js_env_t *env, js_value_t *value, latin1_t *str, size_t len, size_t *result) {
  // Allow continuing even with a pending exception

  size_t cstr_len;
  JSCStringBuf buf;
  const char *cstr = JS_ToCStringLen(env->context, &cstr_len, value->ref.val, &buf);

  size_t latin1_len = latin1_length_from_utf8((utf8_t *) cstr, cstr_len);

  if (str == NULL) {
    *result = latin1_len;
  } else if (len != 0) {
    size_t written = latin1_len < len ? latin1_len : len;

    utf8_convert_to_latin1((utf8_t *) cstr, cstr_len, str);

    if (written < len) str[written] = '\0';

    if (result) *result = written;
  } else if (result) *result = 0;

  return 0;
}

int
js_get_value_external(js_env_t *env, js_value_t *value, void **result) {
  // Allow continuing even with a pending exception

  js_finalizer_t *finalizer = JS_GetOpaque(env->context, value->ref.val);

  *result = finalizer->data;

  return 0;
}

int
js_get_value_date(js_env_t *env, js_value_t *value, double *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_array_length(js_env_t *env, js_value_t *array, uint32_t *result) {
  // Allow continuing even with a pending exception

  JSValue length = JS_GetPropertyStr(env->context, array->ref.val, "length");

  JS_ToUint32(env->context, result, length);

  return 0;
}

int
js_get_array_elements(js_env_t *env, js_value_t *array, js_value_t **elements, size_t len, size_t offset, uint32_t *result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  uint32_t written = 0;

  env->depth++;

  uint32_t m;
  err = js_get_array_length(env, array, &m);
  assert(err == 0);

  for (uint32_t i = 0, n = len, j = offset; i < n && j < m; i++, j++) {
    JSValue value = JS_GetPropertyUint32(env->context, array->ref.val, j);

    if (JS_IsException(value)) {
      env->depth--;

      if (env->depth == 0) {
        JSValue error = JS_GetException(env->context);

        js__uncaught_exception(env, error);
      }

      return js__error(env);
    }

    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = value;

    elements[i] = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);

    written++;
  }

  env->depth--;

  if (result) *result = written;

  return 0;
}

int
js_set_array_elements(js_env_t *env, js_value_t *array, const js_value_t *elements[], size_t len, size_t offset) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  for (uint32_t i = 0, n = len, j = offset; i < n; i++, j++) {
    int success = JS_SetPropertyUint32(env->context, array->ref.val, j, elements[i]->ref.val);

    if (success < 0) {
      env->depth--;

      if (env->depth == 0) {
        JSValue error = JS_GetException(env->context);

        js__uncaught_exception(env, error);
      }

      return js__error(env);
    }
  }

  env->depth--;

  return 0;
}

int
js_get_prototype(js_env_t *env, js_value_t *object, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_property_names(js_env_t *env, js_value_t *object, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_filtered_property_names(js_env_t *env, js_value_t *object, js_key_collection_mode_t mode, js_property_filter_t property_filter, js_index_filter_t index_filter, js_key_conversion_mode_t key_conversion, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_has_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_has_own_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_set_property(js_env_t *env, js_value_t *object, js_value_t *key, js_value_t *value) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_delete_property(js_env_t *env, js_value_t *object, js_value_t *key, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  JSValue value = JS_GetPropertyStr(env->context, object->ref.val, name);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  if (result) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_has_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_set_named_property(js_env_t *env, js_value_t *object, const char *name, js_value_t *value) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  int success = JS_SetPropertyStr(env->context, object->ref.val, name, value->ref.val);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  return 0;
}

int
js_delete_named_property(js_env_t *env, js_value_t *object, const char *name, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  JSValue value = JS_GetPropertyUint32(env->context, object->ref.val, index);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  if (result) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_has_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_set_element(js_env_t *env, js_value_t *object, uint32_t index, js_value_t *value) {
  if (JS_HasException(env->context)) return js__error(env);

  env->depth++;

  int success = JS_SetPropertyUint32(env->context, object->ref.val, index, value->ref.val);

  env->depth--;

  if (success < 0) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  return 0;
}

int
js_delete_element(js_env_t *env, js_value_t *object, uint32_t index, bool *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_string_view(js_env_t *env, js_value_t *string, js_string_encoding_t *encoding, const void **str, size_t *len, js_string_view_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_release_string_view(js_env_t *env, js_string_view_t *view) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_callback_info(js_env_t *env, const js_callback_info_t *info, size_t *argc, js_value_t *argv[], js_value_t **receiver, void **data) {
  // Allow continuing even with a pending exception

  if (argv) {
    size_t i = 0, n = info->argc < *argc ? info->argc : *argc;

    for (; i < n; i++) {
      js_value_t *wrapper = malloc(sizeof(js_value_t));

      JS_PushGCRef(env->context, &wrapper->ref);

      wrapper->ref.val = info->argv[i];

      argv[i] = wrapper;

      js__attach_to_handle_scope(env, env->scope, wrapper);
    }

    n = *argc;

    if (i < n) {
      js_value_t *wrapper = malloc(sizeof(js_value_t));

      JS_PushGCRef(env->context, &wrapper->ref);

      wrapper->ref.val = JS_UNDEFINED;

      js__attach_to_handle_scope(env, env->scope, wrapper);

      for (; i < n; i++) {
        argv[i] = wrapper;
      }
    }
  }

  if (argc) {
    *argc = info->argc;
  }

  if (receiver) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = *info->receiver;

    *receiver = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  if (data) {
    *data = info->callback->data;
  }

  return 0;
}

int
js_get_typed_callback_info(const js_typed_callback_info_t *info, js_env_t **env, void **data) {
  // Allow continuing even with a pending exception

  return 0;
}

int
js_get_new_target(js_env_t *env, const js_callback_info_t *info, js_value_t **result) {
  // Allow continuing even with a pending exception

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = JS_NULL;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_get_arraybuffer_info(js_env_t *env, js_value_t *arraybuffer, void **pdata, size_t *plen) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_sharedarraybuffer_info(js_env_t *env, js_value_t *sharedarraybuffer, void **pdata, size_t *plen) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_typedarray_info(js_env_t *env, js_value_t *typedarray, js_typedarray_type_t *ptype, void **pdata, size_t *plen, js_value_t **parraybuffer, size_t *poffset) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_dataview_info(js_env_t *env, js_value_t *dataview, void **pdata, size_t *plen, js_value_t **parraybuffer, size_t *poffset) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_call_function(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  int err;

  if (JS_StackCheck(env->context, argc + 2) < 0) {
    err = js_throw_range_error(env, NULL, "Maximum call stack size exceeded");
    assert(err == 0);

    return js__error(env);
  }

  for (size_t i = argc; i-- > 0;) {
    JS_PushArg(env->context, argv[i]->ref.val);
  }

  JS_PushArg(env->context, function->ref.val);
  JS_PushArg(env->context, receiver->ref.val);

  env->depth++;

  JSValue value = JS_Call(env->context, argc);

  env->depth--;

  if (JS_IsException(value)) {
    if (env->depth == 0) {
      JSValue error = JS_GetException(env->context);

      js__uncaught_exception(env, error);
    }

    return js__error(env);
  }

  if (result) {
    js_value_t *wrapper = malloc(sizeof(js_value_t));

    JS_PushGCRef(env->context, &wrapper->ref);

    wrapper->ref.val = value;

    *result = wrapper;

    js__attach_to_handle_scope(env, env->scope, wrapper);
  }

  return 0;
}

int
js_call_function_with_checkpoint(js_env_t *env, js_value_t *receiver, js_value_t *function, size_t argc, js_value_t *const argv[], js_value_t **result) {
  return js_call_function(env, receiver, function, argc, argv, result);
}

int
js_new_instance(js_env_t *env, js_value_t *constructor, size_t argc, js_value_t *const argv[], js_value_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

static inline bool
js__threadsafe_queue_push(js_threadsafe_queue_t *queue, void *data) {
  uv_mutex_lock(&queue->lock);

  if (queue->closed) {
    uv_mutex_unlock(&queue->lock);

    return false;
  }

  if (queue->len == queue->capacity) {
    size_t capacity = queue->capacity ? queue->capacity << 1u : 16;

    void **resized = realloc(queue->queue, capacity * sizeof(void *));

    if (queue->head > 0) {
      size_t head_len = queue->capacity - queue->head;

      if (head_len > 0) {
        memmove(&resized[capacity - head_len], &resized[queue->head], head_len * sizeof(void *));
      }

      queue->head = capacity - head_len;
    }

    queue->queue = resized;
    queue->capacity = capacity;
  }

  queue->queue[queue->tail] = data;
  queue->tail = (queue->tail + 1) & (queue->capacity - 1);
  queue->len++;

  uv_mutex_unlock(&queue->lock);

  return true;
}

static inline bool
js__threadsafe_queue_pop(js_threadsafe_queue_t *queue, void **result) {
  uv_mutex_lock(&queue->lock);

  if (queue->len == 0) {
    uv_mutex_unlock(&queue->lock);

    return false;
  }

  void *data = queue->queue[queue->head];

  queue->head = (queue->head + 1) & (queue->capacity - 1);
  queue->len--;

  if (queue->capacity > 16 && queue->len <= queue->capacity / 4) {
    size_t capacity = queue->capacity >> 1u;

    void **resized = malloc(capacity * sizeof(void *));

    for (size_t i = 0; i < queue->len; i++) {
      resized[i] = queue->queue[(queue->head + i) & (queue->capacity - 1)];
    }

    free(queue->queue);

    queue->queue = resized;
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = queue->len;
  }

  uv_mutex_unlock(&queue->lock);

  *result = data;

  return true;
}

static inline void
js__threadsafe_queue_close(js_threadsafe_queue_t *queue) {
  uv_mutex_lock(&queue->lock);

  queue->closed = true;

  uv_mutex_unlock(&queue->lock);
}

static inline void
js__threadsafe_function_finalize(js_threadsafe_function_t *function) {
  js_env_t *env = function->env;

  if (function->finalize_cb) {
    function->finalize_cb(env, function->context, function->finalize_hint);
  }

  JS_DeleteGCRef(env->context, &function->function);

  free(function);
}

static void
js__on_threadsafe_function_close(uv_handle_t *handle) {
  js_threadsafe_function_t *function = handle->data;

  js__threadsafe_function_finalize(function);
}

static inline void
js__threadsafe_function_close(js_threadsafe_function_t *function) {
  uv_close((uv_handle_t *) &function->async, js__on_threadsafe_function_close);
}

static inline bool
js__threadsafe_function_call(js_threadsafe_function_t *function) {
  int err;

  js_env_t *env = function->env;

  void *data;

  if (js__threadsafe_queue_pop(&function->queue, &data)) {
    js_handle_scope_t *scope;
    err = js_open_handle_scope(env, &scope);
    assert(err == 0);

    if (function->cb) {
      function->cb(env, &(js_value_t){function->function}, function->context, data);
    } else {
      js_value_t *receiver;
      err = js_get_undefined(env, &receiver);
      assert(err == 0);

      err = js_call_function(env, receiver, &(js_value_t){function->function}, 0, NULL, NULL);
      (void) err;
    }

    err = js_close_handle_scope(env, scope);
    assert(err == 0);

    return true;
  }

  if (atomic_load_explicit(&function->thread_count, memory_order_relaxed) == 0) {
    js__threadsafe_function_close(function);
  }

  return false;
}

static inline void
js__threadsafe_function_signal(js_threadsafe_function_t *function) {
  int err;

  int state = atomic_fetch_or_explicit(&function->state, js_threadsafe_function_pending, memory_order_acq_rel);

  if (state & js_threadsafe_function_running) {
    return;
  }

  err = uv_async_send(&function->async);
  assert(err == 0);
}

static inline void
js__threadsafe_function_dispatch(js_threadsafe_function_t *function) {
  bool done = false;

  int iterations = 1024;

  while (!done && --iterations >= 0) {
    atomic_store_explicit(&function->state, js_threadsafe_function_running, memory_order_release);

    done = js__threadsafe_function_call(function) == false;

    if (atomic_exchange_explicit(&function->state, js_threadsafe_function_idle, memory_order_acq_rel) != js_threadsafe_function_running) {
      done = false;
    }
  }

  if (!done) js__threadsafe_function_signal(function);
}

static void
js__on_threadsafe_function_async(uv_async_t *handle) {
  js_threadsafe_function_t *function = handle->data;

  js__threadsafe_function_dispatch(function);
}

int
js_create_threadsafe_function(js_env_t *env, js_value_t *function, size_t queue_limit, size_t initial_thread_count, js_finalize_cb finalize_cb, void *finalize_hint, void *context, js_threadsafe_function_cb cb, js_threadsafe_function_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  if (function == NULL && cb == NULL) {
    err = js_throw_error(env, NULL, "Either a function or a callback must be provided");
    assert(err == 0);

    return js__error(env);
  };

  if (initial_thread_count == 0) {
    err = js_throw_error(env, NULL, "Initial thread count must be greater than 0");
    assert(err == 0);

    return js__error(env);
  }

  js_threadsafe_function_t *threadsafe_function = malloc(sizeof(js_threadsafe_function_t));

  threadsafe_function->env = env;
  threadsafe_function->context = context;
  threadsafe_function->finalize_cb = finalize_cb;
  threadsafe_function->finalize_hint = finalize_hint;
  threadsafe_function->cb = cb;

  atomic_init(&threadsafe_function->state, js_threadsafe_function_idle);
  atomic_init(&threadsafe_function->thread_count, initial_thread_count);

  JS_AddGCRef(env->context, &threadsafe_function->function);

  if (function) {
    threadsafe_function->function.val = function->ref.val;
  } else {
    threadsafe_function->function.val = JS_NULL;
  }

  err = uv_async_init(env->loop, &threadsafe_function->async, js__on_threadsafe_function_async);
  assert(err == 0);

  threadsafe_function->async.data = threadsafe_function;

  js_threadsafe_queue_t *queue = &threadsafe_function->queue;

  queue->queue = NULL;
  queue->len = 0;
  queue->capacity = 0;
  queue->head = 0;
  queue->tail = 0;
  queue->closed = false;

  err = uv_mutex_init(&queue->lock);
  assert(err == 0);

  *result = threadsafe_function;

  return 0;
}

int
js_get_threadsafe_function_context(js_threadsafe_function_t *function, void **result) {
  // Allow continuing even with a pending exception

  *result = function->context;

  return 0;
}

int
js_call_threadsafe_function(js_threadsafe_function_t *function, void *data, js_threadsafe_function_call_mode_t mode) {
  if (atomic_load_explicit(&function->thread_count, memory_order_relaxed) == 0) return -1;

  if (js__threadsafe_queue_push(&function->queue, data)) {
    js__threadsafe_function_signal(function);

    return 0;
  }

  return -1;
}

int
js_acquire_threadsafe_function(js_threadsafe_function_t *function) {
  int thread_count = atomic_load_explicit(&function->thread_count, memory_order_relaxed);

  while (thread_count != 0) {
    if (
      atomic_compare_exchange_weak_explicit(
        &function->thread_count,
        &thread_count,
        thread_count + 1,
        memory_order_acquire,
        memory_order_relaxed
      )
    ) {
      return 0;
    }
  }

  return -1;
}

int
js_release_threadsafe_function(js_threadsafe_function_t *function, js_threadsafe_function_release_mode_t mode) {
  int thread_count = atomic_load_explicit(&function->thread_count, memory_order_relaxed);

  bool abort = mode == js_threadsafe_function_abort;

  while (thread_count != 0) {
    if (
      atomic_compare_exchange_weak_explicit(
        &function->thread_count,
        &thread_count,
        abort ? 0 : thread_count - 1,
        memory_order_acquire,
        memory_order_relaxed
      )
    ) {
      if (abort || thread_count == 1) {
        js__threadsafe_queue_close(&function->queue);

        js__threadsafe_function_signal(function);
      }

      return 0;
    }
  }

  return -1;
}

int
js_ref_threadsafe_function(js_env_t *env, js_threadsafe_function_t *function) {
  // Allow continuing even with a pending exception

  uv_ref((uv_handle_t *) &function->async);

  return 0;
}

int
js_unref_threadsafe_function(js_env_t *env, js_threadsafe_function_t *function) {
  // Allow continuing even with a pending exception

  uv_unref((uv_handle_t *) &function->async);

  return 0;
}

int
js_add_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  if (JS_HasException(env->context)) return js__error(env);

  js_teardown_task_t *task = malloc(sizeof(js_teardown_task_t));

  task->type = js_immediate_teardown;
  task->immediate.cb = callback;
  task->data = data;

  intrusive_list_prepend(&env->teardown_queue.tasks, &task->list);

  return 0;
}

int
js_remove_teardown_callback(js_env_t *env, js_teardown_cb callback, void *data) {
  if (JS_HasException(env->context)) return js__error(env);

  if (env->destroying) return 0;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    js_teardown_task_t *task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_immediate_teardown && task->immediate.cb == callback && task->data == data) {
      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      free(task);

      return 0;
    }
  }

  return 0;
}

int
js_add_deferred_teardown_callback(js_env_t *env, js_deferred_teardown_cb callback, void *data, js_deferred_teardown_t **result) {
  if (JS_HasException(env->context)) return js__error(env);

  js_teardown_task_t *task = malloc(sizeof(js_teardown_task_t));

  task->type = js_deferred_teardown;
  task->deferred.cb = callback;
  task->deferred.handle.env = env;
  task->data = data;

  intrusive_list_prepend(&env->teardown_queue.tasks, &task->list);

  env->refs++;

  if (result) *result = &task->deferred.handle;

  return 0;
}

int
js_finish_deferred_teardown_callback(js_deferred_teardown_t *handle) {
  // Allow continuing even with a pending exception

  int err;

  js_env_t *env = handle->env;

  intrusive_list_for_each(next, &env->teardown_queue.tasks) {
    js_teardown_task_t *task = intrusive_entry(next, js_teardown_task_t, list);

    if (task->type == js_deferred_teardown && &task->deferred.handle == handle) {
      intrusive_list_remove(&env->teardown_queue.tasks, &task->list);

      if (--env->refs == 0 && env->destroying) {
        err = uv_async_send(&env->teardown);
        assert(err == 0);
      }

      free(task);

      return 0;
    }
  }

  return -1;
}

int
js_throw(js_env_t *env, js_value_t *error) {
  if (JS_HasException(env->context)) return js__error(env);

  JS_Throw(env->context, error->ref.val);

  return 0;
}

static inline int
js__vformat(char **result, size_t *size, const char *message, va_list args) {
  va_list args_copy;
  va_copy(args_copy, args);

  int res = vsnprintf(NULL, 0, message, args_copy);

  va_end(args_copy);

  if (res < 0) return res;

  *size = res + 1 /* NULL */;
  *result = malloc(*size);

  va_copy(args_copy, args);

  vsnprintf(*result, *size, message, args_copy);

  va_end(args_copy);

  return 0;
}

int
js_throw_error(js_env_t *env, const char *code, const char *message) {
  return 0;
}

int
js_throw_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  size_t len;
  char *formatted;
  err = js__vformat(&formatted, &len, message, args);
  assert(err == 0);

  err = js_throw_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  va_list args;
  va_start(args, message);

  err = js_throw_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_type_error(js_env_t *env, const char *code, const char *message) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_throw_type_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  size_t len;
  char *formatted;
  err = js__vformat(&formatted, &len, message, args);
  assert(err == 0);

  err = js_throw_type_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_type_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  va_list args;
  va_start(args, message);

  err = js_throw_type_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_range_error(js_env_t *env, const char *code, const char *message) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_throw_range_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  size_t len;
  char *formatted;
  err = js__vformat(&formatted, &len, message, args);
  assert(err == 0);

  err = js_throw_range_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_range_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  va_list args;
  va_start(args, message);

  err = js_throw_range_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_syntax_error(js_env_t *env, const char *code, const char *message) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_throw_syntax_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  size_t len;
  char *formatted;
  err = js__vformat(&formatted, &len, message, args);
  assert(err == 0);

  err = js_throw_syntax_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_syntax_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  va_list args;
  va_start(args, message);

  err = js_throw_syntax_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_throw_reference_error(js_env_t *env, const char *code, const char *message) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_throw_reference_verrorf(js_env_t *env, const char *code, const char *message, va_list args) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  size_t len;
  char *formatted;
  err = js__vformat(&formatted, &len, message, args);
  assert(err == 0);

  err = js_throw_reference_error(env, code, formatted);

  free(formatted);

  return err;
}

int
js_throw_reference_errorf(js_env_t *env, const char *code, const char *message, ...) {
  if (JS_HasException(env->context)) return js__error(env);

  int err;

  va_list args;
  va_start(args, message);

  err = js_throw_reference_verrorf(env, code, message, args);

  va_end(args);

  return err;
}

int
js_is_exception_pending(js_env_t *env, bool *result) {
  // Allow continuing even with a pending exception

  *result = JS_HasException(env->context);

  return 0;
}

int
js_get_and_clear_last_exception(js_env_t *env, js_value_t **result) {
  // Allow continuing even with a pending exception

  JSValue error = JS_GetException(env->context);

  if (JS_IsUninitialized(error)) return js_get_undefined(env, result);

  js_value_t *wrapper = malloc(sizeof(js_value_t));

  JS_PushGCRef(env->context, &wrapper->ref);

  wrapper->ref.val = error;

  *result = wrapper;

  js__attach_to_handle_scope(env, env->scope, wrapper);

  return 0;
}

int
js_fatal_exception(js_env_t *env, js_value_t *error) {
  // Allow continuing even with a pending exception

  js__uncaught_exception(env, error->ref.val);

  return 0;
}

int
js_terminate_execution(js_env_t *env) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_adjust_external_memory(js_env_t *env, int64_t change_in_bytes, int64_t *result) {
  // Allow continuing even with a pending exception

  env->external_memory += change_in_bytes;

  if (result) *result = env->external_memory;

  return 0;
}

int
js_request_garbage_collection(js_env_t *env) {
  // Allow continuing even with a pending exception

  if (env->platform->options.expose_garbage_collection) {
    JS_GC(env->context);
  }

  return 0;
}

int
js_get_heap_statistics(js_env_t *env, js_heap_statistics_t *result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_get_heap_space_statistics(js_env_t *env, js_heap_space_statistics_t statistics[], size_t len, size_t offset, size_t *result) {
  if (result) *result = 0;

  return 0;
}

int
js_create_inspector(js_env_t *env, js_inspector_t **result) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_destroy_inspector(js_env_t *env, js_inspector_t *inspector) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_on_inspector_response(js_env_t *env, js_inspector_t *inspector, js_inspector_message_cb cb, void *data) {
  return 0;
}

int
js_on_inspector_response_transitional(js_env_t *env, js_inspector_t *inspector, js_inspector_message_transitional_cb cb, void *data) {
  return 0;
}

int
js_on_inspector_paused(js_env_t *env, js_inspector_t *inspector, js_inspector_paused_cb cb, void *data) {
  return 0;
}

int
js_connect_inspector(js_env_t *env, js_inspector_t *inspector) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_send_inspector_request(js_env_t *env, js_inspector_t *inspector, js_value_t *message) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_send_inspector_request_transitional(js_env_t *env, js_inspector_t *inspector, const char *message, size_t len) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_attach_context_to_inspector(js_env_t *env, js_inspector_t *inspector, js_context_t *context, const char *name, size_t len) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}

int
js_detach_context_from_inspector(js_env_t *env, js_inspector_t *inspector, js_context_t *context) {
  int err;

  err = js_throw_error(env, NULL, "Unsupported operation");
  assert(err == 0);

  return js__error(env);
}
