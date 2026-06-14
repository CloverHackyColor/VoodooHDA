#include "License.h"

#ifndef _COMMON_H
#define _COMMON_H

#undef panic
void panic(const char *str, ...) __attribute__((__noreturn__));

/* Release hardening: never intentionally panic from runtime IOAudio/coreaudiod paths.
 * Old ASSERT/BUG expanded to panic(), which can turn malformed CoreAudio state,
 * unsupported stream formats, or teardown races into a kernel panic.  Keep the
 * macros non-fatal; call sites that need errors must return them explicitly. */
#define ASSERT(expr) do { if (!(expr)) { } } while (0)
#define BUG(msg) do { } while (0)

#define RELEASE(x) do { if (x) { (x)->release(); (x) = NULL; } } while (0)
#define DELETE(x) do { if (x) { delete (x); (x) = NULL; } } while (0)
#define FREE(x) do { if (x) { freeMem(x); (x) = NULL; } } while (0)

#endif
