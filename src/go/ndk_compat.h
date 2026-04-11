/*
 * ndk_compat.h - GCC compatibility shim for Android NDK headers
 *
 * The NDK sysroot headers use Clang-specific nullability qualifiers
 * (_Nonnull, _Nullable) and availability attributes (__BIONIC_AVAILABILITY)
 * that GCC does not understand. This header defines them as no-ops so that
 * the host GCC (used by gopls / cgo on the development machine) can parse
 * the NDK headers without errors.
 *
 * When building with the actual NDK Clang cross-compiler, these are already
 * defined natively, so the #ifndef guards make this header harmless.
 */

#ifndef __clang__

/* Nullability qualifiers — Clang-only, make them vanish under GCC */
#ifndef _Nonnull
#define _Nonnull
#endif

#ifndef _Nullable
#define _Nullable
#endif

#ifndef _Null_unspecified
#define _Null_unspecified
#endif

/* Bionic availability / versioning macros */
#ifdef __BIONIC_AVAILABILITY
#undef __BIONIC_AVAILABILITY
#endif
#define __BIONIC_AVAILABILITY(...) __attribute__((unused))

#ifdef __INTRODUCED_IN
#undef __INTRODUCED_IN
#endif
#define __INTRODUCED_IN(x) __attribute__((unused))

#ifdef __DEPRECATED_IN
#undef __DEPRECATED_IN
#endif
#define __DEPRECATED_IN(x, ...) __attribute__((unused))

#ifdef __REMOVED_IN
#undef __REMOVED_IN
#endif
#define __REMOVED_IN(x, ...) __attribute__((unused))

/* __nodiscard — GCC < 10 may not support [[nodiscard]] via this spelling */
#ifndef __nodiscard
#define __nodiscard
#endif

#endif /* !__clang__ */
