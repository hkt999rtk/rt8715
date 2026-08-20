/////////////////////////////////////////////////////////////////////////////
/* NCM TX aggregation + GTimer flush path.
 *
 * Extracted from ncm.c so the TX helpers can be reused by third parties
 * without pulling in the RX/unwrap code. Public API is declared in ncm_tx.h:
 *   cdc_ncm_fill_tx_frame(), cdc_ncm_tx_fixup(),
 *   cdc_ncm_tx_lock()/cdc_ncm_tx_unlock(),
 *   ncm_timer_init(), ncm_stat_send_latency()
 */
#include "ncm_tx_abi.h"
#include "ncm_tx_batch.h"
#include "../large_memcpy_gdma.h"

#ifndef NCM_TX_COMPAT_SINGLE_DATAGRAM
#define NCM_TX_COMPAT_SINGLE_DATAGRAM 0
#endif

#ifndef CONFIG_NCM_TX_LINKED_GDMA
#define CONFIG_NCM_TX_LINKED_GDMA 0
#endif

/* 固件侧日志映射：库侧由 stub smart_log.h 提供 RTK_LOGS 宏，固件 SDK 没有，
 * 这里映射到 rt_printf，避免链接 undefined reference。 */
#include "diag.h"
#ifndef RTK_LOGS
#define RTK_LOGS(tag, fmt, ...) rt_printf("[%s] " fmt, tag, ##__VA_ARGS__)
#endif

/* byte-order helpers — little-endian target: identity (mirrors cdc_ncm.c) */
#define le16_to_cpu(x)  ((u16)(x))
#define le32_to_cpu(x)  ((u32)(x))
#define cpu_to_le16(x)  ((u16)(x))
#define cpu_to_le32(x)  ((u32)(x))

static const char *TAG = "NCM";
#ifdef NCM_DEBUG
#define __msg(fmt, ...) RTK_LOGS(TAG, fmt, ##__VA_ARGS__)
#define __MSG(fmt, ...) RTK_LOGS(TAG, fmt, ##__VA_ARGS__)
#define __here__ RTK_LOGS(TAG, "[%s]:%d\n",__func__,__LINE__)
#define __NCM_MSG(fmt, ...) RTK_LOGS(TAG, fmt, ##__VA_ARGS__)
#else
#define __msg(fmt, ...)
#define __MSG(fmt, ...)
#define __NCM_MSG(fmt, ...)
#define __here__
#endif

#if !NCM_TX_COMPAT_SINGLE_DATAGRAM && \
	(defined(USE_TIMER) || defined(USE_STAT))
extern u64 usbh_get_timestamp(struct usb_host *host);
#endif

#if !NCM_TX_COMPAT_SINGLE_DATAGRAM
/* hal.c: flush a pending NTB (deferred flush path, USE_TIMER) */
extern int usbh_cdc_ncm_send_data(u8 *buf, u32 len);

#ifdef USE_TIMER
static void cdc_ncm_tx_timer_cb(usbh_cdc_ncm_host_hal_t *dev);
static void cdc_ncm_tx_timeout_start(struct cdc_ncm_ctx *ctx, usbh_cdc_ncm_host_hal_t *dev);
static void cdc_ncm_txpath_bh(unsigned long param);
#endif
#ifdef USE_THREAD
static void ncm_tx_bh_thread(void *param);
#endif
#ifdef USE_STAT
static void ncm_stat_thread(void *param);
#endif
#endif

#if !NCM_TX_COMPAT_SINGLE_DATAGRAM
static size_t cdc_ncm_align_tail(void *data, size_t len, size_t modulus, size_t remainder, size_t max)
{
	size_t align = ALIGN(len, modulus) - len + remainder;

	if (len + align > max)
		align = max - len;
	__MSG("%s align=%d max=%d len=%d\n", __func__,(int)align,(int)max,(int)len);
	if (align)
	{
		void *end = (char *)data + len;
		memset(end, 0, align);
	}
	return len + align;
}


static struct usb_cdc_ncm_ndp16 *cdc_ncm_ndp(struct cdc_ncm_ctx *ctx, void *data, size_t len, __u32 sign, size_t reserve)
{
    struct usb_cdc_ncm_ndp16 *ndp16 = NULL;
    struct usb_cdc_ncm_nth16 *nth16 = (struct usb_cdc_ncm_nth16 *)data;
    size_t ndpoffset = le16_to_cpu(nth16->wNdpIndex);
    size_t curr_len = len;
    size_t max_len = ctx->tx_curr_size; // Maximum length of the data buffer

	__MSG("%s\n", __func__);
	__MSG("%s ndpoffset=%d\n", __func__,ndpoffset);
    /* If NDP should be moved to the end of the NCM package, we can't follow the
     * NTH16 header as we would normally do. NDP isn't written to the data buffer yet, and
     * the wNdpIndex field in the header is actually not consistent with reality. It will be later.
     */
    if (ctx->drvflags & CDC_NCM_FLAG_NDP_TO_END) {
        if (ctx->delayed_ndp16->dwSignature == sign)
            return ctx->delayed_ndp16;

        /* We can only push a single NDP to the end. Return
         * NULL to send what we've already got and queue this
         * data buffer for later.
         */
        else if (ctx->delayed_ndp16->dwSignature)
            return NULL;
    }

    /* follow the chain of NDPs, looking for a match */
    while (ndpoffset) {
        ndp16 = (struct usb_cdc_ncm_ndp16 *)(data + ndpoffset);
        if (ndp16->dwSignature == sign)
		{
			__MSG("%s find ndp\n", __func__);
			return ndp16;
		}
        ndpoffset = le16_to_cpu(ndp16->wNextNdpIndex);
    }

    /* align new NDP */
    if (!(ctx->drvflags & CDC_NCM_FLAG_NDP_TO_END))
	{
		__MSG("%s NEW NDP cdc_ncm_align_tail\n", __func__);
        curr_len = cdc_ncm_align_tail(data, curr_len, ctx->tx_ndp_modulus, 0, max_len);
	}

    /* verify that there is room for the NDP and the datagram (reserve) */
    if ((max_len - curr_len - reserve) < ctx->max_ndp_size)
	{
		__MSG("%s NULL\n", __func__);
        return NULL;
	}

    /* link to it */
    if (ndp16)
        ndp16->wNextNdpIndex = cpu_to_le16(curr_len);
    else
        nth16->wNdpIndex = cpu_to_le16(curr_len);

    /* push a new empty NDP */
    if (!(ctx->drvflags & CDC_NCM_FLAG_NDP_TO_END)) {
		__MSG("%s NEW NDP\n", __func__);
        ndp16 = (struct usb_cdc_ncm_ndp16 *)(data + curr_len);
		__MSG("%s ctx->max_ndp_size=%d\n", __func__,ctx->max_ndp_size);
        memset(ndp16, 0, ctx->max_ndp_size); // Initialize the new NDP with zeros
        curr_len += ctx->max_ndp_size;

		// com_print_buf_data(data, curr_len);	
		__MSG("%s curr_len=%d\n", __func__,curr_len);
    } else {
        ndp16 = ctx->delayed_ndp16;
    }

