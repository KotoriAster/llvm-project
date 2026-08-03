// Trace the libdispatch work used by Apple's new linker.
//
// Build:
//   xcrun clang -dynamiclib -fblocks -O2 trace_ldprime_input.c \
//     -o /tmp/trace_ldprime_input.dylib
// Add -DLDPRIME_TRACE_EXTENDED=1 to also trace blocks submitted to the
// serial "com.apple.ld.combiner" queue.
//
// Run:
//   LDPRIME_TRACE_FILE=/tmp/ldprime.jsonl \
//   LDPRIME_TRACE_CALLER_OFFSET=0x107858 \
//   DYLD_INSERT_LIBRARIES=/tmp/trace_ldprime_input.dylib \
//     xcrun ld <normal linker arguments>
//
// 0x107858 is the parseFiles dispatch_apply return address in Xcode 26.3's
// ld-1230.1. Omit the filter on another linker version, find the corresponding
// callsite in the JSONL output, and rerun with that value.
//
// The output is newline-delimited JSON. Each selected dispatch_apply item and,
// in extended mode, each combiner block has begin/end events with a monotonic
// timestamp and kernel thread id. The callsite image and offset distinguish
// input parsing from other parallel linker stages.

#define _DARWIN_C_SOURCE

#include <Block.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int trace_fd = STDERR_FILENO;
static mach_timebase_info_data_t timebase;
static _Atomic uint64_t next_event_id = 1;
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static uintptr_t trace_caller_offset;

static void initialize_trace_once(void) {
  (void)mach_timebase_info(&timebase);
  const char *path = getenv("LDPRIME_TRACE_FILE");
  if (path && path[0]) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd >= 0)
      trace_fd = fd;
  }
  const char *offset = getenv("LDPRIME_TRACE_CALLER_OFFSET");
  if (offset && offset[0])
    trace_caller_offset = (uintptr_t)strtoull(offset, NULL, 0);
}

static uint64_t timestamp_ns(void) {
  (void)pthread_once(&trace_once, initialize_trace_once);
  uint64_t ticks = mach_continuous_time();
  return ticks * timebase.numer / timebase.denom;
}

static uint64_t thread_id(void) {
  uint64_t tid = 0;
  (void)pthread_threadid_np(NULL, &tid);
  return tid;
}

static const char *queue_label(dispatch_queue_t queue) {
  return queue ? dispatch_queue_get_label(queue) : "DISPATCH_APPLY_AUTO";
}

static void describe_address(const void *address, const char **image,
                             uintptr_t *offset) {
  Dl_info info = {0};
  if (address && dladdr(address, &info) && info.dli_fbase) {
    *image = info.dli_fname ? info.dli_fname : "";
    *offset = (uintptr_t)address - (uintptr_t)info.dli_fbase;
    return;
  }
  *image = "";
  *offset = 0;
}

static void emit_event(const char *kind, const char *phase, uint64_t id,
                       size_t index, size_t count, const char *queue,
                       const char *image, uintptr_t caller_offset) {
  char buffer[1536];
  int length = snprintf(
      buffer, sizeof(buffer),
      "{\"ts_ns\":%llu,\"tid\":%llu,\"ph\":\"%s\",\"kind\":\"%s\","
      "\"id\":%llu,\"index\":%zu,\"count\":%zu,\"queue\":\"%s\","
      "\"image\":\"%s\",\"caller_offset\":\"0x%llx\"}\n",
      (unsigned long long)timestamp_ns(), (unsigned long long)thread_id(),
      phase, kind, (unsigned long long)id, index, count, queue ? queue : "",
      image ? image : "", (unsigned long long)caller_offset);
  if (length > 0) {
    size_t bytes = (size_t)length < sizeof(buffer) ? (size_t)length
                                                   : sizeof(buffer) - 1;
    (void)write(trace_fd, buffer, bytes);
  }
}

__attribute__((constructor, used)) static void initialize_trace(void) {
  (void)pthread_once(&trace_once, initialize_trace_once);
}

