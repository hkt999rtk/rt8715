#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "wcam_source.h"

static void test_decoder_entry(void)
{
    struct wcam_source s;
    wcam_source_start(&s, 100);
    assert(!s.remote);
    assert(!wcam_source_accept(&s, 0, 101, 101));
    assert(!wcam_source_accept(&s, 1, 99, 102));
    /* Network activation follows source_start and stamps the new generation,
     * even if the first fresh IDR finishes in the very same tick. */
    assert(wcam_source_accept(&s, 1, 100, 102));
    assert(wcam_source_accept(&s, 0, 130, 130));
    assert(!wcam_source_expired(&s, 379));
    assert(wcam_source_expired(&s, 380));
    assert(!wcam_source_accept(&s, 1, 379, 381));
    assert(!wcam_source_accept(&s, 1, 380, 381));
    assert(wcam_source_accept(&s, 1, 381, 381));
}

static void test_delayed_consumer(void)
{
    struct wcam_source s;
    wcam_source_start(&s, 10);
    assert(wcam_source_accept(&s, 1, 20, 200));
    assert(!wcam_source_expired(&s, 269));
    assert(wcam_source_expired(&s, 270));
    assert(!wcam_source_accept(&s, 1, 275, 525));
    assert(wcam_source_accept(&s, 1, 530, 530));
    /* USB held the preceding frame until 790. The frame received at 740
     * maintained stream continuity even though the gap between submits is 260. */
    assert(wcam_source_accept(&s, 0, 740, 790));
    assert(!wcam_source_expired(&s, 989));
    assert(!wcam_source_accept(&s, 0, 990, 990));
    assert(wcam_source_expired(&s, 990));
    assert(!wcam_source_accept(&s, 0, 991, 991));
    assert(wcam_source_accept(&s, 1, 991, 991));
    /* A fresh complete IDR can restart the decoder after a gap on its own. */
    assert(wcam_source_accept(&s, 1, 1300, 1305));
}

static void test_long_wait_and_wrap(void)
{
    struct wcam_source s;
    const uint64_t wrap = UINT64_C(1) << 32;
    wcam_source_start(&s, 10);
    assert(wcam_source_accept(&s, 1, 0x80000020u, 0x80000021u));
    wcam_source_start(&s, wrap - 11);
    assert(wcam_source_accept(&s, 1, wrap - 6, wrap + 100));
    assert(!wcam_source_expired(&s, wrap + 243));
    assert(wcam_source_expired(&s, wrap + 244));
    assert(!wcam_source_accept(&s, 1, wrap + 245, 3 * wrap + 245));
    assert(wcam_source_accept(&s, 1, 3 * wrap + 245, 3 * wrap + 246));
}

static void test_session_gate(void)
{
    assert(wcam_session_current(7, 7, 1, 0));
    assert(!wcam_session_current(7, 7, 0, 0));
    assert(!wcam_session_current(7, 7, 1, 1));
    /* A stop/start completed while the local encoder or remote queue was busy. */
    assert(!wcam_session_current(7, 8, 1, 0));
}

int main(void)
{
    test_decoder_entry();
    test_delayed_consumer();
    test_long_wait_and_wrap();
    test_session_gate();
    puts("source: IDR entry, arrival deadlines, long idle, tick wrap, session gate PASS");
    return 0;
}
