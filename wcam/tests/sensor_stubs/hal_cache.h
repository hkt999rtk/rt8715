#include <stdint.h>
void dcache_clean_invalidate_by_addr(uint32_t *addr, unsigned bytes);
void dcache_invalidate_by_addr(uint32_t *addr, unsigned bytes);
