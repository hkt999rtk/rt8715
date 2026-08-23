#include "touch_path_profiler.h"
#include "airplay_mutex_profiler.h"
#include "car_ack_response_cache.h"

#ifndef CONFIG_TOUCH_PATH_PROFILE
#define CONFIG_TOUCH_PATH_PROFILE 0
#endif

#ifndef CONFIG_TOUCH_MOVE_SAMPLE_HZ
#define CONFIG_TOUCH_MOVE_SAMPLE_HZ 0
#endif

#ifndef CONFIG_TOUCH_PATH_REPORT_DETAIL
#define CONFIG_TOUCH_PATH_REPORT_DETAIL 0
#endif

#ifndef CONFIG_CAR_ACK_TCP_PROFILE
#define CONFIG_CAR_ACK_TCP_PROFILE 0
#endif

#ifndef CONFIG_IPHONE_HTTP_RX_PROFILE
#define CONFIG_IPHONE_HTTP_RX_PROFILE 0
#endif

#ifndef CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH
#define CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH 0
#endif

#ifndef CONFIG_AIRPLAY_HID_HTTP_BYPASS
#define CONFIG_AIRPLAY_HID_HTTP_BYPASS 0
#endif


#ifndef CONFIG_GMT_TIME_PROFILE
#define CONFIG_GMT_TIME_PROFILE 0
#endif

#if CONFIG_TOUCH_PATH_PROFILE || CONFIG_AIRPLAY_HID_HTTP_BYPASS

#include <string.h>
#if CONFIG_GMT_TIME_PROFILE
#include <time.h>
#endif

#include "FreeRTOS.h"
#include "task.h"
#include "diag.h"
#include "hal_timer.h"
#include "lwip/sockets.h"

#define TOUCH_HTTP_SLOTS            128U
#define TOUCH_READ_ORIGINS            4U
#define TOUCH_HID_BYTE0_SLOTS         8U
#define TOUCH_HID_UID_SLOTS           4U
/* HTTPMessage statusCode offset verified against this vendor archive. */
#define TOUCH_HTTP_STATUS_OFFSET  0x478U
#define TOUCH_OSSTATUS_WOULD_BLOCK    11
#define TOUCH_READ_PAIR_MAX_AGE_US 1000000U
#define TOUCH_HTTP_STALE_US       30000000U
#define TOUCH_RELEASE_DEBOUNCE_US     50000U
#define TOUCH_RX_DRAIN_GAP_US          5000U
#define TOUCH_PATH_VERBOSE_REPORT         0

#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
#define TOUCH_HTTP_BYPASS_DRAIN_BYTES       512U
#define TOUCH_HTTP_BYPASS_RETRY_NS      1000000LL
#define TOUCH_HTTP_BYPASS_FINAL_DRAIN_NS 40000000LL
#define TOUCH_HTTP_MESSAGE_HEADER_LEN      0x410U
#define TOUCH_HTTP_MESSAGE_BODY_LEN_OFFSET 0x4a0U
#define TOUCH_HTTP_MESSAGE_CLOSE_OFFSET    0x499U
#define TOUCH_HTTP_MESSAGE_WRITE_IOV       0x8bcU
#define TOUCH_HTTP_MESSAGE_WRITE_IOVCNT    0x8d0U
#define TOUCH_NETTRANSPORT_PLAIN_CUR       0x4028U
#define TOUCH_NETTRANSPORT_PLAIN_END       0x402cU
#endif


#if (CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0) || \
	CONFIG_AIRPLAY_HID_HTTP_BYPASS
/* Offsets verified against the linked HTTPClient/HTTPMessage implementation.
 * The wrapper deliberately uses only the public writer and the client's own
 * serial dispatch queue; it does not patch the customer archive. */
#define TOUCH_HTTP_CLIENT_QUEUE_OFFSET      0x008U
#define TOUCH_HTTP_CLIENT_STATE_OFFSET      0x038U
#define TOUCH_HTTP_CLIENT_WRITE_CTX_OFFSET  0x470U
#define TOUCH_HTTP_CLIENT_WRITE_F_OFFSET    0x480U
#define TOUCH_HTTP_CLIENT_HEAD_OFFSET       0x498U
#define TOUCH_HTTP_MESSAGE_NEXT_OFFSET      0x008U
#endif
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
#define TOUCH_HTTP_CLIENT_READING_RESPONSE      4U
#define TOUCH_HTTP_PIPE_NONE                    0U
#define TOUCH_HTTP_PIPE_QUEUED                  1U
#define TOUCH_HTTP_PIPE_PREWRITTEN              2U
#define TOUCH_HTTP_PIPE_PARTIAL                 3U
#endif

#if CONFIG_TOUCH_MOVE_SAMPLE_HZ > 0
#define TOUCH_MOVE_SAMPLE_PERIOD_US \
	((1000000U + CONFIG_TOUCH_MOVE_SAMPLE_HZ - 1U) / \
	 CONFIG_TOUCH_MOVE_SAMPLE_HZ)
#endif

typedef struct touch_path_stage_s {
	uint64_t sum_us;
	uint32_t max_us;
	uint32_t samples;
} touch_path_stage_t;

typedef struct touch_path_tail_s {
	uint32_t gt_1ms;
	uint32_t gt_3ms;
	uint32_t gt_5ms;
	uint32_t gt_10ms;
	uint32_t gt_16ms;
} touch_path_tail_t;

typedef struct touch_hid_byte0_stat_s {
	uint32_t count;
	uint32_t bytes;
	uint8_t byte0;
	uint8_t min_len;
	uint8_t max_len;
	uint8_t valid;
} touch_hid_byte0_stat_t;

typedef struct touch_hid_uid_stat_s {
	uint32_t uid;
	uint32_t count;
	uint32_t bytes;
	uint8_t min_len;
	uint8_t max_len;
	uint8_t valid;
} touch_hid_uid_stat_t;

typedef struct touch_path_stats_s {
	uint32_t car_hid_commands;
	uint32_t car_hid_bytes;
	uint32_t car_touch_reports;
	uint32_t car_non_touch_hid;
	uint32_t car_touch_forwarded;
	uint32_t car_touch_suppressed;
	uint32_t local_touch_calls;
	uint32_t iphone_hid_calls;
	uint32_t iphone_touch_calls;
	uint32_t iphone_hid_event_matched;
	uint32_t iphone_hid_bytes;
	uint32_t iphone_hid_len_le4;
	uint32_t iphone_hid_len_eq5;
	uint32_t iphone_hid_len_6_8;
	uint32_t iphone_hid_len_9_16;
	uint32_t iphone_hid_len_17_32;
	uint32_t iphone_hid_len_33_64;
	uint32_t iphone_hid_len_65_128;
	uint32_t iphone_hid_len_gt128;
	uint32_t iphone_hid_byte0_overflow;
	uint32_t car_hid_min_length;
	uint32_t car_hid_max_length;
	uint32_t parse_errors;
	uint32_t iphone_hid_errors;
	uint32_t event_overwrites;
	uint32_t unmatched_touch;
	uint32_t standalone_forward;
	uint32_t standalone_hid;
	uint32_t forwarded_without_hid;
	uint32_t car_type_matches;
	uint32_t car_type_overwrites;
	uint32_t car_read_pairs;
	uint32_t car_read_misses;
	uint32_t car_ack_write_calls;
	uint32_t car_ack_write_complete;
	uint32_t car_ack_write_deferred;
	uint32_t car_ack_write_errors;
	uint32_t car_rx_pending_samples;
	uint32_t car_rx_pending_nonzero;
	uint32_t car_rx_pending_max;
	uint32_t car_rx_pending_ioctl_errors;
	uint64_t car_rx_pending_sum;
	uint32_t car_rx_gap_le_1ms;
	uint32_t car_rx_gap_1_5ms;
	uint32_t car_rx_gap_5_15ms;
	uint32_t car_rx_gap_15_30ms;
	uint32_t car_rx_gap_gt_30ms;
	uint32_t car_rx_drain_streak_max;
	uint32_t dispatch_calls;
	uint32_t dispatch_missing;
	uint32_t http_enqueued;
	uint32_t http_enqueue_errors;
	uint32_t http_slot_full;
	uint32_t http_stale_expired;
	uint32_t http_write_calls;
	uint32_t http_write_complete;
	uint32_t http_write_deferred;
	uint32_t http_write_errors;
	uint32_t http_response_calls;
	uint32_t http_response_complete;
	uint32_t http_response_2xx;
	uint32_t http_response_non_2xx;
	uint32_t http_orphan_write;
	uint32_t http_orphan_response;
	uint32_t http_depth_peak;
	uint32_t http_write_while_pending;
	uint32_t http_wire_inflight_peak;
	uint32_t http_response_reorder;
	uint32_t http_pipe_scheduled;
	uint32_t http_pipe_runs;
	uint32_t http_pipe_prewritten;
	uint32_t http_pipe_fake_complete;
	uint32_t http_pipe_partial;
	uint32_t http_pipe_errors;
	uint32_t http_pipe_state_blocked;
	uint32_t http_pipe_untracked;
	uint32_t http_bypass_enqueued;
	uint32_t http_bypass_write_complete;
	uint32_t http_bypass_write_partial;
	uint32_t http_bypass_write_error;
	uint32_t http_bypass_released;
	uint32_t http_bypass_coalesced;
	uint32_t http_bypass_depth_peak;
	uint32_t http_bypass_drain_calls;
	uint32_t http_bypass_drain_bytes;
	uint32_t http_bypass_drain_would_block;
	uint32_t http_bypass_drain_error;
	uint32_t http_bypass_drain_blocked_vendor;
	uint32_t http_bypass_ready_plain;
	uint32_t http_bypass_ready_socket;
	uint32_t http_bypass_no_data;
	uint32_t http_bypass_ioctl_error;
	uint32_t http_bypass_worker_runs;
	uint32_t http_bypass_vendor_overlap;
	uint32_t iphone_rx_read_calls;
	uint32_t iphone_rx_read_bytes;
	uint32_t iphone_rx_post_missing;
	uint32_t iphone_rx_reader_gcd;
	uint32_t iphone_rx_reader_other;
	uint32_t iphone_rx_reader_prio_min;
	uint32_t iphone_rx_reader_prio_max;
	uint32_t car_contact;
	uint32_t car_release;
	uint32_t car_contact_start;
	uint32_t car_duplicate_release;
	uint32_t car_release_lt_50ms;
	uint32_t car_release_suppressed;
	uint32_t car_release_suppressed_lt50;
	uint32_t forward_contact;
	uint32_t forward_release;
	uint32_t forward_action_mismatch;
	uint32_t iphone_contact;
	uint32_t iphone_release;
	uint32_t iphone_action_mismatch;
	uint32_t release_http_enqueued;
	uint32_t release_http_write_complete;
	uint32_t release_http_response_2xx;
	uint32_t seq_car;
	uint32_t seq_forward;
	uint32_t seq_hid;
	uint32_t seq_http_enqueue;
	uint32_t seq_http_write;
	uint32_t seq_forward_missing;
	uint32_t seq_hid_missing;
	uint32_t seq_http_missing;
	uint32_t seq_write_reorder;
	uint32_t after_release_car_contact;
	uint32_t after_release_forward_contact;
	uint32_t after_release_hid_contact;
	uint32_t after_release_http_write;
	uint32_t quiet_hid;
	uint32_t quiet_http_write;
	uint32_t generic_in;
	uint32_t generic_forward;
	uint32_t generic_hid;
	uint32_t generic_missing_forward;
	uint32_t generic_missing_hid;
	uint32_t generic_payload_same;
	uint32_t generic_payload_changed;
	uint32_t generic_x_left;
	uint32_t generic_x_right;
	uint32_t generic_x_same;
	uint32_t generic_direction_flip;
	uint32_t descriptor_create_calls;
	uint32_t descriptor_add_calls;
	uint32_t descriptor_errors;
	uint32_t report5_contact;
	uint32_t report5_release;
	uint32_t report_len5;
	uint32_t report_len12;
	uint32_t report_len_other;
	uint32_t report5_down;
	uint32_t report5_up;
	uint32_t report5_duplicate_contact;
	uint32_t report5_duplicate_release;
	uint32_t report5_same_xy;
	uint32_t report5_moved;
	uint32_t report5_direction_flip;
	uint32_t report5_upper_flag_bits;
	uint32_t report_after_up_contact;
	uint32_t report_len_mismatch;
	uint32_t report_uid_mismatch;
	uint32_t report_uid_overflow;
	uint32_t move_sample_input;
	uint32_t move_sample_sent;
	uint32_t move_sample_suppressed;
	uint32_t move_sample_down;
	uint32_t move_sample_release;
	uint32_t move_sample_passthrough;
	uint32_t move_sample_gate_waits;
	uint32_t move_sample_edge_flushes;
	uint32_t report5_x_min;
	uint32_t report5_x_max;
	uint32_t report5_y_min;
	uint32_t report5_y_max;
	uint32_t report5_x_out_of_range;
	uint32_t report5_y_out_of_range;
	uint32_t after_release_first_x;
	uint32_t after_release_first_y;
	uint32_t after_release_last_x;
	uint32_t after_release_last_y;
	uint8_t after_release_coord_valid;
	touch_path_stage_t car_ack_total;
	touch_path_stage_t car_ack_write;
	touch_path_stage_t car_read_to_type;
	touch_path_stage_t car_rx_read_call;
	touch_path_stage_t car_rx_interval;
	touch_path_stage_t parse_to_touch;
	touch_path_stage_t car_touch_callback;
	touch_path_stage_t touch_to_forward;
	touch_path_stage_t forward_to_hid;
	touch_path_stage_t iphone_hid_enqueue;
	touch_path_stage_t iphone_hid_call_all;
	touch_path_stage_t bridge_end_to_end;
	touch_path_stage_t event_parser_total;
	touch_path_stage_t dispatch_wait;
	touch_path_stage_t dispatch_exec;
	touch_path_stage_t dispatch_total;
	touch_path_stage_t hid_to_http_enqueue;
	touch_path_stage_t http_enqueue_to_write;
	touch_path_stage_t http_write_total;
	touch_path_stage_t http_write_to_response;
	touch_path_stage_t http_response_to_next_write;
	touch_path_stage_t http_enqueue_to_response;
	touch_path_stage_t iphone_rx_write_to_post;
	touch_path_stage_t iphone_rx_post_to_read;
	touch_path_stage_t iphone_rx_read_call;
	touch_path_stage_t iphone_rx_post_to_complete;
	touch_path_stage_t iphone_rx_read_to_complete;
	touch_path_stage_t wire_end_to_end;
	touch_path_stage_t release_car_to_forward;
	touch_path_stage_t release_car_to_hid;
	touch_path_stage_t release_car_to_http_enqueue;
	touch_path_stage_t release_car_to_http_write;
	touch_path_stage_t release_car_to_http_response;
	touch_path_stage_t contact_to_release;
	touch_path_stage_t generic_interval;
	touch_path_stage_t read_complete_to_rearm;
	touch_path_stage_t parser_complete_to_rearm;
	touch_path_stage_t ack_complete_to_rearm;
	touch_path_stage_t event_to_hid_call;
	touch_path_stage_t event_to_hid_return;
	touch_path_stage_t event_to_http_enqueue;
	touch_path_stage_t event_to_http_write;
	touch_path_stage_t move_sample_write_interval;
	touch_path_tail_t event_to_hid_tail;
	touch_path_tail_t event_to_http_write_tail;
	touch_path_tail_t event_to_http_200_tail;
	touch_path_tail_t event_parser_tail;
	touch_path_tail_t iphone_hid_call_tail;
	touch_path_tail_t car_ack_write_tail;
	touch_path_tail_t dispatch_wait_tail;
	touch_path_tail_t dispatch_exec_tail;
	touch_path_tail_t dispatch_total_tail;
	touch_hid_byte0_stat_t iphone_hid_byte0[TOUCH_HID_BYTE0_SLOTS];
	touch_hid_uid_stat_t iphone_hid_uid[TOUCH_HID_UID_SLOTS];
} touch_path_stats_t;

typedef struct touch_http_slot_s {
	void *client;
	void *message;
	uint32_t source_start_us;
	uint32_t hid_start_us;
	uint32_t action_start_us;
	uint32_t enqueue_us;
	uint32_t write_start_us;
	uint32_t write_complete_us;
	uint32_t response_first_post_us;
	uint32_t response_first_read_us;
	uint32_t response_last_generation;
	uint32_t sequence;
	uint8_t active;
	uint8_t write_started;
	uint8_t write_completed;
	uint8_t is_touch;
	uint8_t is_release;
	uint8_t is_down;
	uint8_t response_post_valid;
	uint8_t pipeline_state;
} touch_http_slot_t;

typedef struct touch_read_origin_s {
	TaskHandle_t task;
	void *message;
	uint32_t complete_us;
	uint32_t read_call_us;
	uint32_t pending_bytes;
	int socket_fd;
	uint8_t valid;
	uint8_t pending_valid;
} touch_read_origin_t;

