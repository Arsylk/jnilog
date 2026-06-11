//go:build android

/* freestanding-rev: 18  (bump this counter on any edit under freestanding/ to
 * bust go's cgo build cache, which does not always track these
 * transitively-included headers) */

/* Freestanding C-bridge shim — MUST precede the bridge bodies. It pulls in all
 * system headers, then the freestanding primitives and per-phase libc
 * redirect macros, so those macros only ever rewrite our own call sites (the
 * bodies' re-includes of <string.h> etc. are guarded no-ops). See
 * src/cbridge/freestanding/jl_libc.h. */
#include "../cbridge/freestanding/jl_libc.h"

#include "../cbridge/event_pipe.c"
#include "../cbridge/bridge.c"
#include "../cbridge/hook_common.c"
#include "../cbridge/hook_fields.c"
#include "../cbridge/hook_logging.c"
#include "../cbridge/hook_methods.c"
#include "../cbridge/hooks.c"
#include "../cbridge/main.c"
#include "../cbridge/rangeset.c"
#include "../cbridge/visualize.c"