	__MSG("%s wLength=%d\n", __func__,cpu_to_le16(sizeof(struct usb_cdc_ncm_ndp16) + sizeof(struct usb_cdc_ncm_dpe16)));
    ndp16->dwSignature = sign;
    ndp16->wLength = cpu_to_le16(sizeof(struct usb_cdc_ncm_ndp16) + sizeof(struct usb_cdc_ncm_dpe16));
	// com_print_buf_data(data, curr_len);	
	ctx->tx_curr_len = curr_len;
	// __MSG("%s ndp16->wLength=%d\n", __func__,ndp16->wLength);
    return ndp16;
}



void *cdc_ncm_fill_tx_frame(usbh_cdc_ncm_host_hal_t *dev, void *data, size_t len, __u32 sign, size_t *out_len)
{
    struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
    struct usb_cdc_ncm_nth16 *nth16;// NTH16头指针
    struct usb_cdc_ncm_ndp16 *ndp16;// NDP16头指针
    void *out_data = NULL;// 输出数据
    size_t curr_len = 0; //当前长度
    u16 n = 0, index, ndplen;// 循环变量，索引，NDP长度
    u8 ready2send = 0;// 准备发送标志
    u32 delayed_ndp_size;// 延迟NDP大小

	__NCM_MSG("len=%d ctx->tx_curr_frame_num=%d\n",len,ctx->tx_curr_frame_num);
	__NCM_MSG("out_data=0x%x\n", out_data);
	// 根据标志位计算延迟NDP大小
    if (ctx->drvflags & CDC_NCM_FLAG_NDP_TO_END)
        delayed_ndp_size = ALIGN(ctx->max_ndp_size, ctx->tx_ndp_modulus);
    else
        delayed_ndp_size = 0;
	__NCM_MSG("ctx->max_ndp_size=%d,ctx->tx_ndp_modulus=%d\n", ctx->max_ndp_size,ctx->tx_ndp_modulus);
	__NCM_MSG("delayed_ndp_size=%d\n", delayed_ndp_size);
	// Swap pattern: 存储新数据到 rem，清除指针供循环取用
    if (data != NULL) {
        ctx->tx_rem_data = data;
        ctx->tx_rem_len = len;
        ctx->tx_rem_sign = sign;
        data = NULL;  // 清空指针，循环将从 rem_data 取出
    } else {
        ready2send = 1;// 没有新数据要发送，标记为准备发送
    }


    // 恢复之前累积的发送缓冲区
    out_data = ctx->tx_curr_data;
    curr_len = ctx->tx_curr_len;
    __NCM_MSG("out_data=0x%x curr_len=%d n=%d\n", out_data, (int)curr_len, (int)ctx->tx_curr_frame_num);

		// 使用预分配的 out_buf，避免频繁 malloc/free
    if (!out_data) {
        ctx->tx_curr_size = ctx->tx_max;
        out_data = dev->out_buf;
        memset(out_data, 0, ctx->tx_curr_size);
		// 初始化NTH16头部
        nth16 = (struct usb_cdc_ncm_nth16 *)out_data;
        nth16->dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
        nth16->wHeaderLength = cpu_to_le16(sizeof(struct usb_cdc_ncm_nth16));
        nth16->wSequence = cpu_to_le16(ctx->tx_seq++);// 序列号递增
        ctx->tx_curr_frame_num = 0;// 当前帧号初始化
        ctx->tx_curr_frame_payload = 0;// 当前帧有效载荷大小初始化
        curr_len = sizeof(struct usb_cdc_ncm_nth16);
    }
	__NCM_MSG("curr_len=%d\n",curr_len);
	__NCM_MSG("ctx->tx_max_datagrams=%d\n",ctx->tx_max_datagrams);
	__NCM_MSG(" ctx->tx_curr_frame_num=%d\n",ctx->tx_curr_frame_num);
	__NCM_MSG("22222222\n");
		 // 循环处理每个数据报
    for (n = ctx->tx_curr_frame_num; n < ctx->tx_max_datagrams; n++) {
        // 如果没有新数据要发送，使用之前保存的剩余数据
		if (data == NULL) {
            data = ctx->tx_rem_data;
            len = ctx->tx_rem_len;
            sign = ctx->tx_rem_sign;
            ctx->tx_rem_data = NULL;//释放备份内存
            __NCM_MSG("@@@@@@@@@@@@@@@@@@@@\n");
            if (data == NULL)// 没有剩余数据可发送，退出循环
            {											
                __NCM_MSG("#####################\n");
                break;
            }

        }
		__NCM_MSG(" ctx->tx_remainder=%d,ctx->tx_modulus=%d\n",ctx->tx_remainder,ctx->tx_modulus);
        // 创建并填充NDP16头部
        ctx->tx_curr_len = curr_len;
		ndp16 = cdc_ncm_ndp(ctx, out_data, ctx->tx_curr_len, sign, len + ctx->tx_modulus + ctx->tx_remainder);
        curr_len = ctx->tx_curr_len;
        __NCM_MSG("curr_len=%d\n",curr_len);
		// com_print_buf_data(out_data, curr_len);
		__NCM_MSG(" ctx->tx_remainder=%d,tx_curr_size=%d\n",ctx->tx_remainder,ctx->tx_curr_size);
        curr_len = cdc_ncm_align_tail(out_data, curr_len, ctx->tx_modulus, ctx->tx_remainder, ctx->tx_curr_size);
		// 检查NDP16头部是否有效，并检查是否超过缓冲区大小
		// com_print_buf_data(out_data, curr_len);
        if (!ndp16 || curr_len + len + delayed_ndp_size > ctx->tx_curr_size) {
            // 如果是第一个数据报，释放数据并标记为发送失败
			if (n == 0) {
                // __MSG("------------------\n"); 
                data = NULL;
                len = 0;
                // dev->net->stats.tx_dropped++;
				__NCM_MSG(" tx_dropped\n");
            } else {
				/* no room for skb - store for later, drop old rem if any */
                if (ctx->tx_rem_data != NULL) {
                    __NCM_MSG("ntb_full: dropping old tx_rem_data\n");
                    ctx->tx_rem_data = NULL;
                    ctx->tx_rem_len = 0;
                }
                ctx->tx_rem_data = data;
                ctx->tx_rem_len = len;
                ctx->tx_rem_sign = sign;
                //  __MSG("+++++++++++++++++++\n"); 
                data = NULL;
                ready2send = 1;
				 __NCM_MSG(" ready2send\n");
                ctx->tx_reason_ntb_full++;
            }
            break;
        }


        // com_print_buf_data(out_data, curr_len);
		// 计算并填充NDP长度和索引
        ndplen = le16_to_cpu(ndp16->wLength);
        index = (ndplen - sizeof(struct usb_cdc_ncm_ndp16)) / sizeof(struct usb_cdc_ncm_dpe16) - 1;
		__NCM_MSG("index=%d,ndplen=%d\n",index,ndplen);
        ndp16->dpe16[index].wDatagramLength = cpu_to_le16(len);
        ndp16->dpe16[index].wDatagramIndex = cpu_to_le16(curr_len);
        ndp16->wLength = cpu_to_le16(ndplen + sizeof(struct usb_cdc_ncm_dpe16));
		// 将数据复制到发送缓冲区并更新当前位置和有效载荷大小
		__NCM_MSG("index=%d,ndplen=%d, wDatagramLength=%d,wDatagramIndex=%d,ndp16->wLength=%d\n",index,ndplen,len,curr_len,ndp16->wLength);
        memcpy(out_data + curr_len, data, len);
        curr_len += len;
 	 __NCM_MSG(" out_data=0x%x curr_len=%d\n",out_data,curr_len);  
        // com_print_buf_data(out_data, curr_len);
        // if(index + 1 < ctx->tx_max_datagrams )
        // {
        //     // 填充2个字节 参考iphone7
        //     memset(out_data + curr_len + 2, 0, 2);
        //     curr_len += 2;
        // }

 	 __NCM_MSG(" out_data=0x%x curr_len=%d\n",out_data,curr_len);  
        // com_print_buf_data(out_data, curr_len);
        ctx->tx_curr_frame_payload += len;
        if (data != NULL) {
             __NCM_MSG("==========================\n"); 
            data = NULL;//数据和备份数据一起释放
            len = 0;
            // dev->net->stats.tx_dropped++;
        }
		// NDP full → send
        if (index >= CDC_NCM_DPT_DATAGRAMS_MAX) {
            ready2send = 1;
			__NCM_MSG(" ready2send\n");
            ctx->tx_reason_ndp_full++;
            break;
        }
    }

	__NCM_MSG("3333333\n");

    // 从累积帧数恢复 n，跳过空循环直接进入发送判断
    // if (n == 0)
    //     n = ctx->tx_curr_frame_num;
	// 处理剩余数据
    if (data != NULL) {
        __NCM_MSG("````````````````````````\n"); 
        data = NULL;
        len = 0;
        // dev->net->stats.tx_dropped++;
    }
	__NCM_MSG(" 444444\n");
    ctx->tx_curr_frame_num = n;// 更新当前帧号
	// 根据条件判断是否进入准备发送状态或重启定时器
    if (n == 0) {
        ctx->tx_curr_data = out_data;
        ctx->tx_curr_len = curr_len;
        goto exit_no_data;
    } else if ((n < ctx->tx_max_datagrams) && (ready2send == 0) && (ctx->timer_interval > 0)) {
        ctx->tx_curr_data = out_data;
        ctx->tx_curr_len = curr_len;
        /* Only set pending when exhausted, prevent reset while counting */
		if (n < CDC_NCM_RESTART_TIMER_DATAGRAM_CNT) {
			ctx->tx_timer_pending = CDC_NCM_TIMER_PENDING_CNT;
		}
		__NCM_MSG("tx_timer_pending =%d ctx->tx_curr_len=%d  n=%d\n",ctx->tx_timer_pending,ctx->tx_curr_len,n);

        goto exit_no_data;

    } else {
        if (n == ctx->tx_max_datagrams)
            ctx->tx_reason_max_datagram++;
    }
	__NCM_MSG("55555\n");
	// 如果标志允许NDP发送到末尾，处理延迟NDP部分
		// com_print_buf_data(out_data, curr_len);
    if (ctx->drvflags & CDC_NCM_FLAG_NDP_TO_END) {
        nth16 = (struct usb_cdc_ncm_nth16 *)out_data;
        curr_len = cdc_ncm_align_tail(out_data, curr_len, ctx->tx_ndp_modulus, 0, ctx->tx_curr_size - ctx->max_ndp_size);
        nth16->wNdpIndex = cpu_to_le16(curr_len);
        memcpy(out_data + curr_len, ctx->delayed_ndp16, ctx->max_ndp_size);
        curr_len += ctx->max_ndp_size;
        ndp16 = memset(ctx->delayed_ndp16, 0, ctx->max_ndp_size);
    }
	__NCM_MSG("666666\n");

	/* Pad to tx_max for DMA efficiency, matching Linux min_tx_pkt logic */
	if (curr_len > ctx->tx_max - 3 * ctx->maxpacket) {
		u32 pad = ctx->tx_max - curr_len;
		memset(out_data + curr_len, 0, pad);
		curr_len = ctx->tx_max;
	} else if (curr_len < ctx->tx_max && (curr_len % ctx->maxpacket) == 0) {
		memset(out_data + curr_len, 0, 1);
		curr_len += 1;
	}
	__NCM_MSG("7777777\n");
	// 重新获取NTH16头
	__NCM_MSG(" out_data=0x%x curr_len=%d\n",out_data,curr_len);
    nth16 = (struct usb_cdc_ncm_nth16 *)out_data;
    nth16->wBlockLength = cpu_to_le16(curr_len);
    ctx->tx_curr_data = NULL;
    ctx->tx_curr_len = 0;

	// 更新开销和传输NTB的次数
    ctx->tx_overhead += curr_len - ctx->tx_curr_frame_payload;
	ctx->tx_ntbs++;
#ifdef USE_STAT		
    ctx->tx_datagrams += ctx->tx_curr_frame_num;
    ctx->tx_payload_bytes += ctx->tx_curr_frame_payload;
#endif

#ifdef USE_THREAD
    if (!ctx->bh_created) {
        ctx->bh_created = 1;
        int s = rtos_task_create(NULL, "ncm_tx_bh", ncm_tx_bh_thread, dev, 0x4000, 5);
        if (s != SUCCESS) RTK_LOGS(TAG, "[NCM] bh thread fail\n");
    }
#endif

#ifdef USE_STAT	
    if (!ctx->stat_started) {
        ctx->stat_started = 1;
        int s = rtos_task_create(NULL, "ncm_stat", ncm_stat_thread, dev, 4096, 2);
        if (s != SUCCESS) RTK_LOGS(TAG, "[NCM]stat thread fail\n");
    }	
#endif	
	__NCM_MSG("88888888\n");
    *out_len = curr_len;

    return out_data;

exit_no_data:
#ifdef USE_TIMER
    if (ctx->tx_curr_data != NULL && n > 0) {
		__NCM_MSG("start timer: pending=%u n=%u len=%u\n",
			 ctx->tx_timer_pending, n, ctx->tx_curr_len);
		cdc_ncm_tx_timeout_start(ctx, dev);
	}
#endif	
    return NULL;
}
#endif