typedef struct touch_path_state_s {
	TaskHandle_t event_task;
	TaskHandle_t forward_task;
	uint32_t event_start_us;
	uint32_t touch_start_us;
	uint32_t forward_start_us;
	uint32_t last_car_touch_us;
	uint32_t last_forward_us;
	uint32_t last_action;
	uint32_t last_x;
	uint32_t last_y;
	uint32_t car_request_start_us;
	uint32_t car_ack_write_start_us;
	uint32_t hid_start_us;
	uint32_t hid_source_start_us;
	uint32_t hid_action_start_us;
	uint32_t dispatch_token_next;
	uint32_t dispatch_token_active;
	uint32_t dispatch_source_start_us;
	uint32_t dispatch_hid_start_us;
	uint32_t dispatch_action_start_us;
	uint32_t last_car_release_us;
	uint32_t last_hid_release_us;
	uint32_t last_car_contact_us;
	uint32_t sequence_next;
	uint32_t current_car_sequence;
	uint32_t current_forward_sequence;
	uint32_t hid_sequence;
	uint32_t dispatch_sequence;
	uint32_t last_car_sequence;
	uint32_t last_forward_sequence;
	uint32_t last_hid_sequence;
	uint32_t last_http_enqueue_sequence;
	uint32_t last_http_write_sequence;
	uint32_t release_sequence;
	uint32_t release_start_us;
	uint32_t last_hid_us;
	uint32_t last_http_write_us;
	uint32_t last_http_response_us;
	uint32_t last_http_response_sequence;
	void *last_http_response_client;
	uint32_t generic_sequence;
	uint32_t generic_forward_sequence;
	uint32_t last_generic_us;
	uint32_t last_car_hid_request_us;
	uint32_t advertised_touch_uid;
	uint32_t advertised_descriptor_length;
	uint32_t advertised_x_max;
	uint32_t advertised_y_max;
	uint32_t last_report_release_us;
	uint32_t last_car_read_complete_us;
	uint32_t last_event_parser_complete_us;
	uint32_t last_car_ack_complete_us;
	uint32_t car_rx_drain_streak;
	uint16_t generic_prev_x;
	uint16_t generic_prev_y;
	uint16_t report5_prev_x;
	uint16_t report5_prev_y;
	int8_t generic_prev_direction;
	int8_t report5_prev_direction;
	TaskHandle_t car_request_task;
	TaskHandle_t hid_task;
	TaskHandle_t dispatch_callback_task;
	uint8_t event_active;
	uint8_t event_saw_touch;
	uint8_t touch_active;
	uint8_t touch_forwarded;
	uint8_t forward_active;
	uint8_t forward_saw_hid;
	uint8_t car_request_active;
	uint8_t car_ack_write_started;
	uint8_t hid_active;
	uint8_t dispatch_callback_active;
	uint8_t car_contact_active;
	uint8_t forward_contact_active;
	uint8_t iphone_contact_active;
	uint8_t current_car_is_release;
	uint8_t current_car_release_lt50;
	uint8_t current_forward_is_release;
	uint8_t hid_is_release;
	uint8_t hid_is_down;
	uint8_t hid_is_touch;
	uint8_t dispatch_is_release;
	uint8_t dispatch_is_down;
	uint8_t dispatch_is_touch;
	uint8_t post_release_active;
	uint8_t generic_active;
	uint8_t generic_forward_active;
	uint8_t generic_forward_saw_hid;
	uint8_t generic_prev_valid;
	uint8_t advertised_touch_valid;
	uint8_t report5_prev_valid;
	uint8_t report5_touch_active;
	uint8_t move_sample_active;
	uint8_t move_sample_pending_valid;
	uint8_t car_socket_valid;
	uint8_t car_socket_nodelay_valid;
	uint8_t car_socket_nodelay;
	int car_socket_fd;
	TaskHandle_t generic_task;
	TaskHandle_t generic_forward_task;
	uint32_t move_sample_next_due_us;
	uint32_t move_sample_last_handover_us;
	uint32_t move_sample_pending_uid;
	uint8_t move_sample_pending_report[5];
	TaskHandle_t iphone_http_read_task;
	touch_http_slot_t *iphone_http_read_slot;
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	TaskHandle_t pipeline_write_task;
	void *pipeline_write_message;
	void *pipeline_scheduled_client;
#endif
#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
	void *bypass_client;
	void *bypass_head;
	void **bypass_tail;
	TaskHandle_t bypass_write_task;
	void *bypass_write_message;
	uint32_t bypass_depth;
	uint32_t bypass_last_touch_write_us;
	uint8_t bypass_touch_active;
	uint8_t bypass_worker_scheduled;
	uint8_t bypass_drain_scheduled;
#endif
} touch_path_state_t;

static touch_path_stats_t touch_path_stats
	__attribute__((section(".lpddr.bss.touch_path_stats")));
static touch_path_state_t touch_path_state
	__attribute__((section(".lpddr.bss.touch_path_state")));
static touch_http_slot_t touch_http_slots[TOUCH_HTTP_SLOTS]
	__attribute__((section(".lpddr.bss.touch_http_slots")));
static touch_read_origin_t touch_read_origins[TOUCH_READ_ORIGINS]
	__attribute__((section(".lpddr.bss.touch_read_origins")));
#if CONFIG_GMT_TIME_PROFILE
static time_t touch_gmt_previous_epoch;
static uint32_t touch_gmt_previous_tick_us;
static uint8_t touch_gmt_previous_valid;
#endif

extern int32_t __real_AirPlayResponse_GetInfoHIDReportCommand(
	const void *body, uint32_t length);
extern void __real_acc_carplay_cb_send_touch(
	uint32_t action, uint32_t x, uint32_t y);
extern void __real_acc_carplay_cb_hid_report(
	uint32_t uid, const void *report, uint32_t length);
extern void __real_lib_carplay_touch(uint32_t action, uint32_t x, uint32_t y);
extern int32_t __real_AirPlayReceiverSessionSendHIDReport(
	void *session, uint32_t device_uid, const void *report, uint32_t length);
extern int32_t __real_AirPlayResponse_GetInfoType(
	const void *body, uint32_t length, char *type);
extern int32_t __real_HTTPClientSendMessage(void *client, void *message);
extern int32_t __real_HTTPMessageWriteMessage(
	void *message, void *write_f, void *write_context);
extern int32_t __real_HTTPMessageReadMessage(
	void *message, void *read_f, void *read_context);
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
extern void *CFRetain(void *object);
extern void CFRelease(void *object);
extern void dispatch_async_f(void *queue, void *context,
			     void (*function)(void *));
extern void HTTPClientInvalidate(void *client);
#endif
#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
extern int32_t HTTPHeader_AddFieldF(void *header, const char *name,
				   const char *format, ...);
extern int32_t HTTPHeader_Commit(void *header);
extern uint64_t dispatch_time(uint64_t when, int64_t delta_nanoseconds);
extern void dispatch_after_f(uint64_t when, void *queue, void *context,
			     void (*function)(void *));
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH == 0
extern void *CFRetain(void *object);
extern void CFRelease(void *object);
extern void dispatch_async_f(void *queue, void *context,
			     void (*function)(void *));
extern void HTTPClientInvalidate(void *client);
#endif
#endif
#if CONFIG_IPHONE_HTTP_RX_PROFILE
extern ssize_t __real_lwip_read(int socket_fd, void *buffer, size_t length);
#endif
extern int32_t __real_HIDTouchScreenSingleCreateDescriptor(
	void **descriptor, uint32_t *descriptor_length,
	uint32_t width, uint32_t height);
extern int32_t __real_AirPlayInfoArrayAddHIDDevice(
	void *array, uint32_t uid, const char *name, uint32_t vendor_id,
	uint32_t product_id, uint32_t country_code,
	const void *descriptor, uint32_t descriptor_length, void *display_uuid);

static void touch_path_stage_add(touch_path_stage_t *stage, uint32_t elapsed_us)
{
	stage->sum_us += elapsed_us;
	stage->samples++;
	if (elapsed_us > stage->max_us) {
		stage->max_us = elapsed_us;
	}
}

/* HIDTouchScreenFillReport stores the normalized action in the descriptor's
 * Touch bit: zero is release/no-contact and non-zero is contact/move. */
static int touch_action_is_release(uint32_t action)
{
	return action == 0U;
}

static void touch_hid_uid_record(uint32_t uid, uint32_t length)
{
	uint32_t i;
	touch_hid_uid_stat_t *free_slot = NULL;
	uint8_t stored_len = (uint8_t)(length > 255U ? 255U : length);

	for (i = 0U; i < TOUCH_HID_UID_SLOTS; i++) {
		touch_hid_uid_stat_t *slot = &touch_path_stats.iphone_hid_uid[i];

		if (slot->valid && (slot->uid == uid)) {
			slot->count++;
			slot->bytes += length;
			if (stored_len < slot->min_len) slot->min_len = stored_len;
			if (stored_len > slot->max_len) slot->max_len = stored_len;
			return;
		}
		if (!slot->valid && (free_slot == NULL)) free_slot = slot;
	}
	if (free_slot != NULL) {
		free_slot->valid = 1U;
		free_slot->uid = uid;
		free_slot->count = 1U;
		free_slot->bytes = length;
		free_slot->min_len = stored_len;
		free_slot->max_len = stored_len;
	} else {
		touch_path_stats.report_uid_overflow++;
	}
}

static void touch_report5_record(uint32_t uid, const void *report,
				 uint32_t length, uint32_t now_us)
{
	const uint8_t *bytes = (const uint8_t *)report;
	uint32_t x;
	uint32_t y;
	int touch;

	touch_hid_uid_record(uid, length);
	if (length == 5U) touch_path_stats.report_len5++;
	else if (length == 12U) touch_path_stats.report_len12++;
	else touch_path_stats.report_len_other++;
	if (touch_path_state.advertised_touch_valid) {
		/* The 62-byte single-touch descriptor defines a 5-byte input report. */
		if ((touch_path_state.advertised_descriptor_length == 62U) &&
		    (length != 5U)) {
			touch_path_stats.report_len_mismatch++;
		}
		if (uid != touch_path_state.advertised_touch_uid) {
			touch_path_stats.report_uid_mismatch++;
		}
	}
	if ((bytes == NULL) || (length != 5U)) return;

	touch = (bytes[0] & 0x01U) != 0U;
	x = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8);
	y = (uint32_t)bytes[3] | ((uint32_t)bytes[4] << 8);
	if (touch_path_state.advertised_touch_valid) {
		if (x > touch_path_state.advertised_x_max) {
			touch_path_stats.report5_x_out_of_range++;
		}
		if (y > touch_path_state.advertised_y_max) {
			touch_path_stats.report5_y_out_of_range++;
		}
	}
	if ((bytes[0] & 0xfeU) != 0U) touch_path_stats.report5_upper_flag_bits++;
	if ((touch_path_stats.report5_contact +
	     touch_path_stats.report5_release) == 0U) {
		touch_path_stats.report5_x_min = x;
		touch_path_stats.report5_x_max = x;
		touch_path_stats.report5_y_min = y;
		touch_path_stats.report5_y_max = y;
	} else {
		if (x < touch_path_stats.report5_x_min) touch_path_stats.report5_x_min = x;
		if (x > touch_path_stats.report5_x_max) touch_path_stats.report5_x_max = x;
		if (y < touch_path_stats.report5_y_min) touch_path_stats.report5_y_min = y;
		if (y > touch_path_stats.report5_y_max) touch_path_stats.report5_y_max = y;
	}
	if (touch) {
		touch_path_stats.report5_contact++;
		if (!touch_path_state.report5_touch_active) {
			touch_path_stats.report5_down++;
			if (touch_path_state.last_report_release_us != 0U) {
				touch_path_stats.report_after_up_contact++;
			}
		} else {
			touch_path_stats.report5_duplicate_contact++;
		}
	} else {
		touch_path_stats.report5_release++;
		if (touch_path_state.report5_touch_active) {
			touch_path_stats.report5_up++;
		} else {
			touch_path_stats.report5_duplicate_release++;
		}
		touch_path_state.last_report_release_us = now_us;
	}
	if (touch_path_state.report5_prev_valid) {
		int32_t dx = (int32_t)x - (int32_t)touch_path_state.report5_prev_x;
		int32_t dy = (int32_t)y - (int32_t)touch_path_state.report5_prev_y;
		int8_t direction = 0;

		if ((dx == 0) && (dy == 0)) {
			touch_path_stats.report5_same_xy++;
		} else {
			touch_path_stats.report5_moved++;
			if (dx > 0) direction = 1;
			else if (dx < 0) direction = -1;
			if (direction == 0) {
				if (dy > 0) direction = 1;
				else if (dy < 0) direction = -1;
			}
			if ((direction != 0) &&
			    (touch_path_state.report5_prev_direction != 0) &&
			    (direction != touch_path_state.report5_prev_direction)) {
				touch_path_stats.report5_direction_flip++;
			}
			if (direction != 0) touch_path_state.report5_prev_direction = direction;
		}
	}
	touch_path_state.report5_prev_x = (uint16_t)x;
	touch_path_state.report5_prev_y = (uint16_t)y;
	touch_path_state.report5_prev_valid = 1U;
	touch_path_state.report5_touch_active = touch != 0;
}

static void touch_path_tail_add(touch_path_tail_t *tail, uint32_t elapsed_us)
{
	if (elapsed_us > 1000U) {
		tail->gt_1ms++;
	}
	if (elapsed_us > 3000U) {
		tail->gt_3ms++;
	}
	if (elapsed_us > 5000U) {
		tail->gt_5ms++;
	}
	if (elapsed_us > 10000U) {
		tail->gt_10ms++;
	}
	if (elapsed_us > 16000U) {
		tail->gt_16ms++;
	}
}

/* Record only format metadata, never HID payload contents.  byte0 is useful
 * for separating report IDs on descriptors that use them, while the length
 * bins still identify the format when byte0 is ordinary report data. */
static void touch_hid_format_record(const void *report, uint32_t length)
{
	const uint8_t *bytes = (const uint8_t *)report;
	uint32_t i;
	touch_hid_byte0_stat_t *free_slot = NULL;

	touch_path_stats.iphone_hid_bytes += length;
	if (length <= 4U) {
		touch_path_stats.iphone_hid_len_le4++;
	} else if (length == 5U) {
		touch_path_stats.iphone_hid_len_eq5++;
	} else if (length <= 8U) {
		touch_path_stats.iphone_hid_len_6_8++;
	} else if (length <= 16U) {
		touch_path_stats.iphone_hid_len_9_16++;
	} else if (length <= 32U) {
		touch_path_stats.iphone_hid_len_17_32++;
	} else if (length <= 64U) {
		touch_path_stats.iphone_hid_len_33_64++;
	} else if (length <= 128U) {
		touch_path_stats.iphone_hid_len_65_128++;
	} else {
		touch_path_stats.iphone_hid_len_gt128++;
	}
	if ((report == NULL) || (length == 0U)) return;
	for (i = 0U; i < TOUCH_HID_BYTE0_SLOTS; i++) {
		touch_hid_byte0_stat_t *slot =
			&touch_path_stats.iphone_hid_byte0[i];

		if (slot->valid && (slot->byte0 == bytes[0])) {
			uint8_t stored_len =
				(uint8_t)(length > 255U ? 255U : length);

			slot->count++;
			slot->bytes += length;
			if (stored_len < slot->min_len) slot->min_len = stored_len;
			if (stored_len > slot->max_len) slot->max_len = stored_len;
			return;
		}
		if (!slot->valid && (free_slot == NULL)) free_slot = slot;
	}
	if (free_slot != NULL) {
		free_slot->valid = 1U;
		free_slot->byte0 = bytes[0];
		free_slot->count = 1U;
		free_slot->bytes = length;
		free_slot->min_len = (uint8_t)(length > 255U ? 255U : length);
		free_slot->max_len = free_slot->min_len;
	} else {
		touch_path_stats.iphone_hid_byte0_overflow++;
	}
}

static int touch_path_is_hid_send_report(const char *type)
{
	static const char expected[] = "hidSendReport";
	uint32_t i;

	if (type == NULL) {
		return 0;
	}
	for (i = 0U; i < sizeof(expected); i++) {
		if (type[i] != expected[i]) {
			return 0;
		}
	}
	return 1;
}

static touch_http_slot_t *touch_http_find(void *message)
{
	uint32_t i;

	for (i = 0U; i < TOUCH_HTTP_SLOTS; i++) {
		if (touch_http_slots[i].active &&
		    (touch_http_slots[i].message == message)) {
			return &touch_http_slots[i];
		}
	}
	return NULL;
}

static touch_http_slot_t *touch_http_allocate(void *client, void *message,
					       uint32_t source_start_us,
					       uint32_t hid_start_us,
					       uint32_t action_start_us,
					       uint32_t enqueue_us,
					       int is_touch, int is_release, int is_down,
					       uint32_t sequence)
{
	uint32_t i;
	uint32_t depth = 0U;
	touch_http_slot_t *free_slot = NULL;

	for (i = 0U; i < TOUCH_HTTP_SLOTS; i++) {
		if (touch_http_slots[i].active &&
		    ((enqueue_us - touch_http_slots[i].enqueue_us) >
		     TOUCH_HTTP_STALE_US)) {
			memset(&touch_http_slots[i], 0, sizeof(touch_http_slots[i]));
			touch_path_stats.http_stale_expired++;
		}
		if (touch_http_slots[i].active) {
			depth++;
		} else if (free_slot == NULL) {
			free_slot = &touch_http_slots[i];
		}
	}
	if (free_slot == NULL) {
		return NULL;
	}
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->client = client;
	free_slot->message = message;
	free_slot->source_start_us = source_start_us;
	free_slot->hid_start_us = hid_start_us;
	free_slot->action_start_us = action_start_us;
	free_slot->enqueue_us = enqueue_us;
	free_slot->is_touch = is_touch != 0;
	free_slot->is_release = is_release != 0;
	free_slot->is_down = is_down != 0;
	free_slot->sequence = sequence;
	free_slot->active = 1U;
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	free_slot->pipeline_state = TOUCH_HTTP_PIPE_QUEUED;
#endif
	depth++;
	if (depth > touch_path_stats.http_depth_peak) {
		touch_path_stats.http_depth_peak = depth;
	}
	return free_slot;
}

static uint32_t touch_http_live_depth(void)
{
	uint32_t i;
	uint32_t depth = 0U;

	for (i = 0U; i < TOUCH_HTTP_SLOTS; i++) {
		depth += touch_http_slots[i].active != 0U;
	}
	return depth;
}

#if (CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0) || \
	CONFIG_AIRPLAY_HID_HTTP_BYPASS
int32_t __wrap_HTTPMessageWriteMessage(
	void *message, void *write_f, void *write_context);

static void *touch_http_pointer_at(void *base, uint32_t offset)
{
	return *(void **)((uint8_t *)base + offset);
}
#endif

#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
static void touch_http_pipeline_pump(void *context);

static void touch_http_pipeline_schedule(void *client)
{
	void *queue;
	int schedule = 0;

	if (client == NULL) {
		return;
	}
	queue = touch_http_pointer_at(client, TOUCH_HTTP_CLIENT_QUEUE_OFFSET);
	if (queue == NULL) {
		return;
	}

	taskENTER_CRITICAL();
	if (touch_path_state.pipeline_scheduled_client == NULL) {
		touch_path_state.pipeline_scheduled_client = client;
		touch_path_stats.http_pipe_scheduled++;
		schedule = 1;
	}
	taskEXIT_CRITICAL();
	if (schedule) {
		(void)CFRetain(client);
		dispatch_async_f(queue, client, touch_http_pipeline_pump);
	}
}

