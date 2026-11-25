#ifndef WCAM_SOURCE_H
#define WCAM_SOURCE_H

#include <stdint.h>

#define WCAM_REMOTE_TIMEOUT_MS 250u

struct wcam_source {
    uint64_t last_remote_ms;
    uint64_t earliest_idr_ms;
    int remote;
};

static inline void wcam_source_start(struct wcam_source *s, uint64_t now)
{
    s->last_remote_ms = now;
    s->earliest_idr_ms = now;
    s->remote = 0;
}

static inline int wcam_source_due(const struct wcam_source *s, uint64_t now)
{
    return s->remote && now - s->last_remote_ms >= WCAM_REMOTE_TIMEOUT_MS;
}

static inline int wcam_source_expired(struct wcam_source *s, uint64_t now)
{
    if (!wcam_source_due(s, now)) return 0;
    /* A queued IDR stamped in the transition tick could predate fallback. */
    s->earliest_idr_ms = now + 1;
    s->remote = 0;
    return 1;
}

static inline int wcam_source_accept(struct wcam_source *s, int idr,
                                     uint64_t arrived, uint64_t now)
{
    /* Test continuity on the receive clock. An on-time frame can still be
     * queued behind USB, but must itself be fresh enough to display. */
    if (now - arrived >= WCAM_REMOTE_TIMEOUT_MS || (!idr && wcam_source_due(s, arrived)))
        return 0;
    if (!s->remote && (!idr || arrived < s->earliest_idr_ms)) return 0;
    s->remote = 1;
    s->last_remote_ms = arrived;
    return 1;
}

static inline int wcam_session_current(uint32_t payload_generation,
                                       uint32_t stream_generation,
                                       int streaming, int disconnected)
{
    return streaming && !disconnected && payload_generation == stream_generation;
}

#endif