#if NCM_TX_COMPAT_SINGLE_DATAGRAM
_Static_assert(sizeof(struct usb_cdc_ncm_nth16) == 12U,
	       "NCM NTH16 wire layout changed");
_Static_assert(sizeof(struct usb_cdc_ncm_ndp16) == 8U,
	       "NCM NDP16 wire layout changed");
_Static_assert(sizeof(struct usb_cdc_ncm_dpe16) == 4U,
	       "NCM DPE16 wire layout changed");
_Static_assert(offsetof(struct cdc_ncm_ctx, mtx) == 172U,
	       "customer cdc_ncm_ctx ABI changed before mtx");
_Static_assert(offsetof(struct cdc_ncm_ctx, tx_max) == 208U,
	       "customer cdc_ncm_ctx ABI changed before tx_max");
_Static_assert(offsetof(struct cdc_ncm_ctx, tx_seq) == 228U,
	       "customer cdc_ncm_ctx ABI changed before tx_seq");

struct ncm_single_profile {
	volatile u32 ok;
	volatile u32 payload_bytes;
	volatile u32 ntb_bytes;
	volatile u32 clear_bytes;
	volatile u32 batch_ntbs;
	volatile u32 batch_datagrams;
	volatile u32 batch_payload_bytes;
	volatile u32 batch_reject;
	volatile u32 reject_dev;
	volatile u32 reject_arg;
	volatile u32 reject_size;
	volatile u32 timer_init;
	volatile u32 timer_start;
	volatile u32 lock_fail;
	volatile u32 cfg_tx_max;
	volatile u32 cfg_max_datagram;
	volatile u32 cfg_max_ndp;
	volatile u32 cfg_max_datagrams;
	volatile u32 cfg_modulus;
	volatile u32 cfg_remainder;
	volatile u32 gdma_attempts;
	volatile u32 gdma_success;
	volatile u32 gdma_fallback;
	volatile u32 gdma_bytes;
	volatile u32 gdma_cpu_edge_bytes;
	volatile u32 gdma_blocks;
	volatile u32 gdma_batches;
	volatile u32 gdma_cpu_fallback_bytes;
};

