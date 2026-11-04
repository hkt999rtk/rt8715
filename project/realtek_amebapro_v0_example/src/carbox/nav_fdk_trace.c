/* Pinned FDK v0.1.6 function ABI. Opaque pointers, no private-struct offsets.
 * Preserve native args/result and only observe concealment state transitions. */
#include "nav_tx_trace.h"
#if CONFIG_AUDIO_NAV_TX_TRACE
extern int native_apply(void *,void *,void *,const void *,int,unsigned char,int,unsigned)
    __asm__("__real__Z18CConcealment_ApplyP16CConcealmentInfoP22CAacDecoderChannelInfoP28CAacDecoderStaticChannelInfoPK16SamplingRateInfoihij");
extern int conceal_state(void *) __asm__("_Z21CConcealment_GetStateP16CConcealmentInfo");
int traced_apply(void *,void *,void *,const void *,int,unsigned char,int,unsigned)
    __asm__("__wrap__Z18CConcealment_ApplyP16CConcealmentInfoP22CAacDecoderChannelInfoP28CAacDecoderStaticChannelInfoPK16SamplingRateInfoihij");
int traced_apply(void *c,void *ch,void *st,const void *sr,int n,unsigned char lpd,int ok,unsigned flags)
{
    int before=conceal_state(c);
    int result=native_apply(c,ch,st,sr,n,lpd,ok,flags);
    navtx_fdk_state(before,conceal_state(c),ok,n);
    return result;
}
#endif