static void touch_http_pipeline_pump(void *context)
{
	void *client = context;
	void *message;
	void *write_f;
	void *write_context;
	uint32_t wire_depth = 1U;
	int invalidate = 0;

	taskENTER_CRITICAL();
	if (touch_path_state.pipeline_scheduled_client == client) {
		touch_path_state.pipeline_scheduled_client = NULL;
	}
	touch_path_stats.http_pipe_runs++;
	taskEXIT_CRITICAL();

	/* State 4 means the normal HTTPClient state machine has completed the
	 * head write and is waiting for that response.  Only then may a later
	 * request be put on the same ordered transport stream. */
	if (*((uint8_t *)client + TOUCH_HTTP_CLIENT_STATE_OFFSET) !=
	    TOUCH_HTTP_CLIENT_READING_RESPONSE) {
		taskENTER_CRITICAL();
		touch_path_stats.http_pipe_state_blocked++;
		taskEXIT_CRITICAL();
		goto exit;
	}

	message = touch_http_pointer_at(client, TOUCH_HTTP_CLIENT_HEAD_OFFSET);
	if (message == NULL) {
		goto exit;
	}
	taskENTER_CRITICAL();
	{
		touch_http_slot_t *head_slot = touch_http_find(message);
		if ((head_slot == NULL) || !head_slot->write_completed) {
			touch_path_stats.http_pipe_untracked++;
			message = NULL;
		}
	}
	taskEXIT_CRITICAL();
	if (message == NULL) {
		goto exit;
	}

	write_context = touch_http_pointer_at(
		client, TOUCH_HTTP_CLIENT_WRITE_CTX_OFFSET);
	write_f = touch_http_pointer_at(client, TOUCH_HTTP_CLIENT_WRITE_F_OFFSET);
	message = touch_http_pointer_at(message, TOUCH_HTTP_MESSAGE_NEXT_OFFSET);

	while ((message != NULL) &&
	       (wire_depth < CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH)) {
		touch_http_slot_t *slot;
		uint8_t pipeline_state;
		int32_t result;

		taskENTER_CRITICAL();
		slot = touch_http_find(message);
		if ((slot == NULL) || (slot->client != client)) {
			touch_path_stats.http_pipe_untracked++;
			taskEXIT_CRITICAL();
			break;
		}
		pipeline_state = slot->pipeline_state;
		if (pipeline_state == TOUCH_HTTP_PIPE_PREWRITTEN) {
			wire_depth++;
			taskEXIT_CRITICAL();
			message = touch_http_pointer_at(
				message, TOUCH_HTTP_MESSAGE_NEXT_OFFSET);
			continue;
		}
		if (pipeline_state != TOUCH_HTTP_PIPE_QUEUED) {
			taskEXIT_CRITICAL();
			break;
		}
		touch_path_state.pipeline_write_task =
			xTaskGetCurrentTaskHandle();
		touch_path_state.pipeline_write_message = message;
		taskEXIT_CRITICAL();

		result = __wrap_HTTPMessageWriteMessage(
			message, write_f, write_context);

		taskENTER_CRITICAL();
		touch_path_state.pipeline_write_task = NULL;
		touch_path_state.pipeline_write_message = NULL;
		slot = touch_http_find(message);
		if (slot != NULL) {
			if (result == 0) {
				slot->pipeline_state =
					TOUCH_HTTP_PIPE_PREWRITTEN;
				touch_path_stats.http_pipe_prewritten++;
				wire_depth++;
			} else if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
				slot->pipeline_state = TOUCH_HTTP_PIPE_PARTIAL;
				touch_path_stats.http_pipe_partial++;
			} else {
				slot->pipeline_state = TOUCH_HTTP_PIPE_NONE;
				touch_path_stats.http_pipe_errors++;
				invalidate = 1;
			}
		}
		taskEXIT_CRITICAL();
		if (result != 0) {
			break;
		}
		message = touch_http_pointer_at(
			message, TOUCH_HTTP_MESSAGE_NEXT_OFFSET);
	}

	if (invalidate) {
		/* A failed encrypted write cannot safely be replayed because the TX
		 * stream state may already have advanced. */
		HTTPClientInvalidate(client);
	}
exit:
	CFRelease(client);
}
#endif

#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
typedef int32_t (*touch_http_transport_read_f)(
	void *buffer, uint32_t length, uint32_t *out_length, void *context);

static void touch_http_bypass_worker(void *context);
static void touch_http_bypass_drain_worker(void *context);

static int32_t touch_http_bypass_prepare(void *message)
{
	uint8_t *bytes = (uint8_t *)message;
	uint32_t header_length;
	uint32_t body_length;
	int32_t result;

	header_length = *(uint32_t *)(bytes +
		TOUCH_HTTP_MESSAGE_HEADER_LEN);
	if (header_length != 0U) {
		if (bytes[TOUCH_HTTP_MESSAGE_CLOSE_OFFSET] != 0U) {
			result = HTTPHeader_AddFieldF(bytes + 0x10U,
				"Connection", "close");
			if (result != 0) {
				return result;
			}
		}
		result = HTTPHeader_Commit(bytes + 0x10U);
		if (result != 0) {
			return result;
		}
		header_length = *(uint32_t *)(bytes +
			TOUCH_HTTP_MESSAGE_HEADER_LEN);
	}

	*(void **)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOV) = bytes + 0x10U;
	*(uint32_t *)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOV + 4U) =
		header_length;
	*(uint32_t *)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOVCNT) =
		header_length != 0U ? 1U : 0U;
	body_length = *(uint32_t *)(bytes +
		TOUCH_HTTP_MESSAGE_BODY_LEN_OFFSET);
	if (body_length != 0U) {
		*(void **)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOV + 8U) =
			touch_http_pointer_at(message, 0x49cU);
		*(uint32_t *)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOV + 12U) =
			body_length;
		*(uint32_t *)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOVCNT) = 2U;
	}
	*(void **)(bytes + TOUCH_HTTP_MESSAGE_WRITE_IOV + 16U) =
		bytes + TOUCH_HTTP_MESSAGE_WRITE_IOV;
	*(void **)(bytes + TOUCH_HTTP_MESSAGE_NEXT_OFFSET) = NULL;
	return 0;
}

static int32_t touch_http_bypass_drain(void *client)
{
	uint8_t buffer[TOUCH_HTTP_BYPASS_DRAIN_BYTES];
	touch_http_transport_read_f read_f;
	void *read_context;
	uint32_t pass;

	/* Never steal bytes while the vendor state machine owns a request on
	 * this client.  HID bypass normally leaves the vendor queue empty. */
	if (touch_http_pointer_at(client, TOUCH_HTTP_CLIENT_HEAD_OFFSET) != NULL) {
		taskENTER_CRITICAL();
		touch_path_stats.http_bypass_drain_blocked_vendor++;
		taskEXIT_CRITICAL();
		return TOUCH_OSSTATUS_WOULD_BLOCK;
	}
	read_f = (touch_http_transport_read_f)touch_http_pointer_at(
		client, 0x47cU);
	read_context = touch_http_pointer_at(
		client, TOUCH_HTTP_CLIENT_WRITE_CTX_OFFSET);
	if ((read_f == NULL) || (read_context == NULL)) {
		return -1;
	}

	for (pass = 0U; pass < 32U; pass++) {
		uint8_t *plain_cur;
		uint8_t *plain_end;
		uint32_t pending_bytes = 0U;
		int socket_fd;
		int ready_plain;
		int ready_socket = 0;
		uint32_t out_length = 0U;
		int32_t result;

		/* _NetTransportRead can wait for a complete encrypted record.  Do
		 * not enter it merely to discover that no input exists: first check
		 * both its already-decrypted plaintext and the underlying socket.
		 * Repeating this check for every pass is important; after consuming
		 * one response, the next read must not wait for a future response. */
		plain_cur = *(uint8_t **)((uint8_t *)read_context +
			TOUCH_NETTRANSPORT_PLAIN_CUR);
		plain_end = *(uint8_t **)((uint8_t *)read_context +
			TOUCH_NETTRANSPORT_PLAIN_END);
		ready_plain = (plain_cur != NULL) && (plain_end != NULL) &&
			(plain_end > plain_cur);
		socket_fd = *(int *)read_context;
		if (!ready_plain && (socket_fd >= 0)) {
			if (lwip_ioctl(socket_fd, FIONREAD, &pending_bytes) == 0) {
				ready_socket = pending_bytes != 0U;
			} else {
				taskENTER_CRITICAL();
				touch_path_stats.http_bypass_ioctl_error++;
				taskEXIT_CRITICAL();
			}
		}
		if (!ready_plain && !ready_socket) {
			taskENTER_CRITICAL();
			touch_path_stats.http_bypass_no_data++;
			taskEXIT_CRITICAL();
			return TOUCH_OSSTATUS_WOULD_BLOCK;
		}
		taskENTER_CRITICAL();
		if (ready_plain) {
			touch_path_stats.http_bypass_ready_plain++;
		} else {
			touch_path_stats.http_bypass_ready_socket++;
		}
		taskEXIT_CRITICAL();

		result = read_f(buffer, sizeof(buffer),
			&out_length, read_context);

		taskENTER_CRITICAL();
		touch_path_stats.http_bypass_drain_calls++;
		touch_path_stats.http_bypass_drain_bytes += out_length;
		if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
			touch_path_stats.http_bypass_drain_would_block++;
		} else if (result != 0) {
			touch_path_stats.http_bypass_drain_error++;
		}
		taskEXIT_CRITICAL();
		if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
			return result;
		}
		if (result != 0) {
			return result;
		}
		if (out_length == 0U) {
			return TOUCH_OSSTATUS_WOULD_BLOCK;
		}
		/* read_f has already authenticated/decrypted this transport record.
		 * This dedicated HID client only receives empty request responses, so
		 * the plaintext can be discarded without HTTP/RTSP parsing. */
	}
	return 0;
}

static void touch_http_bypass_schedule_worker(void *client,
					      uint32_t delay_us, int force)
{
	void *queue;
	int schedule = 0;

	queue = touch_http_pointer_at(client, TOUCH_HTTP_CLIENT_QUEUE_OFFSET);
	if (queue == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	if (force || !touch_path_state.bypass_worker_scheduled) {
		touch_path_state.bypass_worker_scheduled = 1U;
		schedule = 1;
	}
	taskEXIT_CRITICAL();
	if (schedule) {
		(void)CFRetain(client);
		if (delay_us != 0U) {
			dispatch_after_f(dispatch_time(0U,
				(int64_t)delay_us * 1000LL), queue, client,
				touch_http_bypass_worker);
		} else {
			dispatch_async_f(queue, client,
				touch_http_bypass_worker);
		}
	}
}

static void touch_http_bypass_schedule_drain(void *client)
{
	void *queue;
	int schedule = 0;

	queue = touch_http_pointer_at(client, TOUCH_HTTP_CLIENT_QUEUE_OFFSET);
	if (queue == NULL) {
		return;
	}
	taskENTER_CRITICAL();
	if (!touch_path_state.bypass_drain_scheduled) {
		touch_path_state.bypass_drain_scheduled = 1U;
		schedule = 1;
	}
	taskEXIT_CRITICAL();
	if (schedule) {
		(void)CFRetain(client);
		dispatch_after_f(dispatch_time(0U,
			TOUCH_HTTP_BYPASS_FINAL_DRAIN_NS), queue, client,
			touch_http_bypass_drain_worker);
	}
}

static int32_t touch_http_bypass_enqueue(void *client, void *message)
{
	int32_t result = touch_http_bypass_prepare(message);
	int force = 0;

	if (result != 0) {
		return result;
	}
	(void)CFRetain(message);
	(void)CFRetain(client);
	taskENTER_CRITICAL();
	if ((touch_path_state.bypass_client != NULL) &&
	    (touch_path_state.bypass_client != client) &&
	    (touch_path_state.bypass_depth != 0U)) {
		taskEXIT_CRITICAL();
		CFRelease(client);
		CFRelease(message);
		return -1;
	}
	if (touch_path_state.bypass_client != client) {
		touch_path_state.bypass_client = client;
		touch_path_state.bypass_head = NULL;
		touch_path_state.bypass_tail =
			&touch_path_state.bypass_head;
		touch_path_state.bypass_worker_scheduled = 0U;
		touch_path_state.bypass_drain_scheduled = 0U;
		touch_path_state.bypass_touch_active = 0U;
		touch_path_state.bypass_last_touch_write_us = 0U;
	}
	*touch_path_state.bypass_tail = message;
	touch_path_state.bypass_tail = (void **)((uint8_t *)message +
		TOUCH_HTTP_MESSAGE_NEXT_OFFSET);
	touch_path_state.bypass_depth++;
	{
		touch_http_slot_t *slot = touch_http_find(message);

		force = (slot != NULL) && slot->is_touch &&
			(slot->is_down || slot->is_release);
	}
	touch_path_stats.http_bypass_enqueued++;
	if (touch_path_state.bypass_depth >
	    touch_path_stats.http_bypass_depth_peak) {
		touch_path_stats.http_bypass_depth_peak =
			touch_path_state.bypass_depth;
	}
	taskEXIT_CRITICAL();
	touch_http_bypass_schedule_worker(client, 0U, force);
	return 0;
}

static void touch_http_bypass_worker(void *context)
{
	void *client = context;
	void *write_f = touch_http_pointer_at(
		client, TOUCH_HTTP_CLIENT_WRITE_F_OFFSET);
	void *write_context = touch_http_pointer_at(
		client, TOUCH_HTTP_CLIENT_WRITE_CTX_OFFSET);
	int fatal = 0;

	taskENTER_CRITICAL();
	if (touch_path_state.bypass_client != client) {
		taskEXIT_CRITICAL();
		CFRelease(client);
		return;
	}
	touch_path_state.bypass_worker_scheduled = 0U;
	touch_path_stats.http_bypass_worker_runs++;
	taskEXIT_CRITICAL();
	(void)touch_http_bypass_drain(client);

	for (;;) {
		void *message;
		void *next;
		int32_t result;

		taskENTER_CRITICAL();
		message = touch_path_state.bypass_head;
		if (message != NULL) {
			touch_path_state.bypass_write_task =
				xTaskGetCurrentTaskHandle();
			touch_path_state.bypass_write_message = message;
		}
		taskEXIT_CRITICAL();
		if (message == NULL) {
			break;
		}

		result = __wrap_HTTPMessageWriteMessage(
			message, write_f, write_context);
		taskENTER_CRITICAL();
		touch_path_state.bypass_write_task = NULL;
		touch_path_state.bypass_write_message = NULL;
		if (result == 0) {
			uint32_t write_complete_us = hal_read_curtime_us();
			touch_http_slot_t *completed_slot = touch_http_find(message);

			next = touch_http_pointer_at(
				message, TOUCH_HTTP_MESSAGE_NEXT_OFFSET);
			touch_path_state.bypass_head = next;
			if (next == NULL) {
				touch_path_state.bypass_tail =
					&touch_path_state.bypass_head;
			}
			if (touch_path_state.bypass_depth != 0U) {
				touch_path_state.bypass_depth--;
			}
			touch_path_stats.http_bypass_write_complete++;
			touch_path_stats.http_bypass_released++;
			if ((completed_slot != NULL) && completed_slot->is_touch) {
#if CONFIG_TOUCH_MOVE_SAMPLE_HZ > 0
				touch_path_stats.move_sample_sent++;
				if (completed_slot->is_release) {
					touch_path_state.bypass_touch_active = 0U;
					touch_path_state.bypass_last_touch_write_us = 0U;
				} else {
					if (!completed_slot->is_down &&
					    (touch_path_state.bypass_last_touch_write_us != 0U)) {
						touch_path_stage_add(
							&touch_path_stats.move_sample_write_interval,
							write_complete_us -
							touch_path_state.bypass_last_touch_write_us);
					}
					touch_path_state.bypass_touch_active = 1U;
					touch_path_state.bypass_last_touch_write_us =
						write_complete_us;
				}
#endif
			}
			if (completed_slot != NULL) {
				memset(completed_slot, 0, sizeof(*completed_slot));
			}
		} else if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
			touch_path_stats.http_bypass_write_partial++;
			next = NULL;
		} else {
			touch_path_stats.http_bypass_write_error++;
			next = NULL;
			fatal = 1;
		}
		taskEXIT_CRITICAL();

		if (result == 0) {
			CFRelease(message);
			CFRelease(client);
			(void)touch_http_bypass_drain(client);
			continue;
		}
		if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
			touch_http_bypass_schedule_worker(client, 1000U, 0);
		}
		break;
	}

	if (fatal) {
		void *discard;

		taskENTER_CRITICAL();
		discard = touch_path_state.bypass_head;
		touch_path_state.bypass_head = NULL;
		touch_path_state.bypass_tail =
			&touch_path_state.bypass_head;
		touch_path_state.bypass_depth = 0U;
		touch_path_state.bypass_client = NULL;
		touch_path_state.bypass_touch_active = 0U;
		touch_path_state.move_sample_active = 0U;
		touch_path_state.move_sample_pending_valid = 0U;
		touch_path_state.bypass_last_touch_write_us = 0U;
		touch_path_state.move_sample_last_handover_us = 0U;
		touch_path_state.move_sample_next_due_us = 0U;
		taskEXIT_CRITICAL();
		while (discard != NULL) {
			void *next = touch_http_pointer_at(
				discard, TOUCH_HTTP_MESSAGE_NEXT_OFFSET);
			taskENTER_CRITICAL();
			{
				touch_http_slot_t *slot = touch_http_find(discard);
				if (slot != NULL) {
					memset(slot, 0, sizeof(*slot));
				}
			}
			taskEXIT_CRITICAL();
			CFRelease(discard);
			CFRelease(client);
			discard = next;
		}
		HTTPClientInvalidate(client);
	} else {
		touch_http_bypass_schedule_drain(client);
	}
	CFRelease(client);
}

static void touch_http_bypass_drain_worker(void *context)
{
	void *client = context;
	int32_t result;

	taskENTER_CRITICAL();
	if (touch_path_state.bypass_client != client) {
		taskEXIT_CRITICAL();
		CFRelease(client);
		return;
	}
	touch_path_state.bypass_drain_scheduled = 0U;
	taskEXIT_CRITICAL();
	result = touch_http_bypass_drain(client);
	if ((result != 0) && (result != TOUCH_OSSTATUS_WOULD_BLOCK)) {
		HTTPClientInvalidate(client);
	}
	CFRelease(client);
}
#endif

