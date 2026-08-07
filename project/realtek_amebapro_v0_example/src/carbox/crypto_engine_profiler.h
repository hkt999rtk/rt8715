#ifndef CARBOX_CRYPTO_ENGINE_PROFILER_H
#define CARBOX_CRYPTO_ENGINE_PROFILER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void crypto_engine_profiler_report(uint32_t sequence);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_CRYPTO_ENGINE_PROFILER_H */