static struct ncm_single_profile ncm_single_profile;
static const struct carbox_ncm_tx_batch *ncm_active_batch;
struct ncm_prebuilt_tx {
	void *buffer;
	size_t len;
};
static const struct ncm_prebuilt_tx *ncm_active_prebuilt;
static u8 ncm_batch_built;
static u16 ncm_batch_sent_frames;

#define NCM_BATCH_LENGTH_TAG 0x80000000U
#define NCM_PREBUILT_LENGTH_TAG 0x40000000U

extern int usbh_cdc_ncm_send_data(u8 *buf, u32 len);

static size_t ncm_batch_align(size_t len, u16 modulus, u16 remainder)
{
	return ALIGN(len, (size_t)modulus) + remainder;
}

static void *cdc_ncm_tx_build_batch(usbh_cdc_ncm_host_hal_t *dev,
				    const struct carbox_ncm_tx_batch *batch,
				    u8 *target, size_t target_capacity,
				    size_t *out_len, u16 *built_frames)
{
	struct cdc_ncm_ctx *ctx;
	struct usb_cdc_ncm_nth16 *nth16;
	struct usb_cdc_ncm_ndp16 *ndp16;
	size_t offsets[CARBOX_NCM_TX_BATCH_MAX_DATAGRAMS];
	size_t header_len;
	size_t ndp_len;
	size_t ntb_len;
	size_t payload_bytes = 0U;
	u16 modulus;
	u16 remainder;
	u32 frame_index;
	u32 selected_count;
	int candidate_valid;
#if CONFIG_NCM_TX_LINKED_GDMA
	carbox_gdma_copy_block_t copy_blocks[CARBOX_NCM_TX_BATCH_MAX_SEGMENTS];
	size_t copy_block_count = 0U;
#endif

	if (out_len == NULL || built_frames == NULL) {
		ncm_single_profile.batch_reject++;
		return NULL;
	}
	*out_len = 0U;
	*built_frames = 0U;
	if (dev == NULL || batch == NULL ||
	    batch->magic != CARBOX_NCM_TX_BATCH_MAGIC ||
	    batch->frame_count < 1U ||
	    batch->frame_count > CARBOX_NCM_TX_BATCH_MAX_DATAGRAMS ||
	    batch->segment_count > CARBOX_NCM_TX_BATCH_MAX_SEGMENTS) {
		ncm_single_profile.batch_reject++;
		return NULL;
	}
	ctx = (struct cdc_ncm_ctx *)dev->data[0];
	if (ctx == NULL || target == NULL || target_capacity == 0U) {
		ncm_single_profile.batch_reject++;
		return NULL;
	}
	modulus = ctx->tx_modulus != 0U ? ctx->tx_modulus : 1U;
	remainder = ctx->tx_remainder;
	if ((modulus & (modulus - 1U)) != 0U || remainder >= modulus) {
		ncm_single_profile.batch_reject++;
		return NULL;
	}

	selected_count = batch->frame_count;
	if (ctx->tx_max_datagrams != 0U &&
	    selected_count > ctx->tx_max_datagrams)
		selected_count = ctx->tx_max_datagrams;

	/* Select the largest FIFO prefix that fits the negotiated NTB capacity. */
	for (; selected_count >= 1U; --selected_count) {
		ndp_len = sizeof(*ndp16) +
			sizeof(struct usb_cdc_ncm_dpe16) *
			((size_t)selected_count + 1U);
		header_len = sizeof(*nth16) + ndp_len;
		if ((ctx->max_ndp_size != 0U && ndp_len > ctx->max_ndp_size) ||
		    header_len > ctx->tx_max || header_len > target_capacity ||
		    header_len > 0xffffU)
			continue;

		ntb_len = header_len;
		payload_bytes = 0U;
		candidate_valid = 1;
		for (frame_index = 0U; frame_index < selected_count; ++frame_index) {
			const struct carbox_ncm_tx_frame *frame =
				&batch->frame[frame_index];
			size_t segment_bytes = 0U;
			u32 segment_index;

			if (frame->segment_count == 0U ||
			    frame->first_segment >= batch->segment_count ||
			    frame->segment_count >
				batch->segment_count - frame->first_segment ||
			    frame->len == 0U || frame->len > 0xffffU ||
			    (ctx->max_datagram_size != 0U &&
			     frame->len > ctx->max_datagram_size)) {
				ncm_single_profile.batch_reject++;
				return NULL;
			}
			for (segment_index = 0U;
			     segment_index < frame->segment_count; ++segment_index) {
				const struct carbox_ncm_tx_segment *segment =
					&batch->segment[frame->first_segment + segment_index];
				if (segment->data == NULL ||
				    segment->len > frame->len - segment_bytes) {
					ncm_single_profile.batch_reject++;
					return NULL;
				}
				segment_bytes += segment->len;
			}
			if (segment_bytes != frame->len) {
				ncm_single_profile.batch_reject++;
				return NULL;
			}

			offsets[frame_index] = ncm_batch_align(ntb_len, modulus,
							 remainder);
			if (offsets[frame_index] < ntb_len ||
			    offsets[frame_index] > ctx->tx_max ||
			    offsets[frame_index] > target_capacity ||
			    frame->len > ctx->tx_max - offsets[frame_index] ||
			    frame->len > target_capacity - offsets[frame_index] ||
			    frame->len > 0xffffU - offsets[frame_index]) {
				candidate_valid = 0;
				break;
			}
			ntb_len = offsets[frame_index] + frame->len;
			payload_bytes += frame->len;
		}
		if (candidate_valid != 0)
			break;
	}
	if (selected_count < 1U) {
		ncm_single_profile.batch_reject++;
		return NULL;
	}

	usb_os_lock(ctx->mtx);
	memset(target, 0, header_len);
	nth16 = (struct usb_cdc_ncm_nth16 *)target;
	nth16->dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
	nth16->wHeaderLength = cpu_to_le16(sizeof(*nth16));
	nth16->wSequence = cpu_to_le16(ctx->tx_seq++);
	nth16->wBlockLength = cpu_to_le16(ntb_len);
	nth16->wNdpIndex = cpu_to_le16(sizeof(*nth16));

	ndp16 = (struct usb_cdc_ncm_ndp16 *)(target + sizeof(*nth16));
	ndp16->dwSignature = cpu_to_le32(USB_CDC_NCM_NDP16_NOCRC_SIGN);
	ndp16->wLength = cpu_to_le16(ndp_len);
	for (frame_index = 0U; frame_index < selected_count; ++frame_index) {
		const struct carbox_ncm_tx_frame *frame = &batch->frame[frame_index];
		u32 segment_index;
		u8 *destination = target + offsets[frame_index];

		ndp16->dpe16[frame_index].wDatagramIndex =
			cpu_to_le16(offsets[frame_index]);
		ndp16->dpe16[frame_index].wDatagramLength =
			cpu_to_le16(frame->len);
		if (offsets[frame_index] > header_len && frame_index == 0U) {
			memset(target + header_len, 0,
			       offsets[frame_index] - header_len);
		} else if (frame_index != 0U) {
			size_t previous_end = offsets[frame_index - 1U] +
				batch->frame[frame_index - 1U].len;
			if (offsets[frame_index] > previous_end) {
				memset(target + previous_end, 0,
				       offsets[frame_index] - previous_end);
			}
		}
		for (segment_index = 0U; segment_index < frame->segment_count;
		     ++segment_index) {
			const struct carbox_ncm_tx_segment *segment =
				&batch->segment[frame->first_segment + segment_index];
#if CONFIG_NCM_TX_LINKED_GDMA
			copy_blocks[copy_block_count].dst = destination;
			copy_blocks[copy_block_count].src = segment->data;
			copy_blocks[copy_block_count].len = (uint32_t)segment->len;
			copy_block_count++;
#else
			memcpy(destination, segment->data, segment->len);
#endif
			destination += segment->len;
		}
	}

#if CONFIG_NCM_TX_LINKED_GDMA
	/* The USB HAL requires one contiguous NTB, so this is a gather copy rather
	 * than USB zero-copy.  Sources remain referenced by the async owner until
	 * this synchronous builder returns.  The helper commits the whole copy or
	 * returns zero after quiescing GDMA, making a complete CPU recopy safe. */
	{
		carbox_gdma_copyv_result_t gdma_result;

		ncm_single_profile.gdma_attempts++;
		if (carbox_linked_gdma_copyv_bytes_try(copy_blocks,
						 copy_block_count,
						 &gdma_result)) {
			ncm_single_profile.gdma_success++;
			ncm_single_profile.gdma_bytes += gdma_result.dma_bytes;
			ncm_single_profile.gdma_cpu_edge_bytes +=
				gdma_result.cpu_edge_bytes;
			ncm_single_profile.gdma_blocks += gdma_result.dma_blocks;
			ncm_single_profile.gdma_batches += gdma_result.dma_batches;
		} else {
			size_t block_index;

			ncm_single_profile.gdma_fallback++;
			ncm_single_profile.gdma_cpu_fallback_bytes +=
				(u32)payload_bytes;
			for (block_index = 0U; block_index < copy_block_count;
			     ++block_index) {
				memcpy(copy_blocks[block_index].dst,
				       copy_blocks[block_index].src,
				       copy_blocks[block_index].len);
			}
		}
	}
#endif

	ctx->tx_overhead += ntb_len - payload_bytes;
	ctx->tx_ntbs++;
	ncm_single_profile.ok++;
	ncm_single_profile.payload_bytes += (u32)payload_bytes;
	ncm_single_profile.ntb_bytes += (u32)ntb_len;
	ncm_single_profile.clear_bytes += (u32)(ntb_len - payload_bytes);
	ncm_single_profile.batch_ntbs++;
	ncm_single_profile.batch_datagrams += selected_count;
	ncm_single_profile.batch_payload_bytes += (u32)payload_bytes;
	*out_len = ntb_len;
	*built_frames = (u16)selected_count;
	usb_os_unlock(ctx->mtx);
	return target;
}

