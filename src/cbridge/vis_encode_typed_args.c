/*
 * vis_encode_typed_args — encodes extracted JNI arguments into a wire format
 * that Go's decodeArgs() deserializes without heuristic string matching.
 *
 * Wire format per argument:
 *   sigChar \x01 primaryValue [ \x03 extraValue ] \x02
 *
 * sigChar    — JNI descriptor prefix: Z B C S I J F D L [
 * primary    — decimal int for primitives; UTF-8 for strings; class/Name for objects
 * extra      — toString() for objects (only when \x03 is present)
 *
 * Add this function to visualize.c and declare it in visualize.h.
 */

#include "visualize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Grow a heap buffer by doubling when needed. Returns new pointer. */
static char *buf_append(char *buf, size_t *len, size_t *cap, const char *src, size_t n) {
    while (*len + n + 1 > *cap) {
        *cap = (*cap < 128) ? 256 : (*cap * 2);
        buf = realloc(buf, *cap);
    }
    memcpy(buf + *len, src, n);
    *len += n;
    buf[*len] = '\0';
    return buf;
}

#define APPEND_LIT(b, l, c, s) buf_append(b, l, c, s, strlen(s))
#define APPEND_CH(b, l, c, ch) do { char _ch = (ch); b = buf_append(b, l, c, &_ch, 1); } while(0)

/*
 * vis_encode_typed_args — encodes up to `count` extracted args from `extracted`.
 * `sig` is the full JNI method descriptor including surrounding parens.
 * Returns a heap-allocated string; caller must free().
 */
char* vis_encode_typed_args(JNIEnv *env, const char *sig, uintptr_t *extracted, int count) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    if (!sig || count <= 0) return buf;

    const char *p = sig;
    if (*p == '(') p++;  /* skip opening paren */

    char tmp[64];

    for (int i = 0; i < count && *p && *p != ')'; i++) {
        char kind_ch = *p;

        /* ── Advance sig pointer past this type ───────────────────── */
        if (kind_ch == 'L') {
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
        } else if (kind_ch == '[') {
            p++;  /* skip '[' */
            while (*p == '[') p++;
            if (*p == 'L') {
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
            } else {
                p++;
            }
        } else {
            p++;
        }

        uintptr_t val = extracted[i];

        /* ── Emit: sigChar \x01 value [\x03 extra] \x02 ─────────────── */
        APPEND_CH(buf, &len, &cap, kind_ch);
        APPEND_CH(buf, &len, &cap, '\x01');

        switch (kind_ch) {
        case 'Z':
            snprintf(tmp, sizeof(tmp), "%d", (int)(val & 1));
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        case 'B':
            snprintf(tmp, sizeof(tmp), "%d", (int)(int8_t)val);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        case 'C':
            snprintf(tmp, sizeof(tmp), "%u", (unsigned)(uint16_t)val);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        case 'S':
            snprintf(tmp, sizeof(tmp), "%d", (int)(int16_t)val);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        case 'I':
            snprintf(tmp, sizeof(tmp), "%d", (int)(int32_t)val);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        case 'J':
            snprintf(tmp, sizeof(tmp), "%lld", (long long)(int64_t)val);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        case 'F': {
            float fv;
            uint32_t u32 = (uint32_t)val;
            memcpy(&fv, &u32, sizeof(fv));
            snprintf(tmp, sizeof(tmp), "%g", (double)fv);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        }
        case 'D': {
            double dv;
            memcpy(&dv, &val, sizeof(dv));
            snprintf(tmp, sizeof(tmp), "%g", dv);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        }
        case '[': {
            if (val == 0 || !env) {
                buf = APPEND_LIT(buf, &len, &cap, "");
            } else {
                char elemSig = (*p == 'L' || *p == '[') ? 'L' : *p;
                char *items = vis_encode_array_items(env, (void *)val, elemSig);
                size_t ilen = items ? strlen(items) : 0;
                APPEND_CH(buf, &len, &cap, '\x01');
                if (items && ilen > 0) buf = buf_append(buf, &len, &cap, items, ilen);
                free(items);
                APPEND_CH(buf, &len, &cap, '\x02');
                kind_ch = 0; /* suppress trailing \x02 in outer APPEND_CH below */
            }
            break;
        }
        case 'L': {
            if (val == 0 || !env) {
                /* null — empty primary, no extra */
                break;
            }
            void *obj = (void *)val;
            if (vis_is_string(env, obj)) {
                buf[len - 2] = 's';
                char *sv = vis_string_value_raw(env, obj);
                if (sv) {
                    buf = buf_append(buf, &len, &cap, sv, strlen(sv));
                    free(sv);
                }
            } else if (vis_is_class(env, obj)) {
                buf[len - 2] = 'c'; /* 'c' = jclass */
                char *cn = vis_class_name(env, obj);
                if (cn) {
                    buf = buf_append(buf, &len, &cap, cn, strlen(cn));
                    free(cn);
                }
            } else {
                /* Generic object: primary = class name, extra = toString */
                char *cn = vis_object_class_name(env, obj);
                char *ts = vis_object_tostring(env, obj);
                if (cn) buf = buf_append(buf, &len, &cap, cn, strlen(cn));
                if (ts) {
                    APPEND_CH(buf, &len, &cap, '\x03');
                    buf = buf_append(buf, &len, &cap, ts, strlen(ts));
                    free(ts);
                }
                free(cn);
            }
            break;
        }
        default:
            snprintf(tmp, sizeof(tmp), "0x%lx", (unsigned long)val);
            buf = APPEND_LIT(buf, &len, &cap, tmp);
            break;
        }

        if (kind_ch != 0) APPEND_CH(buf, &len, &cap, '\x02');
    }

    return buf;
}