static void touch_read_origin_record(TaskHandle_t task, void *message,
				     uint32_t complete_us,
				     uint32_t read_call_us, int socket_fd,
				     uint32_t pending_bytes, int pending_valid)
{
	uint32_t i;
	touch_read_origin_t *oldest = &touch_read_origins[0];

	for (i = 0U; i < TOUCH_READ_ORIGINS; i++) {
		if (!touch_read_origins[i].valid ||
		    (touch_read_origins[i].task == task)) {
			oldest = &touch_read_origins[i];
			break;
		}
		if ((complete_us - touch_read_origins[i].complete_us) >
		    (complete_us - oldest->complete_us)) {
			oldest = &touch_read_origins[i];
		}
	}
	oldest->task = task;
	oldest->message = message;
	oldest->complete_us = complete_us;
	oldest->read_call_us = read_call_us;
	oldest->socket_fd = socket_fd;
	oldest->pending_bytes = pending_bytes;
	oldest->pending_valid = pending_valid != 0;
	oldest->valid = 1U;
}

static uint32_t touch_read_origin_take(TaskHandle_t task, uint32_t now_us,
				       int *matched, uint32_t *read_call_us,
				       int *socket_fd, uint32_t *pending_bytes,
				       int *pending_valid)
{
	uint32_t i;
	uint32_t complete_us = now_us;

	*matched = 0;
	*read_call_us = 0U;
	*socket_fd = -1;
	*pending_bytes = 0U;
	*pending_valid = 0;
	for (i = 0U; i < TOUCH_READ_ORIGINS; i++) {
		if (touch_read_origins[i].valid &&
		    (touch_read_origins[i].task == task)) {
			if ((now_us - touch_read_origins[i].complete_us) <=
			    TOUCH_READ_PAIR_MAX_AGE_US) {
					complete_us = touch_read_origins[i].complete_us;
					*matched = 1;
					*read_call_us = touch_read_origins[i].read_call_us;
					*socket_fd = touch_read_origins[i].socket_fd;
					*pending_bytes = touch_read_origins[i].pending_bytes;
					*pending_valid =
						touch_read_origins[i].pending_valid != 0U;
			}
			touch_read_origins[i].valid = 0U;
			break;
		}
	}
	return complete_us;
}

int32_t __wrap_HIDTouchScreenSingleCreateDescriptor(
	void **descriptor, uint32_t *descriptor_length,
	uint32_t width, uint32_t height)
{
	int32_t result = __real_HIDTouchScreenSingleCreateDescriptor(
		descriptor, descriptor_length, width, height);

	taskENTER_CRITICAL();
	touch_path_stats.descriptor_create_calls++;
	if (result != 0) {
		touch_path_stats.descriptor_errors++;
	} else {
		touch_path_state.advertised_x_max = width;
		touch_path_state.advertised_y_max = height;
		if (descriptor_length != NULL) {
			touch_path_state.advertised_descriptor_length =
				*descriptor_length;
		}
	}
	taskEXIT_CRITICAL();
	return result;
}

int32_t __wrap_AirPlayInfoArrayAddHIDDevice(
	void *array, uint32_t uid, const char *name, uint32_t vendor_id,
	uint32_t product_id, uint32_t country_code,
	const void *descriptor, uint32_t descriptor_length, void *display_uuid)
{
	int32_t result = __real_AirPlayInfoArrayAddHIDDevice(
		array, uid, name, vendor_id, product_id, country_code,
		descriptor, descriptor_length, display_uuid);

	/* The locally generated single-touch descriptor is uniquely 62 bytes. */
	if (descriptor_length == 62U) {
		taskENTER_CRITICAL();
		touch_path_stats.descriptor_add_calls++;
		if (result != 0) {
			touch_path_stats.descriptor_errors++;
		} else {
			const uint8_t *bytes = (const uint8_t *)descriptor;

			touch_path_state.advertised_touch_uid = uid;
			touch_path_state.advertised_descriptor_length =
				descriptor_length;
			touch_path_state.advertised_touch_valid = 1U;
			if (bytes != NULL) {
				touch_path_state.advertised_x_max =
					(uint32_t)bytes[39] |
					((uint32_t)bytes[40] << 8);
				touch_path_state.advertised_y_max =
					(uint32_t)bytes[52] |
					((uint32_t)bytes[53] << 8);
			}
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

/* GetInfoType is the earliest externally visible point after a complete car request. */
int32_t __wrap_AirPlayResponse_GetInfoType(
	const void *body, uint32_t length, char *type)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t call_start_us = hal_read_curtime_us();
	int32_t result = __real_AirPlayResponse_GetInfoType(body, length, type);

	if (touch_path_is_hid_send_report(type)) {
		uint32_t now_us = hal_read_curtime_us();
		uint32_t request_start_us;
		uint32_t read_call_us;
		uint32_t pending_bytes;
		uint32_t interval_us = 0U;
		int socket_fd;
		int pending_valid;
		int read_matched;

		taskENTER_CRITICAL();
		request_start_us = touch_read_origin_take(current, call_start_us,
			&read_matched, &read_call_us, &socket_fd, &pending_bytes,
			&pending_valid);
		touch_path_stats.car_type_matches++;
		if (read_matched) {
			touch_path_stats.car_read_pairs++;
			touch_path_stage_add(&touch_path_stats.car_read_to_type,
				now_us - request_start_us);
			touch_path_stage_add(&touch_path_stats.car_rx_read_call,
				read_call_us);
			if (pending_valid) {
				touch_path_stats.car_rx_pending_samples++;
				touch_path_stats.car_rx_pending_nonzero +=
					pending_bytes != 0U;
				touch_path_stats.car_rx_pending_sum += pending_bytes;
				if (pending_bytes > touch_path_stats.car_rx_pending_max) {
					touch_path_stats.car_rx_pending_max = pending_bytes;
				}
			} else {
				touch_path_stats.car_rx_pending_ioctl_errors++;
			}
			if (touch_path_state.last_car_hid_request_us != 0U) {
				interval_us = request_start_us -
					touch_path_state.last_car_hid_request_us;
				touch_path_stage_add(&touch_path_stats.car_rx_interval,
					interval_us);
				if (interval_us <= 1000U) {
					touch_path_stats.car_rx_gap_le_1ms++;
				} else if (interval_us <= TOUCH_RX_DRAIN_GAP_US) {
					touch_path_stats.car_rx_gap_1_5ms++;
				} else if (interval_us <= 15000U) {
					touch_path_stats.car_rx_gap_5_15ms++;
				} else if (interval_us <= 30000U) {
					touch_path_stats.car_rx_gap_15_30ms++;
				} else {
					touch_path_stats.car_rx_gap_gt_30ms++;
				}
				if (interval_us <= TOUCH_RX_DRAIN_GAP_US) {
					touch_path_state.car_rx_drain_streak++;
					if (touch_path_state.car_rx_drain_streak >
					    touch_path_stats.car_rx_drain_streak_max) {
						touch_path_stats.car_rx_drain_streak_max =
							touch_path_state.car_rx_drain_streak;
					}
				} else {
					touch_path_state.car_rx_drain_streak = 0U;
				}
			}
			touch_path_state.last_car_hid_request_us = request_start_us;
			if (socket_fd >= 0) {
				if (!touch_path_state.car_socket_valid ||
				    (touch_path_state.car_socket_fd != socket_fd)) {
					touch_path_state.car_socket_nodelay_valid = 0U;
				}
				touch_path_state.car_socket_fd = socket_fd;
				touch_path_state.car_socket_valid = 1U;
			}
		} else {
			touch_path_stats.car_read_misses++;
		}
		if (touch_path_state.car_request_active) {
			touch_path_stats.car_type_overwrites++;
		}
		touch_path_state.car_request_task = current;
		touch_path_state.car_request_start_us = request_start_us;
		touch_path_state.car_ack_write_start_us = 0U;
		touch_path_state.car_request_active = 1U;
		touch_path_state.car_ack_write_started = 0U;
		taskEXIT_CRITICAL();
	}
	return result;
}

/*
 * This entry point runs after TCPClient has received the complete vehicle
 * event and sent its response.  GetInfoType and HTTPMessageRead/Write wrappers
 * preserve the preceding read-complete and response-write boundaries so this
 * parser can carry the original vehicle event timestamp through the bridge.
 */
int32_t __wrap_AirPlayResponse_GetInfoHIDReportCommand(
	const void *body, uint32_t length)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	uint32_t event_start_us = start_us;
	int32_t result;

	taskENTER_CRITICAL();
	if (touch_path_state.car_request_active &&
	    (touch_path_state.car_request_task == current)) {
		event_start_us = touch_path_state.car_request_start_us;
		touch_path_stage_add(&touch_path_stats.car_ack_total,
			start_us - event_start_us);
		touch_path_state.car_request_active = 0U;
		touch_path_state.car_request_task = NULL;
	}
	touch_path_stats.car_hid_commands++;
	touch_path_stats.car_hid_bytes += length;
	if ((touch_path_stats.car_hid_min_length == 0U) ||
	    (length < touch_path_stats.car_hid_min_length)) {
		touch_path_stats.car_hid_min_length = length;
	}
	if (length > touch_path_stats.car_hid_max_length) {
		touch_path_stats.car_hid_max_length = length;
	}
	if (touch_path_state.event_active) {
		touch_path_stats.event_overwrites++;
	}
	touch_path_state.event_task = current;
	touch_path_state.event_start_us = event_start_us;
	touch_path_state.event_active = 1U;
	touch_path_state.event_saw_touch = 0U;
	taskEXIT_CRITICAL();

	result = __real_AirPlayResponse_GetInfoHIDReportCommand(body, length);

	taskENTER_CRITICAL();
	{
		uint32_t parser_end_us = hal_read_curtime_us();
		uint32_t elapsed_us = parser_end_us - start_us;
		touch_path_stage_add(&touch_path_stats.event_parser_total,
			elapsed_us);
		touch_path_tail_add(&touch_path_stats.event_parser_tail,
			elapsed_us);
		touch_path_state.last_event_parser_complete_us = parser_end_us;
	}
	if (result == 0) {
		touch_path_stats.parse_errors++;
	}
	if (!touch_path_state.event_saw_touch) {
		touch_path_stats.car_non_touch_hid++;
	}
	touch_path_state.event_active = 0U;
	touch_path_state.event_task = NULL;
	taskEXIT_CRITICAL();
	return result;
}

void __wrap_acc_carplay_cb_hid_report(uint32_t uid, const void *report,
				      uint32_t length)
{
#if CONFIG_TOUCH_MOVE_SAMPLE_HZ > 0
	const uint8_t *bytes = (const uint8_t *)report;
	uint8_t pending_report[5];
	uint32_t pending_uid = 0U;
	uint32_t now_us = hal_read_curtime_us();
	int forward_current = 1;
	int flush_pending = 0;

	if ((bytes == NULL) || (length != sizeof(pending_report))) {
		taskENTER_CRITICAL();
		touch_path_stats.move_sample_passthrough++;
		taskEXIT_CRITICAL();
		__real_acc_carplay_cb_hid_report(uid, report, length);
		return;
	}

	taskENTER_CRITICAL();
	touch_path_stats.move_sample_input++;
	if (touch_action_is_release(bytes[0])) {
		touch_path_stats.move_sample_release++;
		if (touch_path_state.move_sample_pending_valid) {
			pending_uid = touch_path_state.move_sample_pending_uid;
			memcpy(pending_report,
			       touch_path_state.move_sample_pending_report,
			       sizeof(pending_report));
			flush_pending = 1;
			touch_path_stats.move_sample_edge_flushes++;
		}
		touch_path_state.move_sample_pending_valid = 0U;
		touch_path_state.move_sample_active = 0U;
		touch_path_state.move_sample_last_handover_us = 0U;
		touch_path_state.move_sample_next_due_us = 0U;
	} else if (!touch_path_state.move_sample_active) {
		/* DOWN is an ordering edge and must never be sampled away. */
		touch_path_stats.move_sample_down++;
		touch_path_state.move_sample_pending_valid = 0U;
		touch_path_state.move_sample_active = 1U;
		touch_path_state.move_sample_last_handover_us = now_us;
		touch_path_state.move_sample_next_due_us =
			now_us + TOUCH_MOVE_SAMPLE_PERIOD_US;
	} else {
		uint32_t elapsed_us = now_us -
			touch_path_state.move_sample_last_handover_us;

		if (elapsed_us >= TOUCH_MOVE_SAMPLE_PERIOD_US) {
			/* The current report is newer than any saved report, so it is
			 * the correct sample for the newly opened interval. */
			touch_path_state.move_sample_pending_valid = 0U;
			touch_path_state.move_sample_last_handover_us = now_us;
			touch_path_state.move_sample_next_due_us =
				now_us + TOUCH_MOVE_SAMPLE_PERIOD_US;
		} else {
			touch_path_state.move_sample_pending_uid = uid;
			memcpy(touch_path_state.move_sample_pending_report,
			       bytes, sizeof(pending_report));
			touch_path_state.move_sample_pending_valid = 1U;
			touch_path_state.move_sample_next_due_us =
				touch_path_state.move_sample_last_handover_us +
				TOUCH_MOVE_SAMPLE_PERIOD_US;
			touch_path_stats.move_sample_suppressed++;
			touch_path_stats.move_sample_gate_waits++;
			forward_current = 0;
		}
	}
	taskEXIT_CRITICAL();

	/* This is the first external boundary after binary-plist parsing. Filtering
	 * here avoids callback, conversion, HID dispatch, HTTP and crypto work. */
	if (flush_pending) {
		__real_acc_carplay_cb_hid_report(
			pending_uid, pending_report, sizeof(pending_report));
	}
	if (forward_current) {
		__real_acc_carplay_cb_hid_report(uid, report, length);
	}
#else
	__real_acc_carplay_cb_hid_report(uid, report, length);
#endif
}

void __wrap_acc_carplay_cb_send_touch(uint32_t action, uint32_t x, uint32_t y)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	int matched;

	taskENTER_CRITICAL();
	touch_path_stats.car_touch_reports++;
	touch_path_state.current_car_sequence = ++touch_path_state.sequence_next;
	if (touch_path_state.current_car_sequence == 0U) {
		touch_path_state.current_car_sequence =
			++touch_path_state.sequence_next;
	}
	touch_path_state.last_car_sequence =
		touch_path_state.current_car_sequence;
	touch_path_stats.seq_car++;
	touch_path_state.last_car_touch_us = start_us;
	touch_path_state.last_action = action;
	touch_path_state.last_x = x;
	touch_path_state.last_y = y;
	touch_path_state.current_car_is_release =
		touch_action_is_release(action);
	touch_path_state.current_car_release_lt50 = 0U;
	if (touch_action_is_release(action)) {
		touch_path_stats.car_release++;
		if (!touch_path_state.car_contact_active) {
			touch_path_stats.car_duplicate_release++;
		}
		touch_path_state.car_contact_active = 0U;
		touch_path_state.last_car_release_us = start_us;
		touch_path_state.release_sequence =
			touch_path_state.current_car_sequence;
		touch_path_state.release_start_us = start_us;
		touch_path_state.post_release_active = 1U;
		if (touch_path_state.last_car_contact_us != 0U) {
			touch_path_stage_add(&touch_path_stats.contact_to_release,
				start_us - touch_path_state.last_car_contact_us);
		}
	} else {
		touch_path_stats.car_contact++;
		if ((touch_path_state.last_car_release_us != 0U) &&
		    ((start_us - touch_path_state.last_car_release_us) <
		     TOUCH_RELEASE_DEBOUNCE_US)) {
			touch_path_stats.car_release_lt_50ms++;
			touch_path_state.current_car_release_lt50 = 1U;
		}
		if (touch_path_state.post_release_active) {
			touch_path_stats.after_release_car_contact++;
			if (!touch_path_stats.after_release_coord_valid) {
				touch_path_stats.after_release_first_x = x;
				touch_path_stats.after_release_first_y = y;
				touch_path_stats.after_release_coord_valid = 1U;
			}
			touch_path_stats.after_release_last_x = x;
			touch_path_stats.after_release_last_y = y;
		}
		if (!touch_path_state.car_contact_active) {
			touch_path_stats.car_contact_start++;
		}
		touch_path_state.car_contact_active = 1U;
		touch_path_state.last_car_contact_us = start_us;
	}
	matched = touch_path_state.event_active &&
		(touch_path_state.event_task == current);
	if (matched) {
		touch_path_state.event_saw_touch = 1U;
		touch_path_state.touch_active = 1U;
		touch_path_state.touch_forwarded = 0U;
		touch_path_state.touch_start_us = start_us;
		touch_path_stage_add(&touch_path_stats.parse_to_touch,
			start_us - touch_path_state.event_start_us);
	} else {
		touch_path_stats.unmatched_touch++;
	}
	taskEXIT_CRITICAL();

	__real_acc_carplay_cb_send_touch(action, x, y);

	taskENTER_CRITICAL();
	if (matched) {
		touch_path_stage_add(&touch_path_stats.car_touch_callback,
			hal_read_curtime_us() - start_us);
		if (!touch_path_state.touch_forwarded) {
			touch_path_stats.car_touch_suppressed++;
			if (!touch_path_state.current_car_is_release) {
				touch_path_stats.car_release_suppressed++;
				if (touch_path_state.current_car_release_lt50) {
					touch_path_stats.car_release_suppressed_lt50++;
				}
			}
		}
		touch_path_state.touch_active = 0U;
	}
	taskEXIT_CRITICAL();
}