static void *cdc_ncm_tx_fixup_batch(usbh_cdc_ncm_host_hal_t *dev,
				    const struct carbox_ncm_tx_batch *batch,
				    size_t *out_len)
{
	return cdc_ncm_tx_build_batch(dev, batch, dev->out_buf,
				      ((struct cdc_ncm_ctx *)dev->data[0])->tx_max,
				      out_len, &ncm_batch_sent_frames);
}

int carbox_ncm_tx_build_batch(const struct carbox_ncm_tx_batch *batch,
			      void *ntb_buffer, size_t ntb_capacity,
			      size_t *ntb_len, uint16_t *built_frames)
{
	extern usbh_cdc_ncm_host_hal_t usbh_cdc_ncm_host_user;
	void *result;
	struct cdc_ncm_ctx *ctx;

	if (ntb_len == NULL || built_frames == NULL) return -1;
	*ntb_len = 0U;
	*built_frames = 0U;
	if (batch == NULL || batch->magic != CARBOX_NCM_TX_BATCH_MAGIC ||
	    batch->frame_count < 1U) return -1;
	/* Match the compatibility wrapper's safe entry condition.  lwIP may emit
	 * startup traffic before NCM is ready, and disconnect can clear the context
	 * while queued frames still exist. */
	if (usbh_cdc_ncm_host_user.ncm_hw_connect == 0U ||
	    usbh_cdc_ncm_host_user.data[0] == 0UL) return -1;
	ctx = (struct cdc_ncm_ctx *)usbh_cdc_ncm_host_user.data[0];
	if (ctx == NULL || ntb_capacity > 0xffffU) return -1;
	/* Use the same outer lock order as the customer synchronous TX path:
	 * ctx->net, then the builder's ctx->mtx. */
	cdc_ncm_tx_lock(&usbh_cdc_ncm_host_user);
	result = cdc_ncm_tx_build_batch(&usbh_cdc_ncm_host_user, batch,
					(u8 *)ntb_buffer, ntb_capacity, ntb_len,
					built_frames);
	cdc_ncm_tx_unlock(&usbh_cdc_ncm_host_user);
	if (result == NULL) return -1;
	return 0;
}

int carbox_ncm_tx_send_prebuilt(void *ntb_buffer, size_t ntb_len)
{
	struct ncm_prebuilt_tx request;
	int status;

	if (ntb_buffer == NULL || ntb_len == 0U || ntb_len > 0xffffU ||
	    ncm_active_prebuilt != NULL) return -1;
	request.buffer = ntb_buffer;
	request.len = ntb_len;
	ncm_active_prebuilt = &request;
	status = usbh_cdc_ncm_send_data((u8 *)&request,
			NCM_PREBUILT_LENGTH_TAG | (u32)ntb_len);
	ncm_active_prebuilt = NULL;
	return status;
}

