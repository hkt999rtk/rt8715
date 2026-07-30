#include "FreeRTOS.h"
#include "task.h"
#include <platform/platform_stdlib.h>
#include <platform_opts.h>

#if CONFIG_MEDIA_H264_TO_SDCARD
#include "example_media_h264_to_sdcard.h"
#include "video_common_api.h"
#include "h264_encoder.h"
#include "isp_api.h"
#include "h264_api.h"
#include "fatfs_sdcard_api.h"
#include "mmf2_dbg.h"
#include "sensor.h"
#include "sensor_service.h"
#include "avcodec.h"
#include "mp4_muxer.h"
#include "fatfs_ramdisk_api.h"
#include "isp_boot.h"

#define ISP_SW_BUF_NUM	4
#define FATFS_BUF_SIZE	(32*1024)
#define SD_CARD_QUEUE_DEPTH (20)

#define HW_SLOT_NUM 2
#define ISP_FPS 30
#define ISP_STREAM_ID 0

#define SDCARD_RAW_DATA
//#define MP4_SD_STORAGE //It can choose the sd storage or ram storage

extern isp_boot_cfg_t isp_boot_cfg_global;

#if ISP_BOOT_MODE_ENABLE
#define CINIT_DATA_SECTION SECTION(".cinit.data")
CINIT_DATA_SECTION isp_boot_stream_t isp_boot_stream = {
        .width = VIDEO_720P_WIDTH,
        .height = VIDEO_720P_HEIGHT,
        .isp_id = ISP_STREAM_ID,
        .hw_slot_num = HW_SLOT_NUM,
        .fps = ISP_FPS,
        .format = ISP_FORMAT_YUV420_SEMIPLANAR,
        .pin_idx = ISP_PIN_IDX,
        .mode = ISP_FAST_BOOT,
        .interface = ISP_INTERFACE_MIPI,
        .clk = SENSOR_CLK_USE,
        .sensor_fps = SENSOR_FPS, 
        .isp_fw_location = ISP_FW_LOCATION,
        .wake_mode = WAKE_FROM_BOOT,//WAKE_FROM_GPIO//WAKE_FROM_WLAN
        .isp_fw_space = ISP_FW_SPACE,
#if ISP_FW_SPACE == ISP_FW_USERSPACE
        .isp_user_space_addr = SENSOR_FW_USER_ADDR,
        .isp_user_space_size = SENSOR_FW_USER_SIZE,
#else
        .isp_user_space_addr = 0,
        .isp_user_space_size = 0,
#endif
#if SENSOR_USE == SENSOR_ALL
        .isp_multi_sensor = SENSOR_DEFAULT,
        .isp_sensor_auto_sel_flag = SENSOR_AUTO_SEL
#else
        .isp_multi_sensor = 0
#endif
};
#endif

typedef struct {
	VIDEO_BUFFER video_buf;
	uint32_t type;
	uint32_t timestamp;
}MP4_MUXER_BUFFER;

struct h264_to_sd_card_def_setting def_setting = {
	.height = VIDEO_720P_HEIGHT,
	.width = VIDEO_720P_WIDTH,
	.rcMode = H264_RC_MODE_CBR,
	.bitrate = 1*1024*1024,
	.fps = ISP_FPS,
	.gopLen = ISP_FPS,
	.encode_frame_cnt = 310,
	.output_buffer_size = (VIDEO_720P_HEIGHT*VIDEO_720P_WIDTH)/2,
	.isp_stream_id = ISP_STREAM_ID,
	.isp_hw_slot = HW_SLOT_NUM,
	.isp_format = ISP_FORMAT_YUV420_SEMIPLANAR,
	.fatfs_filename = "h264_to_sdcard.h264",
};

typedef struct isp_s{
	isp_stream_t* stream;
	
	isp_cfg_t cfg;
	
	isp_buf_t buf_item[ISP_SW_BUF_NUM];
	xQueueHandle output_ready;
	xQueueHandle output_recycle;
}isp_t;