void __wrap_lib_carplay_touch(uint32_t action, uint32_t x, uint32_t y)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	int matched;

	taskENTER_CRITICAL();
	touch_path_stats.local_touch_calls++;
	touch_path_state.last_forward_us = start_us;
	touch_path_state.current_forward_is_release =
		touch_action_is_release(action);
	if (touch_action_is_release(action)) {
		touch_path_stats.forward_release++;
		touch_path_state.forward_contact_active = 0U;
	} else {
		touch_path_stats.forward_contact++;
		touch_path_state.forward_contact_active = 1U;
	}
	matched = touch_path_state.event_active &&
		touch_path_state.touch_active &&
		(touch_path_state.event_task == current);
	if (matched) {
		if (touch_action_is_release(action) !=
		    (touch_path_state.current_car_is_release != 0U)) {
			touch_path_stats.forward_action_mismatch++;
		}
		touch_path_stats.car_touch_forwarded++;
		touch_path_state.current_forward_sequence =
			touch_path_state.current_car_sequence;
		touch_path_state.last_forward_sequence =
			touch_path_state.current_forward_sequence;
		touch_path_stats.seq_forward++;
		if (!touch_action_is_release(action) &&
		    touch_path_state.post_release_active &&
		    (touch_path_state.current_forward_sequence >
		     touch_path_state.release_sequence)) {
			touch_path_stats.after_release_forward_contact++;
		}
		touch_path_state.touch_forwarded = 1U;
		touch_path_state.forward_task = current;
		touch_path_state.forward_start_us = start_us;
		touch_path_state.forward_active = 1U;
		touch_path_state.forward_saw_hid = 0U;
		touch_path_stage_add(&touch_path_stats.touch_to_forward,
			start_us - touch_path_state.touch_start_us);
		if (touch_action_is_release(action)) {
			touch_path_stage_add(&touch_path_stats.release_car_to_forward,
				start_us - touch_path_state.touch_start_us);
		}
	} else {
		touch_path_stats.standalone_forward++;
		touch_path_stats.seq_forward_missing++;
		touch_path_state.current_forward_sequence = 0U;
	}
	taskEXIT_CRITICAL();

	airplay_mutex_profiler_touch_enter();
	__real_lib_carplay_touch(action, x, y);
	airplay_mutex_profiler_touch_leave();

	taskENTER_CRITICAL();
	if (matched) {
		if (!touch_path_state.forward_saw_hid) {
			touch_path_stats.forwarded_without_hid++;
		}
		touch_path_state.forward_active = 0U;
		touch_path_state.forward_task = NULL;
	}
	taskEXIT_CRITICAL();
}

int32_t __wrap_AirPlayReceiverSessionSendHIDReport(
	void *session, uint32_t device_uid, const void *report, uint32_t length)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	uint32_t event_start_us = 0U;
	int is_touch = 0;
	int is_release = 0;
	int is_down = 0;
	int event_matched = 0;
	int matched = 0;
	int32_t result;

	taskENTER_CRITICAL();
	touch_path_stats.iphone_hid_calls++;
	touch_hid_format_record(report, length);
	if ((report != NULL) && (length == 5U)) {
		const uint8_t *bytes = (const uint8_t *)report;

		is_touch = 1;
		is_release = touch_action_is_release(bytes[0]);
		is_down = !is_release && !touch_path_state.report5_touch_active;
	}
	touch_report5_record(device_uid, report, length, start_us);
	touch_path_state.hid_sequence = 0U;
	event_matched = touch_path_state.event_active &&
		(touch_path_state.event_task == current);
	if (event_matched) {
		event_start_us = touch_path_state.event_start_us;
		touch_path_stats.iphone_hid_event_matched++;
		touch_path_stage_add(&touch_path_stats.event_to_hid_call,
			start_us - event_start_us);
		touch_path_tail_add(&touch_path_stats.event_to_hid_tail,
			start_us - event_start_us);
	}
	if (length == 5U) {
		const uint8_t *bytes = (const uint8_t *)report;
		matched = touch_path_state.event_active &&
			touch_path_state.forward_active &&
			(touch_path_state.forward_task == current);
		if (matched) {
			is_release = (bytes != NULL) &&
				touch_action_is_release(bytes[0]);
			touch_path_stats.iphone_touch_calls++;
			if (is_release) {
				touch_path_stats.iphone_release++;
				touch_path_state.iphone_contact_active = 0U;
				touch_path_state.last_hid_release_us = start_us;
			} else {
				touch_path_stats.iphone_contact++;
				touch_path_state.iphone_contact_active = 1U;
			}
			event_start_us = touch_path_state.event_start_us;
			touch_path_state.forward_saw_hid = 1U;
			touch_path_state.hid_sequence =
				touch_path_state.current_forward_sequence;
			touch_path_state.last_hid_sequence =
				touch_path_state.hid_sequence;
			touch_path_stats.seq_hid++;
			if (!is_release && touch_path_state.post_release_active &&
			    (touch_path_state.hid_sequence >
			     touch_path_state.release_sequence)) {
				touch_path_stats.after_release_hid_contact++;
			}
			touch_path_stage_add(&touch_path_stats.forward_to_hid,
				start_us - touch_path_state.forward_start_us);
			if (is_release !=
			    (touch_path_state.current_forward_is_release != 0U)) {
				touch_path_stats.iphone_action_mismatch++;
			}
			if (is_release) {
				touch_path_stage_add(
					&touch_path_stats.release_car_to_hid,
					start_us - touch_path_state.touch_start_us);
			}
		} else {
			touch_path_stats.standalone_hid++;
			touch_path_stats.seq_hid_missing++;
			touch_path_state.hid_sequence = 0U;
		}
	}
	touch_path_state.last_hid_us = start_us;
	if ((touch_path_state.last_car_touch_us != 0U) &&
	    ((start_us - touch_path_state.last_car_touch_us) > 100000U)) {
		touch_path_stats.quiet_hid++;
	}
	touch_path_state.hid_task = current;
	touch_path_state.hid_start_us = start_us;
	touch_path_state.hid_source_start_us = event_matched ?
		event_start_us : start_us;
	touch_path_state.hid_action_start_us = matched ?
		touch_path_state.touch_start_us : start_us;
	/* Transport behaviour must come directly from the HID report.  The
	 * profiler's vehicle/forward correlation is diagnostic only and may be
	 * disabled or legitimately miss an event. */
	touch_path_state.hid_is_release = is_release != 0;
	touch_path_state.hid_is_down = is_down != 0;
	touch_path_state.hid_is_touch = is_touch != 0;
	touch_path_state.hid_active = 1U;
	taskEXIT_CRITICAL();

	result = __real_AirPlayReceiverSessionSendHIDReport(
		session, device_uid, report, length);

	taskENTER_CRITICAL();
	if (touch_path_state.hid_task == current) {
		touch_path_state.hid_active = 0U;
		touch_path_state.hid_task = NULL;
	}
	{
		uint32_t elapsed_us = hal_read_curtime_us() - start_us;
		touch_path_stage_add(&touch_path_stats.iphone_hid_call_all,
			elapsed_us);
		touch_path_tail_add(&touch_path_stats.iphone_hid_call_tail,
			elapsed_us);
	}
	if (result != 0) {
		touch_path_stats.iphone_hid_errors++;
	}
	if (event_matched) {
		touch_path_stage_add(&touch_path_stats.event_to_hid_return,
			hal_read_curtime_us() - event_start_us);
	}
	if (matched) {
		uint32_t end_us = hal_read_curtime_us();

		touch_path_stage_add(&touch_path_stats.iphone_hid_enqueue,
			end_us - start_us);
		touch_path_stage_add(&touch_path_stats.bridge_end_to_end,
			end_us - event_start_us);
	}
	taskEXIT_CRITICAL();
	return result;
}

uint32_t carbox_touch_dispatch_sync_begin(void)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t token = 0U;

	taskENTER_CRITICAL();
	if (touch_path_state.hid_active &&
	    (touch_path_state.hid_task == current)) {
		token = ++touch_path_state.dispatch_token_next;
		if (token == 0U) {
			token = ++touch_path_state.dispatch_token_next;
		}
		touch_path_state.dispatch_token_active = token;
		touch_path_state.dispatch_source_start_us =
			touch_path_state.hid_source_start_us;
		touch_path_state.dispatch_hid_start_us =
			touch_path_state.hid_start_us;
		touch_path_state.dispatch_action_start_us =
			touch_path_state.hid_action_start_us;
		touch_path_state.dispatch_is_release =
			touch_path_state.hid_is_release;
		touch_path_state.dispatch_is_down =
			touch_path_state.hid_is_down;
		touch_path_state.dispatch_is_touch =
			touch_path_state.hid_is_touch;
		touch_path_state.dispatch_sequence =
			touch_path_state.hid_sequence;
		touch_path_stats.dispatch_calls++;
	}
	taskEXIT_CRITICAL();
	return token;
}

void carbox_touch_dispatch_sync_callback_begin(uint32_t token)
{
	taskENTER_CRITICAL();
	if ((token != 0U) &&
	    (touch_path_state.dispatch_token_active == token)) {
		touch_path_state.dispatch_callback_task =
			xTaskGetCurrentTaskHandle();
		touch_path_state.dispatch_callback_active = 1U;
	}
	taskEXIT_CRITICAL();
}

void carbox_touch_dispatch_sync_callback_end(uint32_t token)
{
	taskENTER_CRITICAL();
	if (touch_path_state.dispatch_token_active == token) {
		touch_path_state.dispatch_callback_active = 0U;
		touch_path_state.dispatch_callback_task = NULL;
	}
	taskEXIT_CRITICAL();
}

void carbox_touch_dispatch_sync_complete(uint32_t token,
					 uint32_t submit_us,
					 uint32_t callback_start_us,
					 uint32_t callback_end_us,
					 uint32_t return_us,
					 int callback_called)
{
	taskENTER_CRITICAL();
	if (touch_path_state.dispatch_token_active == token) {
		if (callback_called) {
			uint32_t wait_us = callback_start_us - submit_us;
			uint32_t exec_us = callback_end_us - callback_start_us;
			uint32_t total_us = return_us - submit_us;

			touch_path_stage_add(&touch_path_stats.dispatch_wait,
				wait_us);
			touch_path_stage_add(&touch_path_stats.dispatch_exec,
				exec_us);
			touch_path_stage_add(&touch_path_stats.dispatch_total,
				total_us);
			touch_path_tail_add(&touch_path_stats.dispatch_wait_tail,
				wait_us);
			touch_path_tail_add(&touch_path_stats.dispatch_exec_tail,
				exec_us);
			touch_path_tail_add(&touch_path_stats.dispatch_total_tail,
				total_us);
		} else {
			touch_path_stats.dispatch_missing++;
		}
		touch_path_state.dispatch_callback_active = 0U;
		touch_path_state.dispatch_callback_task = NULL;
		touch_path_state.dispatch_token_active = 0U;
	}
	taskEXIT_CRITICAL();
}

int32_t __wrap_HTTPClientSendMessage(void *client, void *message)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t enqueue_us = hal_read_curtime_us();
	uint32_t source_start_us = 0U;
	uint32_t hid_start_us = 0U;
	uint32_t action_start_us = 0U;
	int matched = 0;
	int32_t result;

	taskENTER_CRITICAL();
	matched = touch_path_state.dispatch_callback_active &&
		(touch_path_state.dispatch_callback_task == current);
	if (matched) {
		source_start_us = touch_path_state.dispatch_source_start_us;
		hid_start_us = touch_path_state.dispatch_hid_start_us;
		action_start_us = touch_path_state.dispatch_action_start_us;
		if (touch_http_allocate(client, message, source_start_us,
					hid_start_us, action_start_us, enqueue_us,
					touch_path_state.dispatch_is_touch,
					touch_path_state.dispatch_is_release,
					touch_path_state.dispatch_is_down,
					touch_path_state.dispatch_sequence) != NULL) {
			touch_path_stats.http_enqueued++;
			touch_path_stage_add(&touch_path_stats.event_to_http_enqueue,
				enqueue_us - source_start_us);
			if (touch_path_state.dispatch_sequence != 0U) {
				touch_path_stats.seq_http_enqueue++;
				touch_path_state.last_http_enqueue_sequence =
					touch_path_state.dispatch_sequence;
			} else {
				touch_path_stats.seq_http_missing++;
			}
			touch_path_stage_add(&touch_path_stats.hid_to_http_enqueue,
				enqueue_us - hid_start_us);
			if (touch_path_state.dispatch_is_release) {
				touch_path_stats.release_http_enqueued++;
				touch_path_stage_add(
					&touch_path_stats.release_car_to_http_enqueue,
					enqueue_us - action_start_us);
			}
		} else {
			touch_path_stats.http_slot_full++;
			matched = 0;
		}
	}
	taskEXIT_CRITICAL();

#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
	if (matched) {
		result = touch_http_bypass_enqueue(client, message);
	} else
#endif
	{
#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
		taskENTER_CRITICAL();
		if ((touch_path_state.bypass_client == client) &&
		    ((touch_path_state.bypass_depth != 0U) ||
		     touch_path_state.bypass_drain_scheduled)) {
			touch_path_stats.http_bypass_vendor_overlap++;
		}
		taskEXIT_CRITICAL();
#endif
		result = __real_HTTPClientSendMessage(client, message);
	}
	if (matched && (result != 0)) {
		taskENTER_CRITICAL();
		{
			touch_http_slot_t *slot = touch_http_find(message);
			if (slot != NULL) {
				memset(slot, 0, sizeof(*slot));
			}
			touch_path_stats.http_enqueue_errors++;
		}
		taskEXIT_CRITICAL();
	}
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	if (matched && (result == 0)) {
		touch_http_pipeline_schedule(client);
	}
#endif
	return result;
}

int32_t __wrap_HTTPMessageWriteMessage(
	void *message, void *write_f, void *write_context)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	int car_ack = 0;
	int touch_http = 0;
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	int pipeline_pump_write = 0;
	int pipeline_fake_complete = 0;
	void *pipeline_client = NULL;
#endif
#if CONFIG_CAR_ACK_TCP_PROFILE
	int car_ack_socket = -1;
	int mark_car_ack = 0;
	uint32_t car_ack_mark_us = 0U;
#endif
	int32_t result;

	taskENTER_CRITICAL();
	car_ack = touch_path_state.car_request_active &&
		(touch_path_state.car_request_task == current);
	if (car_ack) {
		touch_path_stats.car_ack_write_calls++;
#if CONFIG_CAR_ACK_TCP_PROFILE
		if (touch_path_state.car_socket_valid) {
			car_ack_socket = touch_path_state.car_socket_fd;
		}
#endif
		if (!touch_path_state.car_ack_write_started) {
			touch_path_state.car_ack_write_started = 1U;
			touch_path_state.car_ack_write_start_us = start_us;
		}
	}
	{
		touch_http_slot_t *slot = touch_http_find(message);
		if (slot != NULL) {
			touch_http = 1;
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
			pipeline_client = slot->client;
			pipeline_pump_write =
				(touch_path_state.pipeline_write_task == current) &&
				(touch_path_state.pipeline_write_message == message);
			if (!pipeline_pump_write &&
			    (slot->pipeline_state ==
			     TOUCH_HTTP_PIPE_PREWRITTEN)) {
				/* The request bytes are already on the wire.  Let the
				 * vendor state machine advance to parsing this message's
				 * response without writing or encrypting it twice. */
				pipeline_fake_complete = 1;
				slot->pipeline_state = TOUCH_HTTP_PIPE_NONE;
				touch_path_stats.http_pipe_fake_complete++;
			}
#endif
			touch_path_stats.http_write_calls++;
			if (!slot->write_started) {
				uint32_t i;
				uint32_t wire_inflight = 0U;

				for (i = 0U; i < TOUCH_HTTP_SLOTS; i++) {
					if (touch_http_slots[i].active &&
					    touch_http_slots[i].write_completed) {
						wire_inflight++;
					}
				}
				if (wire_inflight != 0U) {
					touch_path_stats.http_write_while_pending++;
				}
				wire_inflight++;
				if (wire_inflight >
				    touch_path_stats.http_wire_inflight_peak) {
					touch_path_stats.http_wire_inflight_peak =
						wire_inflight;
				}
				if ((touch_path_state.last_http_response_us != 0U) &&
				    (touch_path_state.last_http_response_client ==
				     slot->client)) {
					touch_path_stage_add(
						&touch_path_stats.http_response_to_next_write,
						start_us -
							touch_path_state.last_http_response_us);
				}
				slot->write_started = 1U;
				slot->write_start_us = start_us;
				touch_path_stage_add(
					&touch_path_stats.http_enqueue_to_write,
					start_us - slot->enqueue_us);
			}
		}
	}
	taskEXIT_CRITICAL();

#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	if (pipeline_fake_complete) {
		result = 0;
	} else
#endif
	{
		result = __real_HTTPMessageWriteMessage(
			message, write_f, write_context);
	}

	taskENTER_CRITICAL();
	if (car_ack) {
		if (result == 0) {
			uint32_t ack_end_us = hal_read_curtime_us();
			uint32_t elapsed_us = ack_end_us -
				touch_path_state.car_ack_write_start_us;

			touch_path_stats.car_ack_write_complete++;
			touch_path_stage_add(&touch_path_stats.car_ack_write,
				elapsed_us);
			touch_path_tail_add(&touch_path_stats.car_ack_write_tail,
				elapsed_us);
			touch_path_state.last_car_ack_complete_us = ack_end_us;
#if CONFIG_CAR_ACK_TCP_PROFILE
			if (car_ack_socket >= 0) {
				mark_car_ack = 1;
				car_ack_mark_us = ack_end_us;
			}
#endif
		} else if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
			touch_path_stats.car_ack_write_deferred++;
		} else {
			touch_path_stats.car_ack_write_errors++;
		}
	}
	if (touch_http) {
		touch_http_slot_t *slot = touch_http_find(message);
		if (slot != NULL) {
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
			if (!pipeline_pump_write && !pipeline_fake_complete) {
				if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
					slot->pipeline_state =
						TOUCH_HTTP_PIPE_PARTIAL;
				} else {
					slot->pipeline_state =
						TOUCH_HTTP_PIPE_NONE;
				}
			}
#endif
			if ((result == 0) && !slot->write_completed) {
				uint32_t end_us = hal_read_curtime_us();
				touch_path_stats.http_write_complete++;
				touch_path_stage_add(&touch_path_stats.event_to_http_write,
					end_us - slot->source_start_us);
				touch_path_tail_add(&touch_path_stats.event_to_http_write_tail,
					end_us - slot->source_start_us);
				slot->write_completed = 1U;
				slot->write_complete_us = end_us;
				if (slot->sequence != 0U) {
					touch_path_stats.seq_http_write++;
					if ((touch_path_state.last_http_write_sequence != 0U) &&
					    (slot->sequence <=
					     touch_path_state.last_http_write_sequence)) {
						touch_path_stats.seq_write_reorder++;
					}
					touch_path_state.last_http_write_sequence =
						slot->sequence;
				} else {
					touch_path_stats.seq_http_missing++;
				}
				touch_path_state.last_http_write_us = end_us;
				if (!slot->is_release &&
				    touch_path_state.post_release_active &&
				    (slot->sequence > touch_path_state.release_sequence)) {
					touch_path_stats.after_release_http_write++;
				}
				if ((touch_path_state.last_car_touch_us != 0U) &&
				    ((end_us - touch_path_state.last_car_touch_us) >
				     100000U)) {
					touch_path_stats.quiet_http_write++;
				}
				touch_path_stage_add(
					&touch_path_stats.http_write_total,
					end_us - slot->write_start_us);
				if (slot->is_release) {
					touch_path_stats.release_http_write_complete++;
					touch_path_stage_add(
						&touch_path_stats.release_car_to_http_write,
						end_us - slot->action_start_us);
				}
			} else if (result == TOUCH_OSSTATUS_WOULD_BLOCK) {
				touch_path_stats.http_write_deferred++;
			} else if (result != 0) {
				touch_path_stats.http_write_errors++;
			}
		} else {
			touch_path_stats.http_orphan_write++;
		}
	}
	taskEXIT_CRITICAL();