int carbox_ncm_tx_send_batch(const struct carbox_ncm_tx_batch *batch,
			     uint16_t *sent_frames)
{
	int status;

	if (sent_frames == NULL || batch == NULL ||
	    batch->magic != CARBOX_NCM_TX_BATCH_MAGIC ||
	    batch->frame_count < 2U ||
	    batch->frame_count > CARBOX_NCM_TX_BATCH_MAX_DATAGRAMS ||
	    ncm_active_batch != NULL) {
		ncm_single_profile.batch_reject++;
		return -1;
	}
	*sent_frames = 0U;
	ncm_active_batch = batch;
	ncm_batch_built = 0U;
	ncm_batch_sent_frames = 0U;
	status = usbh_cdc_ncm_send_data((u8 *)batch,
			NCM_BATCH_LENGTH_TAG | batch->frame_count);
	ncm_active_batch = NULL;
	if (ncm_batch_built == 0U)
		return CARBOX_NCM_TX_BATCH_RETRY_SINGLE;
	*sent_frames = ncm_batch_sent_frames;
	return status;
}

/*
 * Hardware-validated compatibility format used with the customer USB archive.
 * Each Ethernet frame is sent immediately as one NCM NTB16:
 *
 *   NTH16 (12 bytes) + NDP16/two DPE16 entries (16 bytes) + payload
 *
 * The second DPE is the required all-zero terminator.  Keeping this format
 * avoids the incomplete deferred-flush path while preserving the new
 * archive's cdc_ncm_ctx ABI.
 */
static void *cdc_ncm_tx_fixup_single(usbh_cdc_ncm_host_hal_t *dev,
				     const void *data, size_t len,
				     size_t *out_len)
{
	struct cdc_ncm_ctx *ctx;
	struct usb_cdc_ncm_nth16 *nth16;
	struct usb_cdc_ncm_ndp16 *ndp16;
	const size_t ndp_len = sizeof(*ndp16) +
			       sizeof(struct usb_cdc_ncm_dpe16) * 2U;
	const size_t payload_offset = sizeof(*nth16) + ndp_len;
	size_t ntb_len;

	if (out_len == NULL) {
		ncm_single_profile.reject_arg++;
		return NULL;
	}
	*out_len = 0U;
	if (dev == NULL) {
		ncm_single_profile.reject_dev++;
		return NULL;
	}
	ctx = (struct cdc_ncm_ctx *)dev->data[0];
	if (ctx == NULL || dev->out_buf == NULL) {
		ncm_single_profile.reject_dev++;
		return NULL;
	}
	if (data == NULL) {
		ncm_single_profile.reject_arg++;
		return NULL;
	}
	/* Subtraction form avoids size_t overflow before validating the NTB. */
	if (ctx->tx_max < payload_offset ||
	    len > ctx->tx_max - payload_offset ||
	    len > 0xffffU - payload_offset) {
		ncm_single_profile.reject_size++;
		return NULL;
	}
	ntb_len = payload_offset + len;

	usb_os_lock(ctx->mtx);
	/* Only the 28-byte NTH/NDP area needs clearing for the terminator DPE. */
	memset(dev->out_buf, 0, payload_offset);

	nth16 = (struct usb_cdc_ncm_nth16 *)dev->out_buf;
	nth16->dwSignature = cpu_to_le32(USB_CDC_NCM_NTH16_SIGN);
	nth16->wHeaderLength = cpu_to_le16(sizeof(*nth16));
	nth16->wSequence = cpu_to_le16(ctx->tx_seq++);
	nth16->wBlockLength = cpu_to_le16(ntb_len);
	nth16->wNdpIndex = cpu_to_le16(sizeof(*nth16));

	ndp16 = (struct usb_cdc_ncm_ndp16 *)(dev->out_buf + sizeof(*nth16));
	ndp16->dwSignature = cpu_to_le32(USB_CDC_NCM_NDP16_NOCRC_SIGN);
	ndp16->wLength = cpu_to_le16(ndp_len);
	ndp16->dpe16[0].wDatagramIndex = cpu_to_le16(payload_offset);
	ndp16->dpe16[0].wDatagramLength = cpu_to_le16(len);

	memcpy(dev->out_buf + payload_offset, data, len);
	ctx->tx_overhead += payload_offset;
	ctx->tx_ntbs++;
	ncm_single_profile.ok++;
	ncm_single_profile.payload_bytes += (u32)len;
	ncm_single_profile.ntb_bytes += (u32)ntb_len;
	ncm_single_profile.clear_bytes += (u32)payload_offset;
	*out_len = ntb_len;
	usb_os_unlock(ctx->mtx);
	return dev->out_buf;
}
#endif

void *cdc_ncm_tx_fixup(usbh_cdc_ncm_host_hal_t *dev, void *data, size_t len, gfp_t flags, size_t *out_len)
{
#if NCM_TX_COMPAT_SINGLE_DATAGRAM
	(void)flags;
	if (ncm_active_batch != NULL && data == (void *)ncm_active_batch &&
	    len == (NCM_BATCH_LENGTH_TAG | ncm_active_batch->frame_count)) {
		void *result = cdc_ncm_tx_fixup_batch(dev, ncm_active_batch, out_len);
		ncm_batch_built = result != NULL;
		return result;
	}
	if (ncm_active_prebuilt != NULL && data == (void *)ncm_active_prebuilt &&
	    len == (NCM_PREBUILT_LENGTH_TAG | ncm_active_prebuilt->len)) {
		*out_len = ncm_active_prebuilt->len;
		return ncm_active_prebuilt->buffer;
	}
	return cdc_ncm_tx_fixup_single(dev, data, len, out_len);
#else
    void *out_data = NULL;
    struct cdc_ncm_ctx *ctx;
    __MSG("%s\n", __func__);

	(void)flags;
	if (dev == NULL || out_len == NULL)
		goto error;
	ctx = (struct cdc_ncm_ctx *)dev->data[0];

    if (ctx == NULL)
        goto error;

	usb_os_lock(ctx->mtx);
#ifdef USE_STAT
    /* latency profiling + adaptive datagram aggregation */
    u64 t0 = usbh_get_timestamp(dev->host);
    if (ctx->tx_last_ts) {
        u32 gap = (u32)(t0 - ctx->tx_last_ts);
        ctx->tx_interval_sum += gap;
        if (gap > ctx->tx_interval_max) ctx->tx_interval_max = gap;
    }
    ctx->tx_last_ts = t0;
#endif
	__MSG("%s start len=%d\n", __func__,len);

    out_data = cdc_ncm_fill_tx_frame(dev, data, len, cpu_to_le32(USB_CDC_NCM_NDP16_NOCRC_SIGN), out_len);

#ifdef USE_STAT
    u64 t1 = usbh_get_timestamp(dev->host);
    u32 proc = (u32)(t1 - t0);
    ctx->tx_proc_sum += proc;
    if (proc > ctx->tx_proc_max) ctx->tx_proc_max = proc;
    ctx->tx_call_cnt++;
#endif
	__MSG("%s end len=%d\n", __func__,len);
	usb_os_unlock(ctx->mtx);
    return out_data;

error:
    return NULL;
#endif
}

