#ifndef HOST_ZEPHYR_UTIL_H
#define HOST_ZEPHYR_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define BIT(n) (1UL << (n))
#define BUILD_ASSERT(condition) _Static_assert(condition, #condition)
#define IS_ENABLED(config) (config)
#define IS_POWER_OF_TWO(n) ((n) && !((n) & ((n) - 1)))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ROUND_UP(n, alignment) (((n) + (alignment) - 1U) & ~((alignment) - 1U))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1U) / (d))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define ARG_UNUSED(value) ((void)(value))

#endif
