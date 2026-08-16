/*
 * Source recovery of the RTL8195B freertos_skbuff.o ABI.
 *
 * The fixed pools, public symbols, sk_buff layout, and reference-count word
 * location match the vendor object.  The intentional functional correction
 * is that dynamically allocated skb data obeys the same shared reference
 * count as pool data, and skb_clone() preserves dyalloc_flag.  RX descriptor
 * buffers can therefore transfer through the normal clone/free pipeline
 * without a free-symbol hook or a use-after-free.
 */

#include <basic_types.h>
#include <osdep_service.h>
#include <skbuff.h>

#define RECOVERED_SKB_DATA_SIZE       1666U
#define RECOVERED_SKB_DATA_COUNT      24U
#define RECOVERED_SKB_WRAPPER_COUNT   26U
#define RECOVERED_SKB_REF_OFFSET      1668U
#define RECOVERED_SKB_REF_END         1672U

#ifndef CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY
#define CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY 0
#endif

#ifndef CONFIG_RECOVERED_SKBUFF_DMA_SHARED_ONLY
#define CONFIG_RECOVERED_SKBUFF_DMA_SHARED_ONLY 0
#endif

/* dyalloc_flag == 1 is the vendor's ordinary dynamic allocation.  Reserve 2
 * for descriptor-backed allocations whose data may outlive the first skb
 * wrapper through the existing clone path. */
#define RECOVERED_SKB_DMA_SHARED 2

#define RECOVERED_ALIGN4(value) (((value) + 3U) & ~3U)

/* Preserve the vendor archive's section ABI.  The product linker treats the
 * .wlan.* namespace specially, so matching symbols in ordinary .text/.bss
 * are not placement-equivalent even when their C ABI is identical. */