/* exported lock helpers for hal.c — both callers hold ctx->mtx across bulk+sema */
void cdc_ncm_tx_lock(usbh_cdc_ncm_host_hal_t *dev) {
    struct cdc_ncm_ctx *ctx;
    if (!dev) return;
    ctx = (struct cdc_ncm_ctx *)dev->data[0];
    if (ctx) usb_os_lock(ctx->net);
}
void cdc_ncm_tx_unlock(usbh_cdc_ncm_host_hal_t *dev) {
    struct cdc_ncm_ctx *ctx;
    if (!dev) return;
    ctx = (struct cdc_ncm_ctx *)dev->data[0];
    if (ctx) usb_os_unlock(ctx->net);
}

#if !NCM_TX_COMPAT_SINGLE_DATAGRAM
#ifdef USE_TIMER


/* Linux-aligned: minimal ISR — only flag + wake bh; stats moved to bh context */
static void cdc_ncm_tx_timer_cb(usbh_cdc_ncm_host_hal_t *dev)
{
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	if (ctx) {
		ctx->bh_need_wake = 1;	/* defer sema_give to ISR tail */
		// cdc_ncm_txpath_bh(dev);
	}
}



static void cdc_ncm_tx_timeout_start(struct cdc_ncm_ctx *ctx, usbh_cdc_ncm_host_hal_t *dev)
{
    /* start timer, if not already started */
    if (ctx->tx_exit || !ctx->timer_ok) return;

	/* always restart to re-enable IRQ (ISR uses INTConfig DISABLE, not Cmd) */
	gtimer_stop(&ctx->send_timer);
	
	ctx->t_timer_send = usbh_get_timestamp(dev->host);	

	gtimer_reload(&ctx->send_timer, ctx->timer_interval);

	gtimer_start(&ctx->send_timer);
}




/* Linux-aligned: timer callback for deferred flush via tasklet-equivalent */
static void cdc_ncm_txpath_bh(unsigned long param)
{
	usbh_cdc_ncm_host_hal_t *dev = (usbh_cdc_ncm_host_hal_t *)param;
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];

	usb_os_lock(ctx->mtx);
	if (ctx->tx_timer_pending != 0) {
		ctx->tx_timer_pending--;
		cdc_ncm_tx_timeout_start(ctx, dev);
		usb_os_unlock(ctx->mtx);
	} else if (dev != NULL) {
		ctx->tx_reason_timeout++;
		usb_os_unlock(ctx->mtx);
		usbh_cdc_ncm_send_data(NULL, 0);
	} else {
		usb_os_unlock(ctx->mtx);
	}
}
#endif
#ifdef USE_THREAD
static void ncm_tx_bh_thread(void *param)
{
	usbh_cdc_ncm_host_hal_t *dev = (usbh_cdc_ncm_host_hal_t *)param;
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	while (!ctx->tx_exit) {
		usb_os_sema_take(ctx->bh_sema, USB_OS_SEMA_TIMEOUT);
		if (ctx->tx_exit) break;
		cdc_ncm_txpath_bh((unsigned long)dev);
	}
}
#endif

#ifdef USE_STAT
static void ncm_stat_thread(void *param)
{
	usbh_cdc_ncm_host_hal_t *dev = (usbh_cdc_ncm_host_hal_t *)param;
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	static u32 loop_seq;

	while (!ctx->tx_exit) {

		rtos_time_delay_ms(1000);


		loop_seq++;
		RTK_LOGS(TAG, "[STAT-S2] #%u woke\n", loop_seq);
		u32 calls   = ctx->tx_call_cnt;
		u32 intv_s  = ctx->tx_interval_sum;
		u32 intv_m  = ctx->tx_interval_max;
		u32 proc_s  = ctx->tx_proc_sum;
		u32 proc_m  = ctx->tx_proc_max;
		u32 send_s  = ctx->tx_send_sum;
		u32 send_m  = ctx->tx_send_max;

#ifdef USE_TIMER
		u32 timer_calls   = ctx->tx_timer_call_cnt;
		u32 timer_intv_s  = ctx->tx_timer_interval_sum;
		u32 timer_intv_m  = ctx->tx_timer_interval_max;
#endif


		RTK_LOGS(TAG, "[STAT-S3] #%u reading ts\n", loop_seq);
		/* idle gap warning: gap > 10ms since last tx_fixup */
		u32 idle_gap = ctx->tx_last_ts ?
			(u32)(usbh_get_timestamp(dev->host) - ctx->tx_last_ts) : 0;

		RTK_LOGS(TAG, "[STAT-S4] #%u ts ok, idle_gap=%dus\n", loop_seq, (int)idle_gap);
		u64 ntbs    = ctx->tx_ntbs;
		u64 dgrams  = ctx->tx_datagrams;
		static u64 s_last_ntbs, s_last_dgrams;
		u64 d_ntbs = ntbs - s_last_ntbs;
		u64 d_dgrams = dgrams - s_last_dgrams;
		s_last_ntbs = ntbs;
		s_last_dgrams = dgrams;


		ctx->tx_call_cnt = 0;
		ctx->tx_interval_sum = 0;
		ctx->tx_interval_max = 0;
		ctx->tx_proc_sum = 0;
		ctx->tx_proc_max = 0;
		ctx->tx_send_sum = 0;
		ctx->tx_send_max = 0;

#ifdef USE_TIMER
		ctx->tx_timer_call_cnt = 0;
		ctx->tx_timer_interval_sum = 0;
		ctx->tx_timer_interval_max = 0;	
#endif



		int ntb_avg = d_ntbs ? (int)((d_dgrams * 10 + d_ntbs / 2) / d_ntbs) : 0;
		if (idle_gap > 10000) {
			RTK_LOGS(TAG, "[STAT] idle gap=%dus (>10ms)\n", idle_gap);
		}


		RTK_LOGS(TAG,
			"[STAT-S5] #%u agg=%d calls=%d intv=%dus/%dus "
			"proc=%dus/%dus send=%dus/%dus ntb=%d(%d.%dd/ntb)\n",
			loop_seq, ctx->tx_max_datagrams, calls,
			calls ? intv_s / calls : 0, intv_m,
			calls ? proc_s / calls : 0, proc_m,
			calls ? send_s / calls : 0, send_m,
			(int)d_ntbs, ntb_avg / 10, ntb_avg % 10);


#ifdef USE_TIMER		
		RTK_LOGS(TAG,
			"[STAT-S6] #%u timer: pending=%d "
			"calls=%d lat=%dus/%dus tick=%d\n",
			loop_seq,
			ctx->tx_timer_pending,
			timer_calls,
			timer_calls ? timer_intv_s / timer_calls : 0, timer_intv_m,
			gtimer_read_tick(&ctx->send_timer));
#endif

	}
}

#endif
#endif

