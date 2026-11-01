#ifndef TEST_CC_H
#define TEST_CC_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, x); abort(); } while (0)
#define LWIP_RAND() ((uint32_t)1)
#endif