#if CONFIG_CAR_ACK_TCP_PROFILE
	if (mark_car_ack) {
		(void)lwip_diag_car_ack_mark(car_ack_socket, car_ack_mark_us);
	}
#endif
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	if (pipeline_fake_complete && (pipeline_client != NULL)) {
		/* This callback runs on the same serial queue.  The newly queued
		 * pump observes state 4 after the current state-machine turn exits. */
		touch_http_pipeline_schedule(pipeline_client);
	}
#endif
	return result;
}

#if CONFIG_IPHONE_HTTP_RX_PROFILE
ssize_t __wrap_lwip_read(int socket_fd, void *buffer, size_t length)
{
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	uint32_t start_us = hal_read_curtime_us();
	ssize_t result = __real_lwip_read(socket_fd, buffer, length);
	uint32_t end_us = hal_read_curtime_us();
	touch_http_slot_t *slot = NULL;
	struct lwip_rx_post_diag post;
	int post_valid = 0;

	memset(&post, 0, sizeof(post));
	taskENTER_CRITICAL();
	if (touch_path_state.iphone_http_read_task == current) {
		slot = touch_path_state.iphone_http_read_slot;
	}
	taskEXIT_CRITICAL();
	if ((slot != NULL) && (result > 0)) {
		post_valid = lwip_diag_rx_post_snapshot(socket_fd, &post) == 0;
		taskENTER_CRITICAL();
		/* The message slot remains owned by HTTPClient throughout this
		 * synchronous read call.  Only the first packet of a response is
		 * used for the end-to-end split. */
		touch_path_stats.iphone_rx_read_calls++;
		touch_path_stats.iphone_rx_read_bytes += (uint32_t)result;
		touch_path_stage_add(&touch_path_stats.iphone_rx_read_call,
			end_us - start_us);
		if (post_valid && slot->active &&
		    (post.generation != slot->response_last_generation)) {
			slot->response_last_generation = post.generation;
			if (!slot->response_post_valid) {
				slot->response_post_valid = 1U;
				slot->response_first_post_us = post.post_us;
				slot->response_first_read_us = start_us;
				if ((int32_t)(start_us - post.post_us) >= 0) {
					touch_path_stage_add(
						&touch_path_stats.iphone_rx_post_to_read,
						start_us - post.post_us);
				} else {
					/* Data arrived while lwip_read was active. */
					touch_path_stage_add(
						&touch_path_stats.iphone_rx_post_to_read,
						0U);
				}
			}
		} else if (!post_valid) {
			touch_path_stats.iphone_rx_post_missing++;
		}
		taskEXIT_CRITICAL();
	}
	return result;
}
#endif

int32_t __wrap_HTTPMessageReadMessage(
	void *message, void *read_f, void *read_context)
{
	int touch_http;
	int car_event_read;
	TaskHandle_t current = xTaskGetCurrentTaskHandle();
	const char *task_name = pcTaskGetName(current);
	uint32_t start_us = hal_read_curtime_us();
	int32_t result;

	taskENTER_CRITICAL();
	touch_http = touch_http_find(message) != NULL;
	if (touch_http) {
		const char *reader_name = pcTaskGetName(current);
		UBaseType_t reader_priority = uxTaskPriorityGet(current);

		touch_path_stats.http_response_calls++;
		if ((touch_path_stats.iphone_rx_reader_gcd +
		     touch_path_stats.iphone_rx_reader_other) == 0U) {
			touch_path_stats.iphone_rx_reader_prio_min = reader_priority;
			touch_path_stats.iphone_rx_reader_prio_max = reader_priority;
		} else {
			if (reader_priority <
			    touch_path_stats.iphone_rx_reader_prio_min) {
				touch_path_stats.iphone_rx_reader_prio_min =
					reader_priority;
			}
			if (reader_priority >
			    touch_path_stats.iphone_rx_reader_prio_max) {
				touch_path_stats.iphone_rx_reader_prio_max =
					reader_priority;
			}
		}
		if ((reader_name != NULL) &&
		    (strcmp(reader_name, "gcd-work") == 0)) {
			touch_path_stats.iphone_rx_reader_gcd++;
		} else {
			touch_path_stats.iphone_rx_reader_other++;
		}
#if CONFIG_IPHONE_HTTP_RX_PROFILE
		touch_path_state.iphone_http_read_task = current;
		touch_path_state.iphone_http_read_slot =
			touch_http_find(message);
#endif
	}
	taskEXIT_CRITICAL();
	/* HTTPMessageReadMessage is shared by pairing, control and event
	 * connections.  Only TCPClient owns the vehicle event socket. */
	car_event_read = !touch_http && task_name != NULL &&
		strcmp(task_name, "TCPClient") == 0;
	if (car_event_read) {
		taskENTER_CRITICAL();
		if (touch_path_state.last_car_read_complete_us != 0U) {
			touch_path_stage_add(&touch_path_stats.read_complete_to_rearm,
				start_us - touch_path_state.last_car_read_complete_us);
		}
		if (touch_path_state.last_event_parser_complete_us != 0U) {
			touch_path_stage_add(&touch_path_stats.parser_complete_to_rearm,
				start_us - touch_path_state.last_event_parser_complete_us);
		}
		if (touch_path_state.last_car_ack_complete_us != 0U) {
			touch_path_stage_add(&touch_path_stats.ack_complete_to_rearm,
				start_us - touch_path_state.last_car_ack_complete_us);
		}
		taskEXIT_CRITICAL();
	}

	result = __real_HTTPMessageReadMessage(message, read_f, read_context);
	if (touch_http) {
		taskENTER_CRITICAL();
#if CONFIG_IPHONE_HTTP_RX_PROFILE
		if (touch_path_state.iphone_http_read_task == current) {
			touch_path_state.iphone_http_read_task = NULL;
			touch_path_state.iphone_http_read_slot = NULL;
		}
#endif
		taskEXIT_CRITICAL();
	}
	if (car_event_read && (result == 0)) {
		uint32_t complete_us = hal_read_curtime_us();
		uint32_t pending_bytes = 0U;
		int socket_fd = (int)(uintptr_t)read_context;

		int pending_valid = socket_fd >= 0 &&
			(lwip_ioctl(socket_fd, FIONREAD, &pending_bytes) == 0);

		taskENTER_CRITICAL();
		touch_path_state.last_car_read_complete_us = complete_us;
		touch_read_origin_record(current, message,
			complete_us, complete_us - start_us, socket_fd,
			pending_bytes, pending_valid);
		taskEXIT_CRITICAL();
	}
	if (touch_http && (result == 0)) {
		uint32_t end_us = hal_read_curtime_us();
		int32_t status = *(const int32_t *)
			((const uint8_t *)message + TOUCH_HTTP_STATUS_OFFSET);

		taskENTER_CRITICAL();
		{
			touch_http_slot_t *slot = touch_http_find(message);
			if (slot != NULL) {
				touch_path_stats.http_response_complete++;
				if ((status >= 200) && (status <= 299)) {
					touch_path_stats.http_response_2xx++;
					if (slot->is_release) {
						touch_path_stats.release_http_response_2xx++;
					}
				} else {
					touch_path_stats.http_response_non_2xx++;
				}
				if ((slot->sequence != 0U) &&
				    (touch_path_state.last_http_response_sequence != 0U) &&
				    (slot->sequence <=
				     touch_path_state.last_http_response_sequence)) {
					touch_path_stats.http_response_reorder++;
				}
				if (slot->sequence != 0U) {
					touch_path_state.last_http_response_sequence =
						slot->sequence;
				}
				touch_path_state.last_http_response_us = end_us;
				touch_path_state.last_http_response_client = slot->client;
				if (slot->write_completed) {
					touch_path_stage_add(
						&touch_path_stats.http_write_to_response,
						end_us - slot->write_complete_us);
					if (slot->response_post_valid &&
					    ((int32_t)(slot->response_first_post_us -
					      slot->write_complete_us) >= 0)) {
						touch_path_stage_add(
							&touch_path_stats.iphone_rx_write_to_post,
							slot->response_first_post_us -
								slot->write_complete_us);
						touch_path_stage_add(
							&touch_path_stats.iphone_rx_post_to_complete,
							end_us -
								slot->response_first_post_us);
						touch_path_stage_add(
							&touch_path_stats.iphone_rx_read_to_complete,
							end_us -
								slot->response_first_read_us);
					}
				}
				touch_path_stage_add(
					&touch_path_stats.http_enqueue_to_response,
					end_us - slot->enqueue_us);
				touch_path_stage_add(
					&touch_path_stats.wire_end_to_end,
					end_us - slot->source_start_us);
				touch_path_tail_add(
					&touch_path_stats.event_to_http_200_tail,
					end_us - slot->source_start_us);
				if (slot->is_release) {
					touch_path_stage_add(
						&touch_path_stats.release_car_to_http_response,
						end_us - slot->action_start_us);
				}
				memset(slot, 0, sizeof(*slot));
			} else {
				touch_path_stats.http_orphan_response++;
			}
		}
		taskEXIT_CRITICAL();
	}
	return result;
}

static unsigned long touch_path_stage_avg(const touch_path_stage_t *stage)
{
	return (unsigned long)(stage->samples != 0U ?
		stage->sum_us / stage->samples : 0U);
}