__attribute__((destructor, used)) static void finish_trace(void) {
  if (trace_fd != STDERR_FILENO)
    (void)close(trace_fd);
}

struct apply_context {
  void (^block)(size_t);
  uint64_t id;
  size_t iterations;
  const char *label;
  const char *image;
  uintptr_t offset;
};

struct passthrough_apply_context {
  void (^block)(size_t);
};

static void invoke_passthrough_apply_item(void *opaque, size_t index) {
  struct passthrough_apply_context *context = opaque;
  context->block(index);
}

static void invoke_apply_item(void *opaque, size_t index) {
  struct apply_context *context = opaque;
  emit_event("dispatch_apply_item", "B", context->id, index,
             context->iterations, context->label, context->image,
             context->offset);
  context->block(index);
  emit_event("dispatch_apply_item", "E", context->id, index,
             context->iterations, context->label, context->image,
             context->offset);
}

static void traced_dispatch_apply(size_t iterations, dispatch_queue_t queue,
                                  void (^block)(size_t)) {
  const void *caller = __builtin_return_address(0);
  const char *image;
  uintptr_t offset;
  describe_address(caller, &image, &offset);
  (void)pthread_once(&trace_once, initialize_trace_once);
  if (trace_caller_offset && trace_caller_offset != offset) {
    struct passthrough_apply_context context = {block};
    dispatch_apply_f(iterations, queue, &context, invoke_passthrough_apply_item);
    return;
  }
  uint64_t id =
      atomic_fetch_add_explicit(&next_event_id, 1, memory_order_relaxed);
  const char *label = queue_label(queue);

  emit_event("dispatch_apply", "S", id, 0, iterations, label, image, offset);
  struct apply_context context = {block, id, iterations, label, image, offset};
  dispatch_apply_f(iterations, queue, &context, invoke_apply_item);
  emit_event("dispatch_apply", "F", id, 0, iterations, label, image, offset);
}

struct group_async_context {
  dispatch_block_t block;
  uint64_t id;
  const char *label;
  const char *image;
  uintptr_t offset;
  int should_trace;
};

static void invoke_group_async(void *opaque) {
  struct group_async_context *context = opaque;
  if (context->should_trace)
    emit_event("dispatch_group_async", "B", context->id, 0, 0, context->label,
               context->image, context->offset);
  context->block();
  if (context->should_trace)
    emit_event("dispatch_group_async", "E", context->id, 0, 0, context->label,
               context->image, context->offset);
  Block_release(context->block);
  free(context);
}

__attribute__((unused)) static void
traced_dispatch_group_async(dispatch_group_t group, dispatch_queue_t queue,
                            dispatch_block_t block) {
  struct group_async_context *context = malloc(sizeof(*context));
  if (!context)
    abort();

  const void *caller = __builtin_return_address(0);
  describe_address(caller, &context->image, &context->offset);
  context->id =
      atomic_fetch_add_explicit(&next_event_id, 1, memory_order_relaxed);
  context->label = queue_label(queue);
  context->should_trace =
      strcmp(context->label, "com.apple.ld.combiner") == 0;
  context->block = Block_copy(block);

  if (context->should_trace)
    emit_event("dispatch_group_async", "S", context->id, 0, 0, context->label,
               context->image, context->offset);
  dispatch_group_async_f(group, queue, context, invoke_group_async);
}

#define DYLD_INTERPOSE(replacement, replacee)                                  \
  __attribute__((used)) static struct {                                        \
    const void *replacement;                                                   \
    const void *replacee;                                                      \
  } _interpose_##replacee __attribute__((section("__DATA,__interpose"))) = {   \
      (const void *)(uintptr_t)&replacement,                                   \
      (const void *)(uintptr_t)&replacee}

DYLD_INTERPOSE(traced_dispatch_apply, dispatch_apply);
#ifdef LDPRIME_TRACE_EXTENDED
DYLD_INTERPOSE(traced_dispatch_group_async, dispatch_group_async);
#endif
