#pragma once

#define JEMU_VERSION_MAJOR 0
#define JEMU_VERSION_MINOR 1
#define JEMU_VERSION_PATCH 0
#define JEMU_VERSION_STR   "0.1.0"

#define JEMU_UNUSED(x) ((void)(x))

#define JEMU_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __GNUC__
#  define JEMU_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define JEMU_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define JEMU_PACKED      __attribute__((packed))
#else
#  define JEMU_LIKELY(x)   (x)
#  define JEMU_UNLIKELY(x) (x)
#  define JEMU_PACKED
#endif
