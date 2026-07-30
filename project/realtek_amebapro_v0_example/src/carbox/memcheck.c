/*
 * memcheck.c — 内存泄漏/越界检测工具
 *
 * 原始来源: f133 平台 (RT-Thread + pthread)
 * 适配目标: Realtek AmebaPro SDK (FreeRTOS)
 *
 * 设计思路:
 *
 *  内存泄漏:
 *    1. 使用 memcheck_malloc/memcheck_free 替代标准 malloc/free 来记录每次分配
 *    2. 申请内存时将指针作为 key 存入哈希表，记录分配时间、线程ID等
 *    3. 释放时从哈希表移除相应条目
 *    4. 通过 checkmem 命令打印哈希表，留存时间越长的越可能是泄漏
 *
 *  内存越界:
 *    1. 每次申请内存时前后多申请 TAG_LEN 字节（8字节）
 *       |tag8B|==========================user data============================|tag8B|
 *    2. 前后8字节写入魔数 (0xDDCCBBAA88664422)
 *    3. 释放时检测魔数是否被破坏
 *
 *  使用方法:
 *    - 在需要监控的模块中，使用 memcheck_malloc(size) / memcheck_free(ptr)
 *      替代标准 malloc / free
 *    - 或者定义 CARBOX_MEMCHECK_WRAP_GLOBAL 宏，在编译时将全局 malloc/free
 *      映射为 memcheck 版本
 *    - 串口输入 checkmem 启动检测，再次输入打印哈希表
 *    - checkmem <num> 仅显示指定大小的分配记录
 *    - checkmem 0 暂停/恢复检测
 *
 *  shell 命令注册（在 main.c 初始化中调用）:
 *   shell_register(memcheck_main, "checkmem", "memcheck tool");
 */

/* ---- SDK 头文件 ---- */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "diag.h"           /* rt_printf */
#include "basic_types.h"    /* u64, u32, s32, BOOL */

/* ---- C 标准库 ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- 配置 ---- */
#define TABLE_SIZE              128
#define BACKTRACE_BUFLEN        1024
#define TAG_LEN                 (sizeof(void*))
#define TAG_LEN2                (TAG_LEN * 2)

/* 内存越界检测魔数 */
static const u64 mem_tag = 0xDDCCBBAA88664422ULL;

/* 是否启用 backtrace（AmebaPro 上默认不支持，保留为扩展点） */
#ifdef CARBOX_MEMCHECK_ENABLE_BACKTRACE
#include <backtrace.h>
#endif

/* ------------------------------------------------------------------ */
/* 哈希表结构                                                         */
/* ------------------------------------------------------------------ */

struct Node {
    u64             key;         /* 内存指针 */
    void           *value;       /* 分配大小 */
    u64             tk;          /* 申请时间 (tick) */
    TaskHandle_t    tid;         /* 任务句柄 */
    char           *backtrace;   /* 函数调用栈（可选） */
    struct Node    *next;
};

struct HashTable {
    struct Node **table;
};

/* ------------------------------------------------------------------ */
/* 工具函数                                                           */
/* ------------------------------------------------------------------ */