#define WLAN_TEXT(symbol) \
	__attribute__((section(".wlan.text." #symbol)))
#define WLAN_BSS(symbol) \
	__attribute__((section(".wlan.bss." #symbol ",\"aw\",%nobits@")))
#define WLAN_DATA(symbol) \
	__attribute__((section(".wlan.data." #symbol)))

struct recovered_skb_wrapper {
	struct list_head list;
	struct sk_buff skb;
};

struct recovered_skb_data {
	struct list_head list;
	u8 data[RECOVERED_SKB_DATA_SIZE];
	atomic_t ref;
};

#ifndef RECOVERED_SKBUFF_HOST_TEST
typedef char recovered_skb_size_must_be_40[
	(sizeof(struct sk_buff) == 40U) ? 1 : -1];
typedef char recovered_wrapper_size_must_be_48[
	(sizeof(struct recovered_skb_wrapper) == 48U) ? 1 : -1];
typedef char recovered_data_size_must_be_1680[
	(sizeof(struct recovered_skb_data) == 1680U) ? 1 : -1];
#endif

/* Weak defaults reproduce the vendor archive.  A strong rtw_opt_skbuf.o may
 * replace these with the product's larger pool without changing this ABI. */
int max_local_skb_num __attribute__((weak)) WLAN_DATA(max_local_skb_num) =
	RECOVERED_SKB_WRAPPER_COUNT;
int max_skb_buf_num __attribute__((weak)) WLAN_DATA(max_skb_buf_num) =
	RECOVERED_SKB_DATA_COUNT;
struct recovered_skb_wrapper skb_pool[RECOVERED_SKB_WRAPPER_COUNT]
	__attribute__((weak)) WLAN_BSS(skb_pool);
struct recovered_skb_data skb_data_pool[RECOVERED_SKB_DATA_COUNT]
	__attribute__((weak, aligned(32))) WLAN_BSS(skb_data_pool);

struct list_head skbdata_list WLAN_BSS(skbdata_list);
int skbdata_used_num WLAN_BSS(skbdata_used_num);
int max_skbdata_used_num WLAN_BSS(max_skbdata_used_num);
int skbbuf_used_num WLAN_BSS(skbbuf_used_num);
int max_skbbuf_used_num WLAN_BSS(max_skbbuf_used_num);

static struct list_head wrapper_skbbuf_list WLAN_BSS(wrapper_skbbuf_list);
static int skb_fail_count WLAN_BSS(skb_fail_count);

extern int printf(const char *format, ...);
extern void cli(void);

static void recovered_list_init(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static void recovered_pool_put(struct list_head *entry,
			       struct list_head *head)
{
	struct list_head *last = head->prev;

	entry->next = head;
	entry->prev = last;
	last->next = entry;
	head->prev = entry;
}

WLAN_TEXT(get_buf_from_poll)
static void *get_buf_from_poll(struct list_head *head, int *used)
{
	struct list_head *entry;

	if (head->next == head) {
		return NULL;
	}
	entry = head->next;
	entry->next->prev = head;
	head->next = entry->next;
	entry->next = entry;
	entry->prev = entry;
	(*used)++;
	return (u8 *)entry + sizeof(*entry);
}

WLAN_TEXT(skb_fail_inc) void skb_fail_inc(void)
{
	save_and_cli();
	skb_fail_count++;
	restore_flags();
}

WLAN_TEXT(skb_fail_get_and_rst) int skb_fail_get_and_rst(void)
{
	int result;

	save_and_cli();
	result = skb_fail_count;
	skb_fail_count = 0;
	restore_flags();
	return result;
}

WLAN_TEXT(init_skb_pool) void init_skb_pool(void)
{
	int count = max_local_skb_num;
	int i;

	memset(skb_pool, 0, (size_t)count * sizeof(skb_pool[0]));
	recovered_list_init(&wrapper_skbbuf_list);
	for (i = 0; i < count; i++) {
		recovered_pool_put(&skb_pool[i].list, &wrapper_skbbuf_list);
	}
	skbbuf_used_num = 0;
	max_skbbuf_used_num = 0;
}

WLAN_TEXT(skb_data_size_check)
void __attribute__((weak)) skb_data_size_check(int size)
{
	if (size != (int)RECOVERED_SKB_DATA_SIZE) {
		printf("\n\rAssert(%d == %u) failed in recovered freertos_skbuff.c",
		       size, (unsigned int)RECOVERED_SKB_DATA_SIZE);
		cli();
		for (;;) {
		}
	}
}

WLAN_TEXT(init_skb_data_pool)
void __attribute__((weak)) init_skb_data_pool(void)
{
	int count = max_skb_buf_num;
	int i;

	skb_data_size_check((int)RECOVERED_SKB_DATA_SIZE);
	memset(skb_data_pool, 0, (size_t)count * sizeof(skb_data_pool[0]));
	recovered_list_init(&skbdata_list);
	for (i = 0; i < count; i++) {
		recovered_pool_put(&skb_data_pool[i].list, &skbdata_list);
	}
	skbdata_used_num = 0;
	max_skbdata_used_num = 0;
}

WLAN_TEXT(deinit_skb_data_pool)
void __attribute__((weak)) deinit_skb_data_pool(void)
{
}

WLAN_TEXT(alloc_skb) struct sk_buff *alloc_skb(unsigned int length)
{
	struct sk_buff *skb;
	u8 *head;
	unsigned int allocation_size;
	int dynamic;

	save_and_cli();
	skb = (struct sk_buff *)get_buf_from_poll(&wrapper_skbbuf_list,
						  &skbbuf_used_num);
	restore_flags();
	if (skb == NULL) {
		printf("alloc_skb: wrapper pool empty\n");
		skb_fail_inc();
		return NULL;
	}
	memset(skb, 0, sizeof(*skb));

	dynamic = length > RECOVERED_SKB_DATA_SIZE;
	if (dynamic) {
#if CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY
		/* Preserve the vendor's exact allocation/end convention. */
		allocation_size = RECOVERED_ALIGN4(length);
		head = (u8 *)rtw_zmalloc(length);
#else
		allocation_size = RECOVERED_ALIGN4(length);
		if (allocation_size < RECOVERED_SKB_REF_END) {
			allocation_size = RECOVERED_SKB_REF_END;
		}
		head = (u8 *)rtw_zmalloc(allocation_size);
#endif
	} else {
		save_and_cli();
		head = (u8 *)get_buf_from_poll(&skbdata_list,
						 &skbdata_used_num);
		restore_flags();
		allocation_size = RECOVERED_ALIGN4(length);
	}
	if (head == NULL) {
		printf("alloc_skb: data allocation failed\n");
		save_and_cli();
		recovered_pool_put((struct list_head *)((u8 *)skb -
							 sizeof(struct list_head)),
				   &wrapper_skbbuf_list);
		skbbuf_used_num--;
		restore_flags();
#if !CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY
		skb_fail_inc();
#endif
		return NULL;
	}

	skb->head = head;
	skb->data = head;
	skb->tail = head;
	skb->end = head + allocation_size;
	skb->dyalloc_flag = dynamic;
	ATOMIC_SET((atomic_t *)(head + RECOVERED_SKB_REF_OFFSET), 1);
	if (skbbuf_used_num > max_skbbuf_used_num) {
		max_skbbuf_used_num = skbbuf_used_num;
	}
	if (!dynamic && skbdata_used_num > max_skbdata_used_num) {
		max_skbdata_used_num = skbdata_used_num;
	}
	return skb;
}

WLAN_TEXT(kfree_skb) void kfree_skb(struct sk_buff *skb)
{
	u8 *head;
	int release_data;
	int dynamic;

	if (skb == NULL) {
		return;
	}
	save_and_cli();
	head = skb->head;
	dynamic = skb->dyalloc_flag;
	release_data = 0;
#if CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY
	if (CONFIG_RECOVERED_SKBUFF_DMA_SHARED_ONLY &&
	    dynamic == RECOVERED_SKB_DMA_SHARED) {
		release_data = ATOMIC_DEC_AND_TEST(
			(atomic_t *)(head + RECOVERED_SKB_REF_OFFSET));
		if (release_data) {
			skb->dyalloc_flag = 0;
			rtw_mfree(head, 0);
		}
	} else if (dynamic == 1) {
		skb->dyalloc_flag = 0;
		rtw_mfree(head, 0);
	} else if (head != NULL) {
		release_data = ATOMIC_DEC_AND_TEST(
			(atomic_t *)(head + RECOVERED_SKB_REF_OFFSET));
		if (release_data) {
			recovered_pool_put((struct list_head *)(head -
							 sizeof(struct list_head)),
				   &skbdata_list);
			skbdata_used_num--;
		}
	}
#else
	if (head != NULL) {
		release_data = ATOMIC_DEC_AND_TEST(
			(atomic_t *)(head + RECOVERED_SKB_REF_OFFSET));
	}
	if (release_data) {
		if (dynamic) {
			skb->dyalloc_flag = 0;
			rtw_mfree(head, 0);
		} else {
			recovered_pool_put((struct list_head *)(head -
							 sizeof(struct list_head)),
				   &skbdata_list);
			skbdata_used_num--;
		}
	}
#endif
	recovered_pool_put((struct list_head *)((u8 *)skb -
						 sizeof(struct list_head)),
			   &wrapper_skbbuf_list);
	skbbuf_used_num--;
	restore_flags();
}

struct net_device;

WLAN_TEXT(kfree_skb_chk_key)
void kfree_skb_chk_key(struct sk_buff *skb, struct net_device *root_dev)
{
	(void)root_dev;
	kfree_skb(skb);
}

WLAN_TEXT(skb_put)
unsigned char *skb_put(struct sk_buff *skb, unsigned int len)
{
	u8 *old_tail = skb->tail;

	skb->tail += len;
	skb->len += len;
	if (skb->tail > skb->end) {
		printf("skb_put overflow in recovered freertos_skbuff.c\n");
		cli();
		for (;;) {
		}
	}
	return old_tail;
}

WLAN_TEXT(skb_reserve) void skb_reserve(struct sk_buff *skb, unsigned int len)
{
	skb->data += len;
	skb->tail += len;
}

WLAN_TEXT(dev_alloc_skb)
struct sk_buff *dev_alloc_skb(unsigned int length, unsigned int reserve_len)
{
	struct sk_buff *skb;
	unsigned int aligned_reserve = RECOVERED_ALIGN4(reserve_len);

#if !CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY
	if (length > 0xffffffffU - aligned_reserve) {
		return NULL;
	}
#endif
	skb = alloc_skb(length + aligned_reserve);
	if (skb != NULL) {
		skb_reserve(skb, aligned_reserve);
	}
	return skb;
}

WLAN_TEXT(skb_assign_buf)
void skb_assign_buf(struct sk_buff *skb, unsigned char *buf,
		    unsigned int len)
{
	skb->head = buf;
	skb->data = buf;
	skb->tail = buf;
	skb->end = buf + len;
}

WLAN_TEXT(skb_tail_pointer)
unsigned char *skb_tail_pointer(const struct sk_buff *skb)
{
	return skb->tail;
}

WLAN_TEXT(skb_end_pointer)
unsigned char *skb_end_pointer(const struct sk_buff *skb)
{
	return skb->end;
}

WLAN_TEXT(skb_set_tail_pointer)
void skb_set_tail_pointer(struct sk_buff *skb, const int offset)
{
	skb->tail = skb->data + offset;
}

WLAN_TEXT(skb_pull)
unsigned char *skb_pull(struct sk_buff *skb, unsigned int len)
{
	if (len > skb->len) {
		return NULL;
	}
	skb->len -= len;
	skb->data += len;
	return skb->data;
}

WLAN_TEXT(skb_copy)
struct sk_buff *skb_copy(const struct sk_buff *source, int gfp_mask,
			 unsigned int reserve_len)
{
	struct sk_buff *copy;
	u8 *destination;

	(void)gfp_mask;
	copy = dev_alloc_skb(source->len, reserve_len);
	if (copy == NULL) {
		return NULL;
	}
	destination = skb_put(copy, source->len);
	memcpy(destination, source->data, source->len);
	copy->dev = source->dev;
	return copy;
}

WLAN_TEXT(skb_clone)
struct sk_buff *skb_clone(struct sk_buff *source, int gfp_mask)
{
	struct sk_buff *clone;

	(void)gfp_mask;
#if CONFIG_RECOVERED_SKBUFF_VENDOR_PARITY
	save_and_cli();
	clone = (struct sk_buff *)get_buf_from_poll(&wrapper_skbbuf_list,
						    &skbbuf_used_num);
	restore_flags();
	if (clone == NULL) {
		printf("skb_clone: wrapper pool empty\n");
		return NULL;
	}
	memset(clone, 0, sizeof(*clone));
	ATOMIC_INC((atomic_t *)(source->head + RECOVERED_SKB_REF_OFFSET));
	clone->head = source->head;
	clone->data = source->data;
	clone->tail = source->tail;
	clone->end = source->end;
	clone->dev = source->dev;
	clone->len = source->len;
	if (CONFIG_RECOVERED_SKBUFF_DMA_SHARED_ONLY &&
	    source->dyalloc_flag == RECOVERED_SKB_DMA_SHARED) {
		clone->dyalloc_flag = RECOVERED_SKB_DMA_SHARED;
	}
	return clone;
#else
	if (source == NULL || source->head == NULL) {
		return NULL;
	}
	save_and_cli();
	clone = (struct sk_buff *)get_buf_from_poll(&wrapper_skbbuf_list,
						    &skbbuf_used_num);
	if (clone != NULL) {
		ATOMIC_INC((atomic_t *)(source->head +
					      RECOVERED_SKB_REF_OFFSET));
	}
	restore_flags();
	if (clone == NULL) {
		printf("skb_clone: wrapper pool empty\n");
		skb_fail_inc();
		return NULL;
	}
	memset(clone, 0, sizeof(*clone));
	clone->head = source->head;
	clone->data = source->data;
	clone->tail = source->tail;
	clone->end = source->end;
	clone->dev = source->dev;
	clone->len = source->len;
	/* Vendor omitted this field.  It is required for shared dynamic data. */
	clone->dyalloc_flag = source->dyalloc_flag;
	if (skbbuf_used_num > max_skbbuf_used_num) {
		max_skbbuf_used_num = skbbuf_used_num;
	}
	return clone;
#endif
}