xQueueHandle sd_card_queue;
u8 fatfs_thread_run;

void stop_fatfs_thread()
{
	fatfs_thread_run = 0;
}

u8 fatfs_thread_running()
{
	if (fatfs_thread_run)
		return 1;
	else 
		return 0;
}

#ifdef SDCARD_RAW_DATA

void card_init_thread(void *param)
{
        int ret = 0;
        ret = fatfs_sd_init();
	if (ret != 0) {
		printf("\n\rfatfs_sd_init err %d\n\r",ret);
		goto exit;
	}
        fatfs_thread_run = 1;
exit:
        vTaskDelete(NULL);
}

void fatfs_thread(void *param)
{
	int ret = 0;
	VIDEO_BUFFER video_buf;
	printf("[FATFS] fatfs_sd_init\n\r");
	// [1][FATFS] fatfs_sd_init
	printf("\n\rfatfs_sd_init\n\r");
	
        if(xTaskCreate(card_init_thread, ((const char*)"card_init_thread"), 1024, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
            printf("\n\r%s xTaskCreate(card_init_thread) failed", __FUNCTION__);
	sd_card_queue = xQueueCreate(SD_CARD_QUEUE_DEPTH, sizeof(VIDEO_BUFFER));
	xQueueReset(sd_card_queue);
	// [2][FATFS] fatfs_sd_create_write_buf
	ret = fatfs_sd_create_write_buf(FATFS_BUF_SIZE);
	if (ret != 0) {
		printf("\n\fatfs_sd_create_write_buf err %d\n\r",ret);
		goto exit;
	}
	
	printf("[FATFS] fatfs_sd_open_file: %s\n\r",def_setting.fatfs_filename);
	
	while(1){
          if(fatfs_thread_run)
            break;
          else
            vTaskDelay(10);
        }
        // [3][FATFS] fatfs_sd_open_file
	fatfs_sd_open_file(def_setting.fatfs_filename);
        while (fatfs_thread_run){
			//xQueueReceive(sd_card_queue, (void *)&mp4_muxer_buf, 0xFFFFFFFF);
                if(xQueueReceive(sd_card_queue, (void *)&video_buf, 10) != pdTRUE) {
                    continue;
                }
		fatfs_sd_write((char*)video_buf.output_buffer,video_buf.output_size);
		if (video_buf.output_buffer != NULL)
			free(video_buf.output_buffer);
	}
	
	fatfs_sd_flush_buf();
	// [13][FATFS] fatfs_sd_close_file
	printf("[FATFS] fatfs_sd_close_file: %s\n\r",def_setting.fatfs_filename);
	fatfs_sd_close_file();
exit:
	vTaskDelete(NULL);
}
#else

void mp4_init_thread(void *param)
{
        struct _mp4_context *mp4_muxer = (struct _mp4_context *)param;
        mp4_muxer_init(mp4_muxer);
        struct fatfs_sd_param_s *fatfs_params = malloc(sizeof(struct fatfs_sd_param_s));
        int ret = 0;
#ifdef MP4_SD_STORAGE
	ret = fatfs_sd_init();
	if (ret != 0) {
		printf("\n\rfatfs_sd_init err %d\n\r",ret);
		goto EXIT;
	}

	fatfs_sd_get_param(fatfs_params);
	set_mp4_fatfs_param(mp4_muxer, fatfs_params);
#else 
        if(fatfs_ram_init()<0){
		printf("\n\rfatfs_ram_init err %d\n\r",ret);
		goto EXIT;
	}
	fatfs_ram_get_param((fatfs_ram_params_t *)fatfs_params);
	set_mp4_fatfs_param(mp4_muxer, (fatfs_sd_params_t *)fatfs_params);
#endif   
	mp4_muxer->width = def_setting.width;
	mp4_muxer->height = def_setting.height;
	mp4_muxer->frame_rate = def_setting.fps;
	mp4_muxer->gop = def_setting.gopLen;
	
	mp4_muxer->period_time = 10;
	mp4_muxer->type = STORAGE_VIDEO;
	mp4_muxer->file_total = 1;
	memset(mp4_muxer->filename,0,sizeof(mp4_muxer->filename));
	strncpy(mp4_muxer->filename,"AmebaPro",sizeof(mp4_muxer->filename));
	mp4_muxer->fatfs_buf_size = 32*1024;
	mp4_start_record(mp4_muxer,1);
        fatfs_thread_run = 1;
EXIT:
        vTaskDelete(NULL);
}
void fatfs_thread(void *param)
{
  	int ret = 0;
	//VIDEO_BUFFER video_buf;
	printf("[FATFS] fatfs_sd_init\n\r");
	// [1][FATFS] fatfs_sd_init
	printf("\n\rfatfs_sd_init\n\r");
        
	
	struct _mp4_context *mp4_muxer = malloc(sizeof(mp4_context));
	memset(mp4_muxer,0,sizeof(mp4_context));
        
        if(xTaskCreate(mp4_init_thread, ((const char*)"card_init_thread"), 1024, (void *)mp4_muxer, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
            printf("\n\r%s xTaskCreate(card_init_thread) failed", __FUNCTION__);
        


	MP4_MUXER_BUFFER mp4_muxer_buf; //= (MP4_MUXER_BUFFER *)param;
	sd_card_queue = xQueueCreate(SD_CARD_QUEUE_DEPTH, sizeof(MP4_MUXER_BUFFER));
	xQueueReset(sd_card_queue);
        while(1){
          if(fatfs_thread_run)
            break;
          else
            vTaskDelay(10);
        }
	//while (fatfs_thread_run || (uxQueueSpacesAvailable(sd_card_queue) != SD_CARD_QUEUE_DEPTH)) {
        while (fatfs_thread_run){
			//xQueueReceive(sd_card_queue, (void *)&mp4_muxer_buf, 0xFFFFFFFF);
                        if(xQueueReceive(sd_card_queue, (void *)&mp4_muxer_buf, 10) != pdTRUE) {
                            continue;
                        }
			mp4_muxer_handle(mp4_muxer, (uint8_t*)mp4_muxer_buf.video_buf.output_buffer, mp4_muxer_buf.video_buf.output_size, AV_CODEC_ID_H264,mp4_muxer_buf.timestamp);
			if (mp4_muxer_buf.video_buf.output_buffer != NULL)
			free(mp4_muxer_buf.video_buf.output_buffer);
	}
        mp4_muxer_close(mp4_muxer);
#ifdef  MP4_SD_STORAGE
        fatfs_sd_close();
#else
        fatfs_ram_close();
#endif
EXIT:
        vTaskDelete(NULL);
}
#endif

void isp_frame_cb(void* p)
{
	BaseType_t xTaskWokenByReceive = pdFALSE;
	BaseType_t xHigherPriorityTaskWoken;
	
	isp_t* ctx = (isp_t*)p;
	isp_info_t* info = &ctx->stream->info;
	isp_cfg_t *cfg = &ctx->cfg;
	isp_buf_t buf;
	//isp_buf_t queue_item;
	
	int is_output_ready = 0;
	
	u32 timestamp = xTaskGetTickCountFromISR();
	
	if(info->isp_overflow_flag == 0){
		is_output_ready = xQueueReceiveFromISR(ctx->output_recycle, &buf, &xTaskWokenByReceive) == pdTRUE;
	}else{
		info->isp_overflow_flag = 0;
		ISP_DBG_ERROR("isp overflow = %d\r\n",cfg->isp_id);
	}
	
	if(is_output_ready){
		isp_handle_buffer(ctx->stream, &buf, MODE_EXCHANGE);
		buf.timestamp = timestamp;
		xQueueSendFromISR(ctx->output_ready, &buf, &xHigherPriorityTaskWoken);	
	}else{
		isp_handle_buffer(ctx->stream, NULL, MODE_SKIP);
	}
	if( xHigherPriorityTaskWoken || xTaskWokenByReceive)
		taskYIELD ();
}

void example_media_h264_to_sdcard_thread(void *param)
{
	int ret;
	#ifdef SDCARD_RAW_DATA
	VIDEO_BUFFER video_buf;
	#else
	MP4_MUXER_BUFFER mp4_muxer_buf;
	#endif
	u8 start_recording = 0;
        isp_init_cfg_t isp_init_cfg;
	isp_t isp_ctx;
	int enc_cnt = 0;
        
        printf("[H264] init video related settings\n\r");
	// [1][H264] init video related settings
	
	memset(&isp_init_cfg, 0, sizeof(isp_init_cfg));
	isp_init_cfg.pin_idx = ISP_PIN_IDX;
	isp_init_cfg.clk = SENSOR_CLK_USE;
	isp_init_cfg.ldc = LDC_STATE;
	isp_init_cfg.fps = SENSOR_FPS;
	isp_init_cfg.isp_fw_location = ISP_FW_LOCATION;
	
	video_subsys_init(&isp_init_cfg);
        
        // [2][ISP] init ISP
	printf("[ISP] init ISP\n\r");
	memset(&isp_ctx,0,sizeof(isp_ctx));
	isp_ctx.output_ready = xQueueCreate(ISP_SW_BUF_NUM, sizeof(isp_buf_t));
	isp_ctx.output_recycle = xQueueCreate(ISP_SW_BUF_NUM, sizeof(isp_buf_t));
	
	isp_ctx.cfg.isp_id = def_setting.isp_stream_id;
	isp_ctx.cfg.format = def_setting.isp_format;
	isp_ctx.cfg.width = def_setting.width;
	isp_ctx.cfg.height = def_setting.height;
	isp_ctx.cfg.fps = def_setting.fps;
	isp_ctx.cfg.hw_slot_num = def_setting.isp_hw_slot;
	
#if ISP_BOOT_MODE_ENABLE
	isp_ctx.cfg.boot_mode = isp_check_boot_status();
#endif
	isp_ctx.stream = isp_stream_create(&isp_ctx.cfg);

        
	isp_stream_set_complete_callback(isp_ctx.stream, isp_frame_cb, (void*)&isp_ctx);
	
	for (int i=0; i<ISP_SW_BUF_NUM; i++ ) {
#if ISP_BOOT_MODE_ENABLE
                if(i<HW_SLOT_NUM){
                      isp_ctx.buf_item[i].slot_id = i;
                      isp_ctx.buf_item[i].y_addr = (uint32_t) isp_boot_cfg_global.isp_buffer[i];
                      isp_ctx.buf_item[i].uv_addr = isp_ctx.buf_item[i].y_addr + def_setting.width*def_setting.height;
                }else{
                      unsigned char *ptr =(unsigned char *) malloc(def_setting.width*def_setting.height*3/2);
                      if (ptr==NULL) {
                              printf("[ISP] Allocate isp buffer[%d] failed\n\r",i);
                              while(1);
                      }
                      isp_ctx.buf_item[i].slot_id = i;
                      isp_ctx.buf_item[i].y_addr = (uint32_t) ptr;
                      isp_ctx.buf_item[i].uv_addr = isp_ctx.buf_item[i].y_addr + def_setting.width*def_setting.height;
                }
#else
		unsigned char *ptr =(unsigned char *) malloc(def_setting.width*def_setting.height*3/2);
		if (ptr==NULL) {
			printf("[ISP] Allocate isp buffer[%d] failed\n\r",i);
			while(1);
		}
		isp_ctx.buf_item[i].slot_id = i;
		isp_ctx.buf_item[i].y_addr = (uint32_t) ptr;
		isp_ctx.buf_item[i].uv_addr = isp_ctx.buf_item[i].y_addr + def_setting.width*def_setting.height;
#endif
		if (i<def_setting.isp_hw_slot) {
			// config hw slot
			//printf("\n\rconfig hw slot[%d] y=%x, uv=%x\n\r",i,isp_ctx.buf_item[i].y_addr,isp_ctx.buf_item[i].uv_addr);
#if !ISP_BOOT_MODE_ENABLE
			isp_handle_buffer(isp_ctx.stream, &isp_ctx.buf_item[i], MODE_SETUP);
#endif
		}
		else {
			// extra sw buffer
			//printf("\n\rextra sw buffer[%d] y=%x, uv=%x\n\r",i,isp_ctx.buf_item[i].y_addr,isp_ctx.buf_item[i].uv_addr);
			if(xQueueSend(isp_ctx.output_recycle, &isp_ctx.buf_item[i], 0)!= pdPASS) {
				printf("[ISP] Queue send fail\n\r");
				while(1);
			}
		}
	}
	
	isp_stream_apply(isp_ctx.stream);
	isp_stream_start(isp_ctx.stream);

        if(xTaskCreate(fatfs_thread, ((const char*)"fatfs_thread"), 1024, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
            printf("\n\r%s xTaskCreate(fatfs_thread) failed", __FUNCTION__);
#if ISP_BOOT_MODE_ENABLE       
        isp_ctx.stream->cfg.boot_mode = ISP_NORMAL_BOOT;
#endif	
        printf("\n\rStart recording %d frames...\n\r",def_setting.encode_frame_cnt);
        
        printf("[H264] create encoder\n\r");
	// [3][H264] create encoder
	struct h264_context* h264_ctx;
	ret = h264_create_encoder(&h264_ctx);
	if (ret != H264_OK) {
		printf("\n\rh264_create_encoder err %d\n\r",ret);
		goto exit;
	}
        
        printf("[H264] get & set encoder parameters\n\r");
	// [6][H264] get & set encoder parameters
	struct h264_parameter h264_parm;
	ret = h264_get_parm(h264_ctx, &h264_parm);
	if (ret != H264_OK) {
		printf("\n\rh264_get_parmeter err %d\n\r",ret);
		goto exit;
	}
	
	h264_parm.height = def_setting.height;
	h264_parm.width = def_setting.width;
	h264_parm.rcMode = def_setting.rcMode;
	h264_parm.bps = def_setting.bitrate;
	h264_parm.ratenum = def_setting.fps;
	h264_parm.gopLen = def_setting.gopLen;
	
	ret = h264_set_parm(h264_ctx, &h264_parm);
	if (ret != H264_OK) {
		printf("\n\rh264_set_parmeter err %d\n\r",ret);
		goto exit;
	}
	
	printf("[H264] init encoder\n\r");
	// [4][H264] init encoder
	ret = h264_init_encoder(h264_ctx);
	if (ret != H264_OK) {
		printf("\n\rh264_init_encoder_buffer err %d\n\r",ret);
		goto exit;
	}

        #if CONFIG_LIGHT_SENSOR
                init_sensor_service();
        #else
                ir_cut_init(NULL);
                ir_cut_enable(1);
        #endif
		
        while (enc_cnt < def_setting.encode_frame_cnt) {
                // [9][ISP] get isp data
                isp_buf_t isp_buf;
                if(xQueueReceive(isp_ctx.output_ready, &isp_buf, 10) != pdTRUE) {
                        continue;
                }
                
                #ifdef SDCARD_RAW_DATA
                // [10][H264] encode data
                video_buf.output_buffer_size = def_setting.output_buffer_size;
                video_buf.output_buffer = malloc(video_buf.output_buffer_size);
                if (video_buf.output_buffer== NULL) {
                        printf("Allocate output buffer fail\n\r");
                        continue;
                }
                ret = h264_encode_frame(h264_ctx, &isp_buf, &video_buf);
                if (ret != H264_OK) {
                        printf("\n\rh264_encode_frame err %d\n\r",ret);
                        if (video_buf.output_buffer != NULL)
                                free(video_buf.output_buffer);
                        continue;
                }
                enc_cnt++;
                
                if (enc_cnt % def_setting.fps == 0) {
                        printf("%d / %d ...\n\r",enc_cnt,def_setting.encode_frame_cnt);
                }
                
                // [11][ISP] put back isp buffer
                xQueueSend(isp_ctx.output_recycle, &isp_buf, 10);
                
                // [12][FATFS] write encoded data into sdcard
                if (start_recording) {
                        xQueueSend(sd_card_queue, (void *)&video_buf, 0xFFFFFFFF);
                }
                else {
                        if (h264_is_i_frame(video_buf.output_buffer)) {
                                start_recording = 1;
                                xQueueSend(sd_card_queue, (void *)&video_buf, 0xFFFFFFFF);
                        }
                        else {
                                if (video_buf.output_buffer != NULL)
                                        free(video_buf.output_buffer);
                        }
                }
                #else
                //video_buf.output_buffer = malloc(video_buf.output_buffer_size);
                mp4_muxer_buf.video_buf.output_buffer_size = def_setting.output_buffer_size;
                mp4_muxer_buf.video_buf.output_buffer = malloc(mp4_muxer_buf.video_buf.output_buffer_size);
                mp4_muxer_buf.timestamp = isp_buf.timestamp;
                if (mp4_muxer_buf.video_buf.output_buffer == NULL) {
                        printf("Allocate output buffer fail\n\r");
                        continue;
                }
                
                ret = h264_encode_frame(h264_ctx, &isp_buf, &mp4_muxer_buf.video_buf);
                if (ret != H264_OK) {
                        printf("\n\rh264_encode_frame err %d\n\r",ret);
                        if (mp4_muxer_buf.video_buf.output_buffer != NULL)
                                free(mp4_muxer_buf.video_buf.output_buffer);
                        continue;
                }
                enc_cnt++;
                
                if (enc_cnt % def_setting.fps == 0) {
                        printf("%d / %d ...\n\r",enc_cnt,def_setting.encode_frame_cnt);
                }
                
                // [11][ISP] put back isp buffer
                xQueueSend(isp_ctx.output_recycle, &isp_buf, 10);
                
                // [12][FATFS] write encoded data into sdcard
                if (start_recording) {
                        xQueueSend(sd_card_queue, (void *)&mp4_muxer_buf, 0xFFFFFFFF);
                }
                else {
                        if (h264_is_i_frame(mp4_muxer_buf.video_buf.output_buffer)) {
                                start_recording = 1;
                                xQueueSend(sd_card_queue, (void *)&mp4_muxer_buf, 0xFFFFFFFF);
                        }
                        else {
                                if (mp4_muxer_buf.video_buf.output_buffer != NULL)
                                        free(mp4_muxer_buf.video_buf.output_buffer);
                        }
                }
                #endif
        }
exit:
	stop_fatfs_thread();
	isp_stream_stop(isp_ctx.stream);
	xQueueReset(isp_ctx.output_ready);
	xQueueReset(isp_ctx.output_recycle);
	for (int i=0; i<ISP_SW_BUF_NUM; i++ ) {
		unsigned char* ptr = (unsigned char*) isp_ctx.buf_item[i].y_addr;
		if (ptr) 
			free(ptr);
	}
	vTaskDelete(NULL);
}

void example_media_h264_to_sdcard(void)
{
	if(xTaskCreate(example_media_h264_to_sdcard_thread, ((const char*)"example_media_h264_to_sdcard_thread"), 1024, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
		printf("\n\r%s xTaskCreate(example_media_h264_to_sdcard_thread) failed", __FUNCTION__);
}
#endif //CONFIG_MEDIA_H264_TO_SDCARD