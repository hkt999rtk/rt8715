#ifndef SCREEN_FLOW_EVIDENCE_H
#define SCREEN_FLOW_EVIDENCE_H
#include <stdint.h>
#include <stddef.h>
/* stage: 0=入队入口，1=进入原厂帧处理，2=观测到未进入处理便释放。
 * stage2不是已证明的丢包原因；可能是SPS筛选、停止清理或入队拒绝。 */
void carbox_screen_flow_frame(unsigned stage,const void *data,size_t length);
void carbox_screen_flow_report(uint32_t sequence);
#endif
