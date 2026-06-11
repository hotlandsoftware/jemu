#pragma once

#define GEMU_VERSION_MAJOR 0
#define GEMU_VERSION_MINOR 1
#define GEMU_VERSION_PATCH 0
#define GEMU_VERSION_STR   "0.1.0"

#define GEMU_UNUSED(x) ((void)(x))

#define GEMU_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __GNUC__
#  define GEMU_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define GEMU_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define GEMU_PACKED      __attribute__((packed))
#else
#  define GEMU_LIKELY(x)   (x)
#  define GEMU_UNLIKELY(x) (x)
#  define GEMU_PACKED
#endif
