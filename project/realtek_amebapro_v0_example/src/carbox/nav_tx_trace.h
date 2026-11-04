#ifndef CARBOX_NAV_TX_TRACE_H
#define CARBOX_NAV_TX_TRACE_H
#include <stdint.h>
#include <stddef.h>
#ifndef CONFIG_AUDIO_NAV_TX_TRACE
#define CONFIG_AUDIO_NAV_TX_TRACE 0
#endif
/* Stamps are wrapping HAL us, valid for short intervals (<2^31 us). */
enum navtx_stage { NT_ENTER, NT_COPY, NT_WAIT, NT_LOCK, NT_CORE_END,
    NT_ENQUEUE, NT_DEQUEUE, NT_BUILD, NT_READY, NT_USB, NT_HAL_END, NT_RETURN, NT_STAGES };
#if CONFIG_AUDIO_NAV_TX_TRACE
uint32_t navtx_begin(uint32_t epoch, const void *rtp, size_t len,
    uint32_t q_us, uint32_t debt_us);
void navtx_end(uint32_t id, int result, int error);
void navtx_prepared(const void *pbuf);
void navtx_core_wait(void);
void navtx_core_acquired(uintptr_t caller);
void navtx_core_release(void);
uint32_t navtx_ncm(const void *pbuf, uint32_t depth);
void navtx_mark(uint32_t id, unsigned stage);
void navtx_transport_end(uint32_t id, int status);
void navtx_decode_context(uint32_t session, uint32_t ts, unsigned plc);
void navtx_fdk_state(int before, int after, int frame_ok, int samples);
void navtx_report(void);
#else
#define navtx_begin(e,p,l,q,d) (0U)
#define navtx_end(i,r,e) ((void)0)
#define navtx_prepared(p) ((void)0)
#define navtx_core_wait() ((void)0)
#define navtx_core_acquired(c) ((void)0)
#define navtx_core_release() ((void)0)
#define navtx_ncm(p,d) (0U)
#define navtx_mark(i,s) ((void)0)
#define navtx_transport_end(i,s) ((void)0)
#define navtx_decode_context(s,t,p) ((void)0)
#define navtx_fdk_state(b,a,f,s) ((void)0)
#define navtx_report() ((void)0)
#endif
#endif
