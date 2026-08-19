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

#ifndef NCM_TX_COMPAT_SINGLE_DATAGRAM
#define NCM_TX_COMPAT_SINGLE_DATAGRAM 0
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

#if defined(USE_TIMER) || defined(USE_STAT)
extern u64 usbh_get_timestamp(struct usb_host *host);
#endif

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



#if NCM_TX_COMPAT_SINGLE_DATAGRAM
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
	struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	struct usb_cdc_ncm_nth16 *nth16;
	struct usb_cdc_ncm_ndp16 *ndp16;
	const size_t ndp_len = sizeof(*ndp16) +
			       sizeof(struct usb_cdc_ncm_dpe16) * 2U;
	const size_t payload_offset = sizeof(*nth16) + ndp_len;
	const size_t ntb_len = payload_offset + len;

	if (ctx == NULL || dev->out_buf == NULL || data == NULL ||
	    out_len == NULL || ntb_len > ctx->tx_max || ntb_len > 0xffffU) {
		return NULL;
	}

	usb_os_lock(ctx->mtx);
	memset(dev->out_buf, 0, ntb_len);

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
	*out_len = ntb_len;
	usb_os_unlock(ctx->mtx);
	return dev->out_buf;
}
#endif

void *cdc_ncm_tx_fixup(usbh_cdc_ncm_host_hal_t *dev, void *data, size_t len, gfp_t flags, size_t *out_len)
{
    void *out_data = NULL;
    struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
    __MSG("%s\n", __func__);

#if NCM_TX_COMPAT_SINGLE_DATAGRAM
	(void)flags;
	return cdc_ncm_tx_fixup_single(dev, data, len, out_len);
#endif

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
}

/* exported lock helpers for hal.c — both callers hold ctx->mtx across bulk+sema */
void cdc_ncm_tx_lock(usbh_cdc_ncm_host_hal_t *dev) {
    struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
    if (ctx) usb_os_lock(ctx->net);
}
void cdc_ncm_tx_unlock(usbh_cdc_ncm_host_hal_t *dev) {
    struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
    if (ctx) usb_os_unlock(ctx->net);
}

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

void ncm_timer_init(usbh_cdc_ncm_host_hal_t *dev)
{
    struct cdc_ncm_ctx *ctx = (struct cdc_ncm_ctx *)dev->data[0];
	int ret;
	ret = usb_os_lock_create(&ctx->mtx);
	ret = usb_os_lock_create(&ctx->net);
	// usb_os_lock_delete(ctx->mtx);
	ctx->tx_exit = 0;
    RTK_LOGS(TAG, "[NCM] -------------------------------------------\n");
#ifdef USE_TIMER

	ctx->tx_reason_timeout = 0;
	ctx->bh_need_wake = 0;
	memset(&ctx->send_timer, 0, sizeof(ctx->send_timer));
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
