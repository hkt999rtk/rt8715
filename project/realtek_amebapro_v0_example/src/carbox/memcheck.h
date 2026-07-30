/*
 * memcheck.h — 内存泄漏/越界检测工具头文件
 *
 * 适配目标: Realtek AmebaPro SDK (FreeRTOS)
 */

#ifndef CARBOX_MEMCHECK_H
#define CARBOX_MEMCHECK_H

#include "basic_types.h"   /* u32, s32 */
#include <stddef.h>        /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 内存分配（带越界检测和泄漏追踪）
 *
 * 行为等同于 pvPortMalloc，额外在前后写入魔数并记录到哈希表。
 * 返回的指针偏移 TAG_LEN 字节，指向用户可用区域。
 *
 * @param size  用户请求的字节数
 * @return      用户数据区指针，失败返回 NULL
 */
void *memcheck_malloc(size_t size);

/**
 * @brief 内存释放（带越界检测）
 *
 * 检测前后魔数是否被破坏，从哈希表移除记录，然后释放内存。
 *
 * @param ptr  memcheck_malloc 返回的指针，NULL 时直接返回
 */
void memcheck_free(void *ptr);

/**
 * @brief checkmem shell 命令入口
 *
 * 通过 shell_register() 注册为 "checkmem" 命令。
 * 签名匹配 shell_program_t: s32 (*)(u32, char **)
 *
 * 用法:
 *   checkmem          首次输入启动检测，之后打印哈希表
 *   checkmem 1         启用检测
 *   checkmem 0         暂停/恢复检测
 *   checkmem <N>       仅显示大小为 N 的分配记录
 */
s32 memcheck_main(u32 argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* CARBOX_MEMCHECK_H */