static u64 GetTickCount(void)
{
    return (u64)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static struct Node *createNode(u64 key, void *value)
{
    struct Node *newNode = (struct Node *)pvPortMalloc(sizeof(struct Node));
    if (newNode == NULL) {
        return NULL;
    }
    newNode->key       = key;
    newNode->value     = value;
    newNode->next      = NULL;
    newNode->tk        = GetTickCount();
    newNode->tid       = xTaskGetCurrentTaskHandle();
    newNode->backtrace = NULL;
    return newNode;
}

static void freeNode(struct Node *node)
{
    if (node) {
        if (node->backtrace) {
            vPortFree(node->backtrace);
            node->backtrace = NULL;
        }
        vPortFree(node);
    }
}

static struct HashTable *createHashTable(void)
{
    struct HashTable *hashTable;
    int i;

    hashTable = (struct HashTable *)pvPortMalloc(sizeof(struct HashTable));
    if (hashTable == NULL) {
        return NULL;
    }
    hashTable->table = (struct Node **)pvPortMalloc(TABLE_SIZE * sizeof(struct Node *));
    if (hashTable->table == NULL) {
        vPortFree(hashTable);
        return NULL;
    }
    for (i = 0; i < TABLE_SIZE; i++) {
        hashTable->table[i] = NULL;
    }
    return hashTable;
}

static int hashFunction(u64 key)
{
    return (int)((key / TAG_LEN) % TABLE_SIZE);
}

static void *insert(struct HashTable *hashTable, u64 key, void *value)
{
    int index = hashFunction(key);
    struct Node *newNode = createNode(key, value);

    if (newNode == NULL) {
        return NULL;
    }
    if (hashTable->table[index] == NULL) {
        hashTable->table[index] = newNode;
    } else {
        newNode->next = hashTable->table[index];
        hashTable->table[index] = newNode;
    }
    return newNode;
}

static int delete(struct HashTable *hashTable, u64 key, struct Node **retNode)
{
    int index = hashFunction(key);
    struct Node *currentNode  = hashTable->table[index];
    struct Node *previousNode = NULL;

    while (currentNode != NULL) {
        if (currentNode->key == key) {
            if (previousNode == NULL) {
                hashTable->table[index] = currentNode->next;
            } else {
                previousNode->next = currentNode->next;
            }
            if (retNode) {
                *retNode = currentNode;
            }
            return 0;
        }
        previousNode = currentNode;
        currentNode  = currentNode->next;
    }
    return -1;
}

static void empty(struct HashTable *hashTable)
{
    int index;
    int empty_count = 0;

    for (index = 0; index < TABLE_SIZE; index++) {
        struct Node *currentNode  = hashTable->table[index];
        struct Node *previousNode = NULL;

        while (currentNode != NULL) {
            previousNode = currentNode;
            currentNode  = currentNode->next;
            empty_count++;
            freeNode(previousNode);
        }
        hashTable->table[index] = NULL;
    }
    rt_printf("empty_count=%d\r\n", empty_count);
}

/* ------------------------------------------------------------------ */
/* 内存越界检测                                                       */
/* ------------------------------------------------------------------ */

static const char *tag2str(unsigned char *buf, char str[32])
{
    int i;
    int len = 0;

    memset(str, 0, 32);
    for (i = 0; i < TAG_LEN; i++) {
        len += sprintf(str + len, "%02x", buf[i]);
    }
    str[len] = '\0';
    return str;
}

static void set_memtag(void *ptr, int len)
{
    unsigned char *ptag = (unsigned char *)&mem_tag;
    unsigned char *head = (unsigned char *)ptr - TAG_LEN;
    unsigned char *tail = (unsigned char *)ptr + len;
    int i;

    for (i = 0; i < (int)sizeof(u64); i++) {
        head[i] = ptag[i];
        tail[i] = ptag[i];
    }
}

static int check_memtag(void *ptr, int len)
{
    unsigned char *ptag = (unsigned char *)&mem_tag;
    unsigned char *head = (unsigned char *)ptr - TAG_LEN;
    unsigned char *tail = (unsigned char *)ptr + len;
    int check = 0;
    int i;

    for (i = 0; i < (int)sizeof(u64); i++) {
        if (head[i] != ptag[i]) { check = 1; break; }
        if (tail[i] != ptag[i]) { check = 2; break; }
    }

    if (check != 0) {
        char str1[32], str2[32];
        tag2str(head, str1);
        tag2str(tail, str2);
        rt_printf("ptr=%p len=%d, checkfail[%d][%s:%s]\r\n",
                  ptr, len, check, str1, str2);
    }
    return check;
}

/* ------------------------------------------------------------------ */
/* 打印单个节点                                                       */
/* ------------------------------------------------------------------ */

static void printOneNode(struct Node *currentNode)
{
    rt_printf("%p:\t\t[%lu]\t\t[%llu]\t\t[%llu]\t\t%p\r\n",
              (void *)(uintptr_t)currentNode->key,
              (unsigned long)(uintptr_t)currentNode->value,
              (unsigned long long)currentNode->tk,
              (unsigned long long)(GetTickCount() - currentNode->tk),
              (void *)currentNode->tid);
    if (currentNode->backtrace) {
        rt_printf("%s\r\n", currentNode->backtrace);
    }
}

/* ------------------------------------------------------------------ */
/* 全局状态                                                           */
/* ------------------------------------------------------------------ */

static struct HashTable *myTable = NULL;
static u32               alloc_count = 0;
static SemaphoreHandle_t mem_lock = NULL;
static int               memcheck_enable = 0;
static u64               last_tick = 0;
static int               mem_custom_len = 0;
static int               mem_total = 0;

#ifdef CARBOX_MEMCHECK_ENABLE_BACKTRACE
static void *backtrace_buff = NULL;
#endif

/* ------------------------------------------------------------------ */
/* alloc / free 回调                                                  */
/* ------------------------------------------------------------------ */

static int alloc_callback(void *ptr, int len)
{
    if (!memcheck_enable) return 0;

    xSemaphoreTake(mem_lock, portMAX_DELAY);

    if (ptr != NULL) {
        struct Node *retNode = insert(myTable, (u64)(uintptr_t)ptr,
                                      (void *)(uintptr_t)len);
        alloc_count++;
        mem_total += len;
        set_memtag(ptr, len);

#ifdef CARBOX_MEMCHECK_ENABLE_BACKTRACE
        if (backtrace_buff && retNode) {
            void *trace[16];
            int   bt_size;
            int   bt_len = 0;
            int   i;

            bt_size = backtrace(trace, 16);
            for (i = 0; i < bt_size && trace[i]; i++) {
                if (bt_len + 32 > BACKTRACE_BUFLEN) break;
                bt_len += sprintf((char *)backtrace_buff + bt_len,
                                  "backtrace : 0x%x\r\n",
                                  (unsigned int)(uintptr_t)trace[i]);
            }
            retNode->backtrace = pvPortMalloc(bt_len + 2);
            if (retNode->backtrace) {
                memcpy(retNode->backtrace, backtrace_buff, bt_len);
                ((char *)retNode->backtrace)[bt_len] = '\0';
            }
        }
#endif

        if (mem_custom_len > 0 && len == mem_custom_len) {
            rt_printf("ADD len=%d, tid=%p, ptr=%p\r\n",
                      len, (void *)xTaskGetCurrentTaskHandle(), ptr);
        }
    }

    xSemaphoreGive(mem_lock);

    if (GetTickCount() - last_tick > 5000) {
        last_tick = GetTickCount();
        rt_printf("###alloc_count=%lu, mem_total=%d\r\n",
                  (unsigned long)alloc_count, mem_total);
    }

    return 0;
}

static int free_callback(void *ptr)
{
    int result = -1;

    if (!memcheck_enable) return 0;

    xSemaphoreTake(mem_lock, portMAX_DELAY);

    if (ptr != NULL) {
        struct Node *retNode = NULL;

        if (delete(myTable, (u64)(uintptr_t)ptr, &retNode) == 0) {
            u64 len = (u64)(uintptr_t)retNode->value;

            alloc_count--;
            mem_total -= (int)len;
            result = (int)len;

            if (mem_custom_len > 0 && (int)len == mem_custom_len) {
                rt_printf("DEL len=%llu, tid=%p, ptr=%p\r\n",
                          (unsigned long long)len,
                          (void *)xTaskGetCurrentTaskHandle(), ptr);
            }
            if (check_memtag(ptr, (int)len) != 0) {
                printOneNode(retNode);
            }
            freeNode(retNode);
        }
    }

    xSemaphoreGive(mem_lock);

    if (GetTickCount() - last_tick > 5000) {
        last_tick = GetTickCount();
        rt_printf("###alloc_count=%lu, mem_total=%d\r\n",
                  (unsigned long)alloc_count, mem_total);
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* 用户可调用的包装函数                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief 内存分配（带检测）
 *
 * 行为等同于 pvPortMalloc(size + TAG_LEN * 2)，并在前后写入魔数和记录分配信息。
 * 返回的指针偏移 TAG_LEN 字节，指向用户可用区域。
 *
 * @param size 用户请求的字节数
 * @return 用户数据区指针，失败返回 NULL
 */
void *memcheck_malloc(size_t size)
{
    void *raw;
    void *user_ptr;

    if (size == 0) {
        return NULL;
    }

    /* 分配: [tag] + [user data] + [tag] */
    raw = pvPortMalloc(size + TAG_LEN2);
    if (raw == NULL) {
        return NULL;
    }

    user_ptr = (unsigned char *)raw + TAG_LEN;
    alloc_callback(user_ptr, (int)size);
    return user_ptr;
}

/**
 * @brief 内存释放（带检测）
 *
 * 检测内存越界，然后释放包括 tag 区域在内的全部内存。
 *
 * @param ptr memcheck_malloc 返回的指针，NULL 时直接返回
 */
void memcheck_free(void *ptr)
{
    void *raw;

    if (ptr == NULL) {
        return;
    }

    free_callback(ptr);
    raw = (unsigned char *)ptr - TAG_LEN;
    vPortFree(raw);
}

/* ------------------------------------------------------------------ */
/* 哈希表打印                                                         */
/* ------------------------------------------------------------------ */

static void print_hashTable(void)
{
    int index;
    int n = 0;
    u64 total_len = 0;

    rt_printf("============================================MEMCHECK========================================\r\n");
    rt_printf("alloc_count=%lu, mem_total=%d\r\n",
              (unsigned long)alloc_count, mem_total);

    xSemaphoreTake(mem_lock, portMAX_DELAY);

    for (index = 0; index < TABLE_SIZE; index++) {
        struct Node *currentNode = myTable->table[index];

        while (currentNode != NULL) {
            void *ptr = (void *)(uintptr_t)currentNode->key;
            u64   len = (u64)(uintptr_t)currentNode->value;

            if (mem_custom_len > 0 && (int)len != mem_custom_len) {
                /* 过滤：仅显示指定大小的分配 */
            } else {
                printOneNode(currentNode);
                check_memtag(ptr, (int)len);
                n++;
                total_len += len;
            }
            currentNode = currentNode->next;
        }
    }

    xSemaphoreGive(mem_lock);

    rt_printf("alloc_count=%lu, n[%d], total_len[%llu]\r\n",
              (unsigned long)alloc_count, n, (unsigned long long)total_len);
    rt_printf("==========================================MEMCHECK END======================================\r\n");
}

static void empty_hashTable(void)
{
    xSemaphoreTake(mem_lock, portMAX_DELAY);
    rt_printf("alloc_count=%lu\r\n", (unsigned long)alloc_count);
    empty(myTable);
    alloc_count = 0;
    mem_total   = 0;
    xSemaphoreGive(mem_lock);
}

/* ------------------------------------------------------------------ */
/* shell 命令入口（签名匹配 shell_program_t: s32 (*)(u32, char **)）  */
/* ------------------------------------------------------------------ */

/**
 * @brief checkmem 命令处理函数
 *
 * 用法:
 *   checkmem          首次输入启动检测，之后打印哈希表
 *   checkmem 1         启用检测
 *   checkmem 0         暂停检测（不重置数据）
 *   checkmem <N>       仅显示大小为 N 的分配记录
 */
s32 memcheck_main(u32 argc, char **argv)
{
    int param = -9;

    if (argc > 1) {
        param = atoi(argv[1]);
    }

    if (myTable == NULL) {
        rt_printf("create hash table\r\n");

#ifdef CARBOX_MEMCHECK_ENABLE_BACKTRACE
        backtrace_buff = pvPortMalloc(BACKTRACE_BUFLEN);
        if (backtrace_buff == NULL) return 0;
#endif

        myTable = createHashTable();
        if (myTable == NULL) {
#ifdef CARBOX_MEMCHECK_ENABLE_BACKTRACE
            vPortFree(backtrace_buff);
            backtrace_buff = NULL;
#endif
            rt_printf("ERROR: createHashTable failed\r\n");
            return -1;
        }

        mem_lock = xSemaphoreCreateMutex();
        if (mem_lock == NULL) {
            rt_printf("ERROR: xSemaphoreCreateMutex failed\r\n");
            return -1;
        }

        memcheck_enable = 1;
        alloc_count     = 0;
        mem_total       = 0;
        last_tick       = 0;
        mem_custom_len  = 0;

        rt_printf("memcheck started, alloc_count=%lu\r\n",
                  (unsigned long)alloc_count);
        return 0;
    }

    /* 已初始化，处理子命令 */
    if (param >= 0) {
        if (param == 1) {
            memcheck_enable = 1;
            rt_printf("memcheck enabled\r\n");
        } else if (param == 0) {
            memcheck_enable = !memcheck_enable;
            rt_printf("memcheck %s\r\n",
                      memcheck_enable ? "enabled" : "paused");
        } else if (param > 4) {
            mem_custom_len = param;
            rt_printf("memcheck filter len=%d\r\n", mem_custom_len);
        }
        return 0;
    }

    /* 默认行为：打印哈希表 */
    rt_printf("print hash table TAG_LEN=%lu, mem_tag=0x%llx\r\n",
              (unsigned long)TAG_LEN, (unsigned long long)mem_tag);
    print_hashTable();
    mem_custom_len = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 全局 malloc/free 拦截宏（可选）                                    */
/* ------------------------------------------------------------------ */

#ifdef CARBOX_MEMCHECK_WRAP_GLOBAL
/* 警告：此宏会拦截所有模块的 malloc/free，包括 SDK 内部 */
/* 仅在确认 SDK 内部无特殊内存需求时使用 */
#undef  malloc
#define malloc(s)    memcheck_malloc(s)
#undef  free
#define free(p)      memcheck_free(p)
#endif