void carbox_touch_path_profiler_report(uint32_t sequence)
{
	carbox_car_ack_response_cache_report(sequence);
#if CONFIG_TOUCH_PATH_PROFILE
	static touch_path_stats_t stats;
	uint32_t now_us = hal_read_curtime_us();
	uint32_t last_car_touch_us;
	uint32_t last_forward_us;
	uint32_t last_action;
	uint32_t last_x;
	uint32_t last_y;
	uint32_t last_car_contact_us;
	uint32_t last_car_release_us;
	uint32_t last_hid_release_us;
	uint32_t last_car_sequence;
	uint32_t last_forward_sequence;
	uint32_t last_hid_sequence;
	uint32_t last_http_enqueue_sequence;
	uint32_t last_http_write_sequence;
	uint32_t release_sequence;
	uint32_t release_start_us;
	uint32_t last_hid_us;
	uint32_t last_http_write_us;
	uint32_t http_depth;
	uint32_t http_queued = 0U;
	uint32_t http_wire_inflight = 0U;
	uint32_t http_oldest_age_us = 0U;
	uint32_t i;
	uint8_t car_contact_active;
	uint8_t forward_contact_active;
	uint8_t iphone_contact_active;
	uint8_t post_release_active;
	uint8_t car_socket_valid;
	uint8_t car_socket_nodelay_valid;
	uint8_t car_socket_nodelay;
	uint32_t car_rx_drain_streak;
	uint32_t advertised_touch_uid;
	uint32_t advertised_descriptor_length;
	uint32_t advertised_x_max;
	uint32_t advertised_y_max;
	uint32_t last_report_release_us;
	uint32_t move_sample_next_due_us;
	uint8_t advertised_touch_valid;
	uint8_t report5_touch_active;
	uint8_t move_sample_active;
	int car_socket_fd;
#if CONFIG_GMT_TIME_PROFILE
	time_t gmt_epoch;
	struct tm gmt_utc;
	long gmt_delta_seconds = 0L;
	uint32_t gmt_elapsed_ms = 0U;
	uint8_t gmt_utc_valid;
	uint8_t gmt_previous_valid;
#endif
#if CONFIG_CAR_ACK_TCP_PROFILE
	struct lwip_car_ack_diag car_tcp_ack;
	int car_tcp_ack_valid = 0;

	memset(&car_tcp_ack, 0, sizeof(car_tcp_ack));
#endif

	taskENTER_CRITICAL();
	stats = touch_path_stats;
	memset(&touch_path_stats, 0, sizeof(touch_path_stats));
	last_car_touch_us = touch_path_state.last_car_touch_us;
	last_forward_us = touch_path_state.last_forward_us;
	last_action = touch_path_state.last_action;
	last_x = touch_path_state.last_x;
	last_y = touch_path_state.last_y;
	last_car_contact_us = touch_path_state.last_car_contact_us;
	last_car_release_us = touch_path_state.last_car_release_us;
	last_hid_release_us = touch_path_state.last_hid_release_us;
	last_car_sequence = touch_path_state.last_car_sequence;
	last_forward_sequence = touch_path_state.last_forward_sequence;
	last_hid_sequence = touch_path_state.last_hid_sequence;
	last_http_enqueue_sequence =
		touch_path_state.last_http_enqueue_sequence;
	last_http_write_sequence = touch_path_state.last_http_write_sequence;
	release_sequence = touch_path_state.release_sequence;
	release_start_us = touch_path_state.release_start_us;
	last_hid_us = touch_path_state.last_hid_us;
	last_http_write_us = touch_path_state.last_http_write_us;
	car_contact_active = touch_path_state.car_contact_active;
	forward_contact_active = touch_path_state.forward_contact_active;
	iphone_contact_active = touch_path_state.iphone_contact_active;
	post_release_active = touch_path_state.post_release_active;
	car_socket_valid = touch_path_state.car_socket_valid;
	car_socket_nodelay_valid = touch_path_state.car_socket_nodelay_valid;
	car_socket_nodelay = touch_path_state.car_socket_nodelay;
	car_socket_fd = touch_path_state.car_socket_fd;
	car_rx_drain_streak = touch_path_state.car_rx_drain_streak;
	advertised_touch_uid = touch_path_state.advertised_touch_uid;
	advertised_descriptor_length =
		touch_path_state.advertised_descriptor_length;
	advertised_x_max = touch_path_state.advertised_x_max;
	advertised_y_max = touch_path_state.advertised_y_max;
	last_report_release_us = touch_path_state.last_report_release_us;
	advertised_touch_valid = touch_path_state.advertised_touch_valid;
	report5_touch_active = touch_path_state.report5_touch_active;
	move_sample_active = touch_path_state.move_sample_active;
	move_sample_next_due_us = touch_path_state.move_sample_next_due_us;
	http_depth = touch_http_live_depth();
	for (i = 0U; i < TOUCH_HTTP_SLOTS; i++) {
		if (touch_http_slots[i].active) {
			uint32_t age_us = now_us - touch_http_slots[i].enqueue_us;
			if (touch_http_slots[i].write_completed) {
				http_wire_inflight++;
			} else {
				http_queued++;
			}
			if (age_us > http_oldest_age_us) {
				http_oldest_age_us = age_us;
			}
		}
	}
	if (http_depth > stats.http_depth_peak) {
		stats.http_depth_peak = http_depth;
	}
	taskEXIT_CRITICAL();

	if (car_socket_valid && !car_socket_nodelay_valid) {
		int nodelay = 0;
		socklen_t option_length = sizeof(nodelay);

		if (lwip_getsockopt(car_socket_fd, IPPROTO_TCP, TCP_NODELAY,
				    &nodelay, &option_length) == 0) {
			car_socket_nodelay = nodelay != 0;
			car_socket_nodelay_valid = 1U;
			taskENTER_CRITICAL();
			touch_path_state.car_socket_nodelay = car_socket_nodelay;
			touch_path_state.car_socket_nodelay_valid = 1U;
			taskEXIT_CRITICAL();
		}
	}
#if CONFIG_CAR_ACK_TCP_PROFILE
	if (car_socket_valid &&
	    (lwip_diag_car_ack_snapshot(car_socket_fd, &car_tcp_ack, 1,
					now_us) == 0)) {
		car_tcp_ack_valid = 1;
	}
#endif

#if CONFIG_GMT_TIME_PROFILE
	gmt_epoch = time(NULL);
	gmt_utc_valid = gmtime_r(&gmt_epoch, &gmt_utc) != NULL;
	gmt_previous_valid = touch_gmt_previous_valid;
	if (gmt_previous_valid) {
		gmt_delta_seconds = (long)(gmt_epoch - touch_gmt_previous_epoch);
		gmt_elapsed_ms = (now_us - touch_gmt_previous_tick_us) / 1000U;
	}
	touch_gmt_previous_epoch = gmt_epoch;
	touch_gmt_previous_tick_us = now_us;
	touch_gmt_previous_valid = 1U;
	if (gmt_utc_valid) {
		rt_printf("[GMTTIME][%lu] epoch=%ld delta_s=%ld elapsed_ms=%lu "
			  "running/regressed=%u/%u utc=%04d-%02d-%02dT%02d:%02d:%02dZ\r\n",
			  (unsigned long)sequence,
			  (long)gmt_epoch,
			  gmt_previous_valid ? gmt_delta_seconds : 0L,
			  (unsigned long)gmt_elapsed_ms,
			  (unsigned)(gmt_previous_valid &&
				gmt_delta_seconds > 0L),
			  (unsigned)(gmt_previous_valid &&
				gmt_delta_seconds < 0L),
			  gmt_utc.tm_year + 1900,
			  gmt_utc.tm_mon + 1,
			  gmt_utc.tm_mday,
			  gmt_utc.tm_hour,
			  gmt_utc.tm_min,
			  gmt_utc.tm_sec);
	} else {
		rt_printf("[GMTTIME][%lu] epoch=%ld delta_s=%ld elapsed_ms=%lu "
			  "running/regressed=%u/%u utc=invalid\r\n",
			  (unsigned long)sequence,
			  (long)gmt_epoch,
			  gmt_previous_valid ? gmt_delta_seconds : 0L,
			  (unsigned long)gmt_elapsed_ms,
			  (unsigned)(gmt_previous_valid &&
				gmt_delta_seconds > 0L),
			  (unsigned)(gmt_previous_valid &&
				gmt_delta_seconds < 0L));
	}
#endif

#if TOUCH_PATH_VERBOSE_REPORT
	rt_printf("[TOUCHBRIDGE][%lu] car hid/touch/non_touch/forward/suppress/parse_err="
		  "%lu/%lu/%lu/%lu/%lu/%lu bytes=%lu local_touch=%lu "
		  "iphone hid/touch5/error=%lu/%lu/%lu "
		  "standalone forward/hid5=%lu/%lu "
		  "miss touch/no_hid/overwrite=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_hid_commands,
		  (unsigned long)stats.car_touch_reports,
		  (unsigned long)stats.car_non_touch_hid,
		  (unsigned long)stats.car_touch_forwarded,
		  (unsigned long)stats.car_touch_suppressed,
		  (unsigned long)stats.parse_errors,
		  (unsigned long)stats.car_hid_bytes,
		  (unsigned long)stats.local_touch_calls,
		  (unsigned long)stats.iphone_hid_calls,
		  (unsigned long)stats.iphone_touch_calls,
		  (unsigned long)stats.iphone_hid_errors,
		  (unsigned long)stats.standalone_forward,
		  (unsigned long)stats.standalone_hid,
		  (unsigned long)stats.unmatched_touch,
		  (unsigned long)stats.forwarded_without_hid,
		  (unsigned long)stats.event_overwrites);
	rt_printf("[TOUCHBRIDGE][%lu] hid_format car_body min/max=%lu/%lu "
		  "out calls/event_pair/bytes=%lu/%lu/%lu "
		  "len <=4/5/6-8/9-16/17-32/33-64/65-128/>128="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu byte0_overflow=%lu ids=",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_hid_min_length,
		  (unsigned long)stats.car_hid_max_length,
		  (unsigned long)stats.iphone_hid_calls,
		  (unsigned long)stats.iphone_hid_event_matched,
		  (unsigned long)stats.iphone_hid_bytes,
		  (unsigned long)stats.iphone_hid_len_le4,
		  (unsigned long)stats.iphone_hid_len_eq5,
		  (unsigned long)stats.iphone_hid_len_6_8,
		  (unsigned long)stats.iphone_hid_len_9_16,
		  (unsigned long)stats.iphone_hid_len_17_32,
		  (unsigned long)stats.iphone_hid_len_33_64,
		  (unsigned long)stats.iphone_hid_len_65_128,
		  (unsigned long)stats.iphone_hid_len_gt128,
		  (unsigned long)stats.iphone_hid_byte0_overflow);
	for (i = 0U; i < TOUCH_HID_BYTE0_SLOTS; i++) {
		const touch_hid_byte0_stat_t *id = &stats.iphone_hid_byte0[i];
		if (id->valid) {
			rt_printf(" %02x:%lu(%u-%u)", id->byte0,
				  (unsigned long)id->count,
				  (unsigned)id->min_len, (unsigned)id->max_len);
		}
	}
	rt_printf("\r\n");
#endif
#if CONFIG_TOUCH_PATH_REPORT_DETAIL
	rt_printf("[TOUCHSTATE][%lu] action_semantics 0=release nonzero=contact/move "
		  "car contact/release/start/down_lt50/suppressed_down/supp_down_lt50/duplicate="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu forward contact/release/mismatch="
		  "%lu/%lu/%lu iphone contact/release/mismatch=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_contact,
		  (unsigned long)stats.car_release,
		  (unsigned long)stats.car_contact_start,
		  (unsigned long)stats.car_release_lt_50ms,
		  (unsigned long)stats.car_release_suppressed,
		  (unsigned long)stats.car_release_suppressed_lt50,
		  (unsigned long)stats.car_duplicate_release,
		  (unsigned long)stats.forward_contact,
		  (unsigned long)stats.forward_release,
		  (unsigned long)stats.forward_action_mismatch,
		  (unsigned long)stats.iphone_contact,
		  (unsigned long)stats.iphone_release,
		  (unsigned long)stats.iphone_action_mismatch);
	rt_printf("[HIDDESCR][%lu] touch valid/uid/desc_len/xmax/ymax=%u/%lu/%lu/%lu/%lu "
		  "create/add/error=%lu/%lu/%lu report mismatch len/uid=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned)advertised_touch_valid,
		  (unsigned long)advertised_touch_uid,
		  (unsigned long)advertised_descriptor_length,
		  (unsigned long)advertised_x_max,
		  (unsigned long)advertised_y_max,
		  (unsigned long)stats.descriptor_create_calls,
		  (unsigned long)stats.descriptor_add_calls,
		  (unsigned long)stats.descriptor_errors,
		  (unsigned long)stats.report_len_mismatch,
		  (unsigned long)stats.report_uid_mismatch);
#endif
	rt_printf("[HIDREPORT][%lu] len 5/12/other=%lu/%lu/%lu "
		  "contact/release/down/up=%lu/%lu/%lu/%lu active=%u "
		  "moved/same/flip=%lu/%lu/%lu contact_after_up=%lu "
		  "release_age_ms=%ld\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.report_len5,
		  (unsigned long)stats.report_len12,
		  (unsigned long)stats.report_len_other,
		  (unsigned long)stats.report5_contact,
		  (unsigned long)stats.report5_release,
		  (unsigned long)stats.report5_down,
		  (unsigned long)stats.report5_up,
		  (unsigned)report5_touch_active,
		  (unsigned long)stats.report5_moved,
		  (unsigned long)stats.report5_same_xy,
		  (unsigned long)stats.report5_direction_flip,
		  (unsigned long)stats.report_after_up_contact,
		  last_report_release_us != 0U ?
			(long)((now_us - last_report_release_us) / 1000U) : -1L);
	rt_printf("[HIDSAMPLE][%lu] enabled/hz/period_us=%u/%u/%lu "
		  "input/sent/suppressed/down/release/passthrough="
		  "%lu/%lu/%lu/%lu/%lu/%lu gate_waits/edge_flushes=%lu/%lu "
		  "write_interval n/avg/max=%lu/%lu/%lu active=%u "
		  "next_due_in_us=%ld\r\n",
		  (unsigned long)sequence,
		  (unsigned)(CONFIG_TOUCH_MOVE_SAMPLE_HZ > 0),
		  (unsigned)CONFIG_TOUCH_MOVE_SAMPLE_HZ,
#if CONFIG_TOUCH_MOVE_SAMPLE_HZ > 0
		  (unsigned long)TOUCH_MOVE_SAMPLE_PERIOD_US,
#else
		  0UL,
#endif
		  (unsigned long)stats.move_sample_input,
		  (unsigned long)stats.move_sample_sent,
		  (unsigned long)stats.move_sample_suppressed,
		  (unsigned long)stats.move_sample_down,
		  (unsigned long)stats.move_sample_release,
		  (unsigned long)stats.move_sample_passthrough,
		  (unsigned long)stats.move_sample_gate_waits,
		  (unsigned long)stats.move_sample_edge_flushes,
		  (unsigned long)stats.move_sample_write_interval.samples,
		  touch_path_stage_avg(&stats.move_sample_write_interval),
		  (unsigned long)stats.move_sample_write_interval.max_us,
		  (unsigned)move_sample_active,
		  move_sample_active ?
			(long)(int32_t)(move_sample_next_due_us - now_us) :
			0L);
#if CONFIG_TOUCH_PATH_REPORT_DETAIL
	rt_printf("[TOUCHSTATE][%lu] active car/forward/iphone=%u/%u/%u "
		  "age_ms contact/car_release/iphone_release=%ld/%ld/%ld "
		  "last action/x/y=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned)car_contact_active,
		  (unsigned)forward_contact_active,
		  (unsigned)iphone_contact_active,
		  last_car_contact_us != 0U ?
			(long)((now_us - last_car_contact_us) / 1000U) : -1L,
		  last_car_release_us != 0U ?
			(long)((now_us - last_car_release_us) / 1000U) : -1L,
		  last_hid_release_us != 0U ?
			(long)((now_us - last_hid_release_us) / 1000U) : -1L,
		  (unsigned long)last_action,
		  (unsigned long)last_x,
		  (unsigned long)last_y);
	rt_printf("[TOUCHRELEASE][%lu] http enq/write/2xx=%lu/%lu/%lu "
		  "us avg:max contact_to_release=%lu:%lu car_to_forward=%lu:%lu "
		  "car_to_hid=%lu:%lu car_to_http_enq=%lu:%lu "
		  "car_to_http_write=%lu:%lu car_to_200=%lu:%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.release_http_enqueued,
		  (unsigned long)stats.release_http_write_complete,
		  (unsigned long)stats.release_http_response_2xx,
		  touch_path_stage_avg(&stats.contact_to_release),
		  (unsigned long)stats.contact_to_release.max_us,
		  touch_path_stage_avg(&stats.release_car_to_forward),
		  (unsigned long)stats.release_car_to_forward.max_us,
		  touch_path_stage_avg(&stats.release_car_to_hid),
		  (unsigned long)stats.release_car_to_hid.max_us,
		  touch_path_stage_avg(&stats.release_car_to_http_enqueue),
		  (unsigned long)stats.release_car_to_http_enqueue.max_us,
		  touch_path_stage_avg(&stats.release_car_to_http_write),
		  (unsigned long)stats.release_car_to_http_write.max_us,
		  touch_path_stage_avg(&stats.release_car_to_http_response),
		  (unsigned long)stats.release_car_to_http_response.max_us);
	rt_printf("[TOUCHSEQ][%lu] paired car/forward/hid/http_enq/http_write="
		  "%lu/%lu/%lu/%lu/%lu missing forward/hid/http=%lu/%lu/%lu "
		  "write_reorder=%lu last car/forward/hid/enq/write="
		  "%lu/%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.seq_car,
		  (unsigned long)stats.seq_forward,
		  (unsigned long)stats.seq_hid,
		  (unsigned long)stats.seq_http_enqueue,
		  (unsigned long)stats.seq_http_write,
		  (unsigned long)stats.seq_forward_missing,
		  (unsigned long)stats.seq_hid_missing,
		  (unsigned long)stats.seq_http_missing,
		  (unsigned long)stats.seq_write_reorder,
		  (unsigned long)last_car_sequence,
		  (unsigned long)last_forward_sequence,
		  (unsigned long)last_hid_sequence,
		  (unsigned long)last_http_enqueue_sequence,
		  (unsigned long)last_http_write_sequence);
#endif
#if CONFIG_TOUCH_PATH_REPORT_DETAIL
	rt_printf("[HIDFLOW][%lu] safe_parser_counter car_commands/out_hid="
		  "%lu/%lu http_enq/write/2xx=%lu/%lu/%lu "
		  "car_body_bytes=%lu min/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_hid_commands,
		  (unsigned long)stats.iphone_hid_calls,
		  (unsigned long)stats.http_enqueued,
		  (unsigned long)stats.http_write_complete,
		  (unsigned long)stats.http_response_2xx,
		  (unsigned long)stats.car_hid_bytes,
		  (unsigned long)stats.car_hid_min_length,
		  (unsigned long)stats.car_hid_max_length);
	rt_printf("[IPHONEHTTPQ][%lu] depth total/queued/wire/peak/cap="
		  "%lu/%lu/%lu/%lu/%lu oldest_ms=%lu "
		  "flow enq/write/2xx/full/stale/orphan_w/orphan_r="
		  "%lu/%lu/%lu/%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)http_depth,
		  (unsigned long)http_queued,
		  (unsigned long)http_wire_inflight,
		  (unsigned long)stats.http_depth_peak,
		  (unsigned long)TOUCH_HTTP_SLOTS,
		  (unsigned long)(http_oldest_age_us / 1000U),
		  (unsigned long)stats.http_enqueued,
		  (unsigned long)stats.http_write_complete,
		  (unsigned long)stats.http_response_2xx,
		  (unsigned long)stats.http_slot_full,
		  (unsigned long)stats.http_stale_expired,
		  (unsigned long)stats.http_orphan_write,
		  (unsigned long)stats.http_orphan_response);
	rt_printf("[IPHONEHTTPQ][%lu] latency_us enq_to_write avg/max=%lu/%lu "
		  "write_to_200 avg/max=%lu/%lu prev_200_to_write n/avg/max="
		  "%lu/%lu/%lu ordering write_while_pending/wire_peak/response_reorder="
		  "%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  touch_path_stage_avg(&stats.http_enqueue_to_write),
		  (unsigned long)stats.http_enqueue_to_write.max_us,
		  touch_path_stage_avg(&stats.http_write_to_response),
		  (unsigned long)stats.http_write_to_response.max_us,
		  (unsigned long)stats.http_response_to_next_write.samples,
		  touch_path_stage_avg(&stats.http_response_to_next_write),
		  (unsigned long)stats.http_response_to_next_write.max_us,
		  (unsigned long)stats.http_write_while_pending,
		  (unsigned long)stats.http_wire_inflight_peak,
		  (unsigned long)stats.http_response_reorder);
#endif
#if CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH > 0
	rt_printf("[HIDHTTPPIPE][%lu] depth=%u schedule/run=%lu/%lu "
		  "prewrite/fake/partial/error=%lu/%lu/%lu/%lu "
		  "blocked_state/untracked=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned)CONFIG_AIRPLAY_HID_HTTP_PIPELINE_DEPTH,
		  (unsigned long)stats.http_pipe_scheduled,
		  (unsigned long)stats.http_pipe_runs,
		  (unsigned long)stats.http_pipe_prewritten,
		  (unsigned long)stats.http_pipe_fake_complete,
		  (unsigned long)stats.http_pipe_partial,
		  (unsigned long)stats.http_pipe_errors,
		  (unsigned long)stats.http_pipe_state_blocked,
		  (unsigned long)stats.http_pipe_untracked);
#endif
#if CONFIG_AIRPLAY_HID_HTTP_BYPASS
	rt_printf("[HIDHTTPBYPASS][%lu] queue now/peak=%lu/%lu "
		  "enq/write/release/coalesced=%lu/%lu/%lu/%lu partial/error=%lu/%lu "
		  "drain calls/bytes/wouldblock/error/vendor_block=%lu/%lu/%lu/%lu/%lu "
		  "ready plain/socket/no_data/ioctl_err=%lu/%lu/%lu/%lu "
		  "worker/vendor_overlap=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)touch_path_state.bypass_depth,
		  (unsigned long)stats.http_bypass_depth_peak,
		  (unsigned long)stats.http_bypass_enqueued,
		  (unsigned long)stats.http_bypass_write_complete,
		  (unsigned long)stats.http_bypass_released,
		  (unsigned long)stats.http_bypass_coalesced,
		  (unsigned long)stats.http_bypass_write_partial,
		  (unsigned long)stats.http_bypass_write_error,
		  (unsigned long)stats.http_bypass_drain_calls,
		  (unsigned long)stats.http_bypass_drain_bytes,
		  (unsigned long)stats.http_bypass_drain_would_block,
		  (unsigned long)stats.http_bypass_drain_error,
		  (unsigned long)stats.http_bypass_drain_blocked_vendor,
		  (unsigned long)stats.http_bypass_ready_plain,
		  (unsigned long)stats.http_bypass_ready_socket,
		  (unsigned long)stats.http_bypass_no_data,
		  (unsigned long)stats.http_bypass_ioctl_error,
		  (unsigned long)stats.http_bypass_worker_runs,
		  (unsigned long)stats.http_bypass_vendor_overlap);
#endif
#if CONFIG_TOUCH_PATH_REPORT_DETAIL
	rt_printf("[IPHONEHTTPRX][%lu] reader gcd/other=%lu/%lu prio_min/max=%lu/%lu "
		  "lwip_read calls/bytes/post_missing=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.iphone_rx_reader_gcd,
		  (unsigned long)stats.iphone_rx_reader_other,
		  (unsigned long)stats.iphone_rx_reader_prio_min,
		  (unsigned long)stats.iphone_rx_reader_prio_max,
		  (unsigned long)stats.iphone_rx_read_calls,
		  (unsigned long)stats.iphone_rx_read_bytes,
		  (unsigned long)stats.iphone_rx_post_missing);
	rt_printf("[IPHONEHTTPRX][%lu] phase_us write_to_lwip avg/max=%lu/%lu "
		  "lwip_to_read avg/max=%lu/%lu read_call avg/max=%lu/%lu "
		  "lwip_to_200 avg/max=%lu/%lu read_to_200 avg/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  touch_path_stage_avg(&stats.iphone_rx_write_to_post),
		  (unsigned long)stats.iphone_rx_write_to_post.max_us,
		  touch_path_stage_avg(&stats.iphone_rx_post_to_read),
		  (unsigned long)stats.iphone_rx_post_to_read.max_us,
		  touch_path_stage_avg(&stats.iphone_rx_read_call),
		  (unsigned long)stats.iphone_rx_read_call.max_us,
		  touch_path_stage_avg(&stats.iphone_rx_post_to_complete),
		  (unsigned long)stats.iphone_rx_post_to_complete.max_us,
		  touch_path_stage_avg(&stats.iphone_rx_read_to_complete),
		  (unsigned long)stats.iphone_rx_read_to_complete.max_us);
	rt_printf("[CARACK][%lu] requests=%lu write call/complete/defer/error="
		  "%lu/%lu/%lu/%lu request_to_parser_us avg/max=%lu/%lu "
		  "write_us avg/max=%lu/%lu tail >1/3/5/10/16ms="
		  "%lu/%lu/%lu/%lu/%lu socket=%d nodelay=%d valid=%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_type_matches,
		  (unsigned long)stats.car_ack_write_calls,
		  (unsigned long)stats.car_ack_write_complete,
		  (unsigned long)stats.car_ack_write_deferred,
		  (unsigned long)stats.car_ack_write_errors,
		  touch_path_stage_avg(&stats.car_ack_total),
		  (unsigned long)stats.car_ack_total.max_us,
		  touch_path_stage_avg(&stats.car_ack_write),
		  (unsigned long)stats.car_ack_write.max_us,
		  (unsigned long)stats.car_ack_write_tail.gt_1ms,
		  (unsigned long)stats.car_ack_write_tail.gt_3ms,
		  (unsigned long)stats.car_ack_write_tail.gt_5ms,
		  (unsigned long)stats.car_ack_write_tail.gt_10ms,
		  (unsigned long)stats.car_ack_write_tail.gt_16ms,
		  car_socket_valid ? car_socket_fd : -1,
		  car_socket_nodelay_valid ? (int)car_socket_nodelay : -1,
		  (unsigned)car_socket_nodelay_valid);
#endif
#if CONFIG_CAR_ACK_TCP_PROFILE
	rt_printf("[CARACKTCP][%lu] valid=%d marked/acked/already/pending/max/overflow="
		  "%lu/%lu/%lu/%lu/%lu/%lu pcb_nodelay=%d nodelay set/missing="
		  "%lu/%lu local sent_now/buffered=%lu/%lu unsent max/now="
		  "%lu/%luB seg_max=%lu unacked_now=%luB pending_oldest_us=%lu\r\n",
		  (unsigned long)sequence,
		  car_tcp_ack_valid,
		  (unsigned long)car_tcp_ack.marked,
		  (unsigned long)car_tcp_ack.acked,
		  (unsigned long)car_tcp_ack.already_acked,
		  (unsigned long)car_tcp_ack.pending,
		  (unsigned long)car_tcp_ack.pending_max,
		  (unsigned long)car_tcp_ack.queue_overflow,
		  car_tcp_ack_valid ? (int)car_tcp_ack.pcb_nodelay : -1,
		  (unsigned long)car_tcp_ack.nodelay_set,
		  (unsigned long)car_tcp_ack.nodelay_missing,
		  (unsigned long)car_tcp_ack.sent_immediate,
		  (unsigned long)car_tcp_ack.locally_buffered,
		  (unsigned long)car_tcp_ack.unsent_bytes_max,
		  (unsigned long)car_tcp_ack.unsent_bytes,
		  (unsigned long)car_tcp_ack.unsent_segments_max,
		  (unsigned long)car_tcp_ack.unacked_bytes,
		  (unsigned long)car_tcp_ack.pending_oldest_us);
	rt_printf("[CARACKTCP][%lu] peer_ack_us samples/avg/max=%lu/%llu/%lu "
		  "bins <=100us/<=1/<=5/<=20/>20ms=%lu/%lu/%lu/%lu/%lu "
		  "seq una/nxt/lbb=%08lx/%08lx/%08lx\r\n",
		  (unsigned long)sequence,
		  (unsigned long)car_tcp_ack.ack_samples,
		  (unsigned long long)(car_tcp_ack.ack_samples != 0U ?
			car_tcp_ack.ack_us_sum / car_tcp_ack.ack_samples : 0U),
		  (unsigned long)car_tcp_ack.ack_us_max,
		  (unsigned long)car_tcp_ack.ack_le_100us,
		  (unsigned long)car_tcp_ack.ack_le_1ms,
		  (unsigned long)car_tcp_ack.ack_le_5ms,
		  (unsigned long)car_tcp_ack.ack_le_20ms,
		  (unsigned long)car_tcp_ack.ack_gt_20ms,
		  (unsigned long)car_tcp_ack.snd_una,
		  (unsigned long)car_tcp_ack.snd_nxt,
		  (unsigned long)car_tcp_ack.snd_lbb);
	rt_printf("[CARACKRTO][%lu] expired/retransmit/with_pending=%lu/%lu/%lu "
		  "timer rto/rtime/remaining_ms=%lu/%ld/%lu "
		  "nrtx now/max=%u/%u dupacks=%u in_rto=%u\r\n",
		  (unsigned long)sequence,
		  (unsigned long)car_tcp_ack.rto_expired,
		  (unsigned long)car_tcp_ack.rto_retransmit,
		  (unsigned long)car_tcp_ack.rto_with_pending,
		  (unsigned long)car_tcp_ack.rto_ms,
		  (long)car_tcp_ack.rtime_ms,
		  (unsigned long)car_tcp_ack.rto_remaining_ms,
		  (unsigned)car_tcp_ack.nrtx,
		  (unsigned)car_tcp_ack.nrtx_max,
		  (unsigned)car_tcp_ack.dupacks,
		  (unsigned)car_tcp_ack.in_rto);
#endif
	rt_printf("[CAREVENTRX][%lu] interval_us samples/avg/max=%lu/%lu/%lu "
		  "bins <=1/1-5/5-15/15-30/>30ms=%lu/%lu/%lu/%lu/%lu "
		  "read_call_us avg/max=%lu/%lu pending samples/nonzero/avg/max/ioctl_err="
		  "%lu/%lu/%llu/%lu/%lu drain_streak now/max=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_rx_interval.samples,
		  touch_path_stage_avg(&stats.car_rx_interval),
		  (unsigned long)stats.car_rx_interval.max_us,
		  (unsigned long)stats.car_rx_gap_le_1ms,
		  (unsigned long)stats.car_rx_gap_1_5ms,
		  (unsigned long)stats.car_rx_gap_5_15ms,
		  (unsigned long)stats.car_rx_gap_15_30ms,
		  (unsigned long)stats.car_rx_gap_gt_30ms,
		  touch_path_stage_avg(&stats.car_rx_read_call),
		  (unsigned long)stats.car_rx_read_call.max_us,
		  (unsigned long)stats.car_rx_pending_samples,
		  (unsigned long)stats.car_rx_pending_nonzero,
		  (unsigned long long)(stats.car_rx_pending_samples != 0U ?
			stats.car_rx_pending_sum / stats.car_rx_pending_samples : 0U),
		  (unsigned long)stats.car_rx_pending_max,
		  (unsigned long)stats.car_rx_pending_ioctl_errors,
		  (unsigned long)car_rx_drain_streak,
		  (unsigned long)stats.car_rx_drain_streak_max);
#if CONFIG_TOUCH_PATH_REPORT_DETAIL
	rt_printf("[EVENTREARM][%lu] read_complete_to_next_read_us n/avg/max="
		  "%lu/%lu/%lu parser_complete_to_next_read=%lu/%lu/%lu "
		  "ack_complete_to_next_read=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.read_complete_to_rearm.samples,
		  touch_path_stage_avg(&stats.read_complete_to_rearm),
		  (unsigned long)stats.read_complete_to_rearm.max_us,
		  (unsigned long)stats.parser_complete_to_rearm.samples,
		  touch_path_stage_avg(&stats.parser_complete_to_rearm),
		  (unsigned long)stats.parser_complete_to_rearm.max_us,
		  (unsigned long)stats.ack_complete_to_rearm.samples,
		  touch_path_stage_avg(&stats.ack_complete_to_rearm),
		  (unsigned long)stats.ack_complete_to_rearm.max_us);
	rt_printf("[EVENTLAT][%lu] car_read_to hid_call/hid_return/http_enq/http_write/"
		  "iphone_200_us avg:max=%lu:%lu/%lu:%lu/%lu:%lu/%lu:%lu/%lu:%lu "
		  "tail_gt5/10/16ms hid=%lu/%lu/%lu write=%lu/%lu/%lu 200=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  touch_path_stage_avg(&stats.event_to_hid_call),
		  (unsigned long)stats.event_to_hid_call.max_us,
		  touch_path_stage_avg(&stats.event_to_hid_return),
		  (unsigned long)stats.event_to_hid_return.max_us,
		  touch_path_stage_avg(&stats.event_to_http_enqueue),
		  (unsigned long)stats.event_to_http_enqueue.max_us,
		  touch_path_stage_avg(&stats.event_to_http_write),
		  (unsigned long)stats.event_to_http_write.max_us,
		  touch_path_stage_avg(&stats.wire_end_to_end),
		  (unsigned long)stats.wire_end_to_end.max_us,
		  (unsigned long)stats.event_to_hid_tail.gt_5ms,
		  (unsigned long)stats.event_to_hid_tail.gt_10ms,
		  (unsigned long)stats.event_to_hid_tail.gt_16ms,
		  (unsigned long)stats.event_to_http_write_tail.gt_5ms,
		  (unsigned long)stats.event_to_http_write_tail.gt_10ms,
		  (unsigned long)stats.event_to_http_write_tail.gt_16ms,
		  (unsigned long)stats.event_to_http_200_tail.gt_5ms,
		  (unsigned long)stats.event_to_http_200_tail.gt_10ms,
		  (unsigned long)stats.event_to_http_200_tail.gt_16ms);
#endif
	rt_printf("[TOUCHAFTERRELEASE][%lu] active=%u release_seq=%lu "
		  "contact car/forward/hid/http_write=%lu/%lu/%lu/%lu "
		  "quiet_gt100ms hid/http_write=%lu/%lu "
		  "age_ms release/car/hid/http_write=%ld/%ld/%ld/%ld "
		  "coords valid=%u first=%lu,%lu last=%lu,%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned)post_release_active,
		  (unsigned long)release_sequence,
		  (unsigned long)stats.after_release_car_contact,
		  (unsigned long)stats.after_release_forward_contact,
		  (unsigned long)stats.after_release_hid_contact,
		  (unsigned long)stats.after_release_http_write,
		  (unsigned long)stats.quiet_hid,
		  (unsigned long)stats.quiet_http_write,
		  release_start_us != 0U ?
			(long)((now_us - release_start_us) / 1000U) : -1L,
		  last_car_touch_us != 0U ?
			(long)((now_us - last_car_touch_us) / 1000U) : -1L,
		  last_hid_us != 0U ?
			(long)((now_us - last_hid_us) / 1000U) : -1L,
		  last_http_write_us != 0U ?
			(long)((now_us - last_http_write_us) / 1000U) : -1L,
		  (unsigned)stats.after_release_coord_valid,
		  (unsigned long)stats.after_release_first_x,
		  (unsigned long)stats.after_release_first_y,
		  (unsigned long)stats.after_release_last_x,
		  (unsigned long)stats.after_release_last_y);
#if TOUCH_PATH_VERBOSE_REPORT
	rt_printf("[TOUCHBRIDGE][%lu] car_ack type/read_pair/read_miss/overwrite/"
		  "write/complete/defer/error=%lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu "
		  "dispatch calls/missing=%lu/%lu "
		  "http enq/error/full/stale=%lu/%lu/%lu/%lu write call/done/defer/error="
		  "%lu/%lu/%lu/%lu response call/done/2xx/non2xx=%lu/%lu/%lu/%lu "
		  "depth now/peak/oldest_ms=%lu/%lu/%lu orphan write/response=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.car_type_matches,
		  (unsigned long)stats.car_read_pairs,
		  (unsigned long)stats.car_read_misses,
		  (unsigned long)stats.car_type_overwrites,
		  (unsigned long)stats.car_ack_write_calls,
		  (unsigned long)stats.car_ack_write_complete,
		  (unsigned long)stats.car_ack_write_deferred,
		  (unsigned long)stats.car_ack_write_errors,
		  (unsigned long)stats.dispatch_calls,
		  (unsigned long)stats.dispatch_missing,
		  (unsigned long)stats.http_enqueued,
		  (unsigned long)stats.http_enqueue_errors,
		  (unsigned long)stats.http_slot_full,
		  (unsigned long)stats.http_stale_expired,
		  (unsigned long)stats.http_write_calls,
		  (unsigned long)stats.http_write_complete,
		  (unsigned long)stats.http_write_deferred,
		  (unsigned long)stats.http_write_errors,
		  (unsigned long)stats.http_response_calls,
		  (unsigned long)stats.http_response_complete,
		  (unsigned long)stats.http_response_2xx,
		  (unsigned long)stats.http_response_non_2xx,
		  (unsigned long)http_depth,
		  (unsigned long)stats.http_depth_peak,
		  (unsigned long)(http_oldest_age_us / 1000U),
		  (unsigned long)stats.http_orphan_write,
		  (unsigned long)stats.http_orphan_response);
	rt_printf("[TOUCHBRIDGE][%lu] stage_us avg/max parse_to_touch=%lu/%lu "
		  "car_cb=%lu/%lu touch_to_forward=%lu/%lu "
		  "forward_to_hid=%lu/%lu hid_send_touch=%lu/%lu hid_send_all=%lu/%lu "
		  "car_to_http_enq=%lu/%lu parser_total=%lu/%lu "
		  "age_ms car/forward=%ld/%ld last action/x/y=%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  touch_path_stage_avg(&stats.parse_to_touch),
		  (unsigned long)stats.parse_to_touch.max_us,
		  touch_path_stage_avg(&stats.car_touch_callback),
		  (unsigned long)stats.car_touch_callback.max_us,
		  touch_path_stage_avg(&stats.touch_to_forward),
		  (unsigned long)stats.touch_to_forward.max_us,
		  touch_path_stage_avg(&stats.forward_to_hid),
		  (unsigned long)stats.forward_to_hid.max_us,
		  touch_path_stage_avg(&stats.iphone_hid_enqueue),
		  (unsigned long)stats.iphone_hid_enqueue.max_us,
		  touch_path_stage_avg(&stats.iphone_hid_call_all),
		  (unsigned long)stats.iphone_hid_call_all.max_us,
		  touch_path_stage_avg(&stats.bridge_end_to_end),
		  (unsigned long)stats.bridge_end_to_end.max_us,
		  touch_path_stage_avg(&stats.event_parser_total),
		  (unsigned long)stats.event_parser_total.max_us,
		  last_car_touch_us != 0U ?
			(long)((now_us - last_car_touch_us) / 1000U) : -1L,
		  last_forward_us != 0U ?
			(long)((now_us - last_forward_us) / 1000U) : -1L,
		  (unsigned long)last_action,
		  (unsigned long)last_x,
		  (unsigned long)last_y);
	rt_printf("[TOUCHBRIDGE][%lu] transport_us avg/max read_to_type=%lu/%lu "
		  "car_ack=%lu/%lu ack_write=%lu/%lu "
		  "dispatch_wait=%lu/%lu dispatch_exec=%lu/%lu "
		  "dispatch_total=%lu/%lu hid_to_http=%lu/%lu "
		  "enqueue_to_write=%lu/%lu write_total=%lu/%lu "
		  "write_to_200=%lu/%lu enqueue_to_200=%lu/%lu "
		  "car_to_200=%lu/%lu\r\n",
		  (unsigned long)sequence,
		  touch_path_stage_avg(&stats.car_read_to_type),
		  (unsigned long)stats.car_read_to_type.max_us,
		  touch_path_stage_avg(&stats.car_ack_total),
		  (unsigned long)stats.car_ack_total.max_us,
		  touch_path_stage_avg(&stats.car_ack_write),
		  (unsigned long)stats.car_ack_write.max_us,
		  touch_path_stage_avg(&stats.dispatch_wait),
		  (unsigned long)stats.dispatch_wait.max_us,
		  touch_path_stage_avg(&stats.dispatch_exec),
		  (unsigned long)stats.dispatch_exec.max_us,
		  touch_path_stage_avg(&stats.dispatch_total),
		  (unsigned long)stats.dispatch_total.max_us,
		  touch_path_stage_avg(&stats.hid_to_http_enqueue),
		  (unsigned long)stats.hid_to_http_enqueue.max_us,
		  touch_path_stage_avg(&stats.http_enqueue_to_write),
		  (unsigned long)stats.http_enqueue_to_write.max_us,
		  touch_path_stage_avg(&stats.http_write_total),
		  (unsigned long)stats.http_write_total.max_us,
		  touch_path_stage_avg(&stats.http_write_to_response),
		  (unsigned long)stats.http_write_to_response.max_us,
		  touch_path_stage_avg(&stats.http_enqueue_to_response),
		  (unsigned long)stats.http_enqueue_to_response.max_us,
		  touch_path_stage_avg(&stats.wire_end_to_end),
		  (unsigned long)stats.wire_end_to_end.max_us);
	rt_printf("[TOUCHBRIDGE][%lu] tail count/samples "
		  "parser >5/10/16ms=%lu/%lu/%lu/%lu "
		  "hid_send >5/10/16ms=%lu/%lu/%lu/%lu "
		  "ack_write >1/3/5/10ms=%lu/%lu/%lu/%lu/%lu "
		  "dispatch_wait >1/3/5ms=%lu/%lu/%lu/%lu "
		  "dispatch_exec >1/3/5/10ms=%lu/%lu/%lu/%lu/%lu "
		  "dispatch_total >1/3/5/10ms=%lu/%lu/%lu/%lu/%lu\r\n",
		  (unsigned long)sequence,
		  (unsigned long)stats.event_parser_tail.gt_5ms,
		  (unsigned long)stats.event_parser_tail.gt_10ms,
		  (unsigned long)stats.event_parser_tail.gt_16ms,
		  (unsigned long)stats.event_parser_total.samples,
		  (unsigned long)stats.iphone_hid_call_tail.gt_5ms,
		  (unsigned long)stats.iphone_hid_call_tail.gt_10ms,
		  (unsigned long)stats.iphone_hid_call_tail.gt_16ms,
		  (unsigned long)stats.iphone_hid_call_all.samples,
		  (unsigned long)stats.car_ack_write_tail.gt_1ms,
		  (unsigned long)stats.car_ack_write_tail.gt_3ms,
		  (unsigned long)stats.car_ack_write_tail.gt_5ms,
		  (unsigned long)stats.car_ack_write_tail.gt_10ms,
		  (unsigned long)stats.car_ack_write.samples,
		  (unsigned long)stats.dispatch_wait_tail.gt_1ms,
		  (unsigned long)stats.dispatch_wait_tail.gt_3ms,
		  (unsigned long)stats.dispatch_wait_tail.gt_5ms,
		  (unsigned long)stats.dispatch_wait.samples,
		  (unsigned long)stats.dispatch_exec_tail.gt_1ms,
		  (unsigned long)stats.dispatch_exec_tail.gt_3ms,
		  (unsigned long)stats.dispatch_exec_tail.gt_5ms,
		  (unsigned long)stats.dispatch_exec_tail.gt_10ms,
		  (unsigned long)stats.dispatch_exec.samples,
		  (unsigned long)stats.dispatch_total_tail.gt_1ms,
		  (unsigned long)stats.dispatch_total_tail.gt_3ms,
		  (unsigned long)stats.dispatch_total_tail.gt_5ms,
		  (unsigned long)stats.dispatch_total_tail.gt_10ms,
		  (unsigned long)stats.dispatch_total.samples);
#endif
#else
	(void)sequence;
#endif
}

#else

void carbox_touch_path_profiler_report(uint32_t sequence)
{
	(void)sequence;
}

uint32_t carbox_touch_dispatch_sync_begin(void)
{
	return 0U;
}

void carbox_touch_dispatch_sync_callback_begin(uint32_t token)
{
	(void)token;
}

void carbox_touch_dispatch_sync_callback_end(uint32_t token)
{
	(void)token;
}

void carbox_touch_dispatch_sync_complete(uint32_t token,
					 uint32_t submit_us,
					 uint32_t callback_start_us,
					 uint32_t callback_end_us,
					 uint32_t return_us,
					 int callback_called)
{
	(void)token;
	(void)submit_us;
	(void)callback_start_us;
	(void)callback_end_us;
	(void)return_us;
	(void)callback_called;
}

#endif