void ncm_timer_init(usbh_cdc_ncm_host_hal_t *dev)
{
	struct cdc_ncm_ctx *ctx;
	int ret;

	if (dev == NULL)
		return;
	ctx = (struct cdc_ncm_ctx *)dev->data[0];
	if (ctx == NULL)
		return;
#if NCM_TX_COMPAT_SINGLE_DATAGRAM && NCM_TX_ABI_TIMER_LAYOUT
	/* Preserve the archive ABI while explicitly disabling legacy timer use. */
	ctx->timer_ok = 0U;
	ctx->bh_need_wake = 0U;
	ctx->tx_timer_call_cnt = 0U;
	ctx->tx_timer_last = 0U;
	ctx->tx_timer_interval_sum = 0U;
	ctx->tx_timer_interval_max = 0U;
	ctx->t_timer_send = 0U;
#endif
	ret = usb_os_lock_create(&ctx->mtx);
#if NCM_TX_COMPAT_SINGLE_DATAGRAM
	if (ret != 0)
		ncm_single_profile.lock_fail++;
	ncm_single_profile.cfg_tx_max = ctx->tx_max;
	ncm_single_profile.cfg_max_datagram = ctx->max_datagram_size;
	ncm_single_profile.cfg_max_ndp = ctx->max_ndp_size;
	ncm_single_profile.cfg_max_datagrams = ctx->tx_max_datagrams;
	ncm_single_profile.cfg_modulus = ctx->tx_modulus;
	ncm_single_profile.cfg_remainder = ctx->tx_remainder;
#endif
	ret = usb_os_lock_create(&ctx->net);
#if NCM_TX_COMPAT_SINGLE_DATAGRAM
	if (ret != 0)
		ncm_single_profile.lock_fail++;
#endif
	(void)ret;
	// usb_os_lock_delete(ctx->mtx);
	ctx->tx_exit = 0;
    RTK_LOGS(TAG, "[NCM] -------------------------------------------\n");
#ifdef USE_TIMER

	ctx->tx_reason_timeout = 0;
	ctx->bh_need_wake = 0;
	memset(&ctx->send_timer, 0, sizeof(ctx->send_timer));
#if NCM_TX_COMPAT_SINGLE_DATAGRAM
	/* Single-datagram mode has no buffered NTB, so no flush timer is needed. */
	ctx->timer_ok = 0;
	ncm_single_profile.timer_init++;
#else
	gtimer_init(&ctx->send_timer, 0xFF);
	if (ctx->send_timer.timer_adp.tmr_ba == NULL ||
	    ctx->send_timer.timer_adp.tg_ba == NULL) {
		ctx->timer_ok = 0;
		RTK_LOGS(TAG, "[NCM] gtimer init failed, tx timer disabled\n");
	} else {
		ctx->timer_ok = 1;
		gtimer_start_one_shout(&ctx->send_timer, ctx->timer_interval,
		     (void *)cdc_ncm_tx_timer_cb, (uint32_t)dev);
	}
#endif


	
	ctx->tx_timer_call_cnt = 0;
	ctx->tx_timer_interval_sum = 0;
	ctx->tx_timer_interval_max = 0;	
	ctx->t_timer_send = 0;

#endif

	ctx->tx_ntbs = 0;
	ctx->tx_overhead = 0;
#ifdef USE_THREAD
    usb_os_sema_create(&ctx->bh_sema);		
	ctx->bh_created  = 0;
#endif
	
	#ifdef USE_STAT
	ctx->tx_datagrams = 0;
	ctx->tx_payload_bytes = 0;
	ctx->tx_last_ts = 0;
	ctx->tx_call_cnt = 0;
	ctx->tx_interval_sum = 0;
	ctx->tx_interval_max = 0;

	ctx->tx_proc_sum = 0;
	ctx->tx_proc_max = 0;
	ctx->tx_send_sum = 0;
	ctx->tx_send_max = 0;

	ctx->stat_started = 0;


	#endif
}

void ncm_stat_send_latency(usbh_cdc_ncm_host_hal_t *dev, u32 elapsed_us)
{
#ifdef USE_STAT	
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	if (!ctx) return;
	ctx->tx_send_sum += elapsed_us;
	if (elapsed_us > ctx->tx_send_max) ctx->tx_send_max = elapsed_us;
#endif
}

void carbox_ncm_tx_single_profile_report(unsigned long sequence)
{
#if NCM_TX_COMPAT_SINGLE_DATAGRAM
	u32 ok = ncm_single_profile.ok;
	u32 payload = ncm_single_profile.payload_bytes;
	u32 datagrams = ok - ncm_single_profile.batch_ntbs +
		ncm_single_profile.batch_datagrams;

	rt_printf("[NCMTXOPT][%lu] mode=batch%lu ntb/datagrams=%lu/%lu "
		  "reject dev/arg/size=%lu/%lu/%lu payload/ntb/meta_clear/clear_saved="
		  "%lu/%lu/%lu/%luB batch ntb/datagrams/payload/reject="
		  "%lu/%lu/%lu/%lu timer compiled/hw_start=%lu/%lu lock_fail=%lu\r\n",
		  sequence, (unsigned long)CARBOX_NCM_TX_BATCH_MAX_DATAGRAMS,
		  (unsigned long)ok, (unsigned long)datagrams,
		  (unsigned long)ncm_single_profile.reject_dev,
		  (unsigned long)ncm_single_profile.reject_arg,
		  (unsigned long)ncm_single_profile.reject_size,
		  (unsigned long)payload,
		  (unsigned long)ncm_single_profile.ntb_bytes,
		  (unsigned long)ncm_single_profile.clear_bytes,
		  (unsigned long)payload,
		  (unsigned long)ncm_single_profile.batch_ntbs,
		  (unsigned long)ncm_single_profile.batch_datagrams,
		  (unsigned long)ncm_single_profile.batch_payload_bytes,
		  (unsigned long)ncm_single_profile.batch_reject,
		  (unsigned long)ncm_single_profile.timer_init,
		  (unsigned long)ncm_single_profile.timer_start,
		  (unsigned long)ncm_single_profile.lock_fail);
	rt_printf("[NCMTXCFG][%lu] tx_max/max_datagram/max_ndp/max_datagrams="
		  "%lu/%lu/%lu/%lu modulus/remainder=%lu/%lu\r\n",
		  sequence,
		  (unsigned long)ncm_single_profile.cfg_tx_max,
		  (unsigned long)ncm_single_profile.cfg_max_datagram,
		  (unsigned long)ncm_single_profile.cfg_max_ndp,
		  (unsigned long)ncm_single_profile.cfg_max_datagrams,
		  (unsigned long)ncm_single_profile.cfg_modulus,
		  (unsigned long)ncm_single_profile.cfg_remainder);
	rt_printf("[NCMTXGDMA][%lu] enabled=%u attempt/success/fallback=%lu/%lu/%lu "
		  "bytes dma/cpu_edge/cpu_fallback=%lu/%lu/%lu "
		  "descriptors/batches=%lu/%lu\r\n",
		  sequence, (unsigned int)CONFIG_NCM_TX_LINKED_GDMA,
		  (unsigned long)ncm_single_profile.gdma_attempts,
		  (unsigned long)ncm_single_profile.gdma_success,
		  (unsigned long)ncm_single_profile.gdma_fallback,
		  (unsigned long)ncm_single_profile.gdma_bytes,
		  (unsigned long)ncm_single_profile.gdma_cpu_edge_bytes,
		  (unsigned long)ncm_single_profile.gdma_cpu_fallback_bytes,
		  (unsigned long)ncm_single_profile.gdma_blocks,
		  (unsigned long)ncm_single_profile.gdma_batches);
#else
	(void)sequence;
#endif
}
