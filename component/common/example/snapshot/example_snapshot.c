#include "platform_opts.h"
 
#if CONFIG_EXAMPLE_SNAPSHOT
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "jpeg_snapshot.h"
#include "isp_api.h"
#include <stdio.h>

SemaphoreHandle_t g_snapshot_sema = NULL;
#include "section_config.h"
unsigned char *snapshot_raw_data;
unsigned int snapshot_width;
unsigned int snapshot_height;
int snapshot_format;
int snapshot_bayer_type;
VIDEO_BUFFER jpeg_out_buffer;
SDRAM_DATA_SECTION unsigned char jpeg_buf[1920*1080/2];
int jpeg_size = 1920*1080/2;


#include "ff.h"
static FIL     m_file1;
int sd_init;
int timeout_cnt = 0;
void save_file(char *filename, void* buf, int size)
{
	//memset(filename,0,64);
	//sprintf(filename, "%s/%s_%s%s", path, "dump_pcm", "aec(sim_sdk)",".pcm");
	if(sd_init == 0)
	{
		fatfs_sd_init();
		sd_init = 1;
	}
	printf("[snapshot_info] open save file: %s.\r\n", filename);
	printf("[snapshot_info] buffer addr: %d.\r\n", (int)buf);
	if(f_open(&m_file1, filename, FA_OPEN_ALWAYS | FA_READ | FA_WRITE)==FR_OK)
	{
		if(size>0)
		{
			f_write(&m_file1, (void*)buf, size, NULL);
			printf("[snapshot_info] save file size: %d.\r\n", size);
		}
		f_close(&m_file1);
	}
}


static void example_snapshot_thread(void *param)
{
	printf("%s \r\n", __FUNCTION__);

	g_snapshot_sema = xSemaphoreCreateBinary();
	if(g_snapshot_sema == NULL) {
		printf("%s: g_snapshot_sema create fail \r\n", __FUNCTION__);
		goto exit;
	}

	while(1) {
		if(xSemaphoreTake(g_snapshot_sema, portMAX_DELAY) != pdTRUE)
			break;

		printf("w:%d  h:%d format:%d  bayer:%d.\r\n", snapshot_width, snapshot_height, snapshot_format, snapshot_bayer_type);

		if(snapshot_format == 0)
		{
			jpeg_snapshot_initial(snapshot_width, snapshot_height, 10, 1, (u32)jpeg_buf, jpeg_size);
			if(jpeg_snapshot_isp_config(0) < 0)
				printf("%s: jpeg_snapshot_isp_config fail \r\n", __FUNCTION__);
			else {
				if(jpeg_snapshot_isp()<0)
					printf("%s: get image from camera fail \r\n", __FUNCTION__);
				//if(!jpeg_snapshot_get_buffer(&jpeg_out_buffer, 100) < 0)
				//	printf("%s: get image from camera fail \r\n", __FUNCTION__);
				else {
					//printf("Middle point: %d.\r\n", snapshot_raw_data[snapshot_width*snapshot_height/2+snapshot_width/2]);
					jpeg_snapshot_get_buffer(&jpeg_out_buffer, 100);
					if(jpeg_out_buffer.output_buffer)
						save_file("snapshot.jpg", jpeg_out_buffer.output_buffer, jpeg_out_buffer.output_size);
					printf("buf:%d, size:%d, out_size:%d.\r\n", jpeg_out_buffer.output_buffer, jpeg_out_buffer.output_buffer_size, jpeg_out_buffer.output_size);
				}
			}
			if(jpeg_snapshot_isp_deinit() < 0)
				printf("%s: jpeg_snapshot_isp_deinit fail \r\n", __FUNCTION__);
		}

		if(snapshot_format == ISP_FORMAT_YUV420_SEMIPLANAR)
		{
			if(yuv_snapshot_isp_config(snapshot_width, snapshot_height, 1, 0) < 0)
				printf("%s: yuv_snapshot_isp_config fail \r\n", __FUNCTION__);
			else {
				if(yuv_snapshot_isp(&snapshot_raw_data) < 0)
					printf("%s: get image from camera fail \r\n", __FUNCTION__);
				else {
					save_file("snapshot_ycc420.raw", snapshot_raw_data, snapshot_width*snapshot_height*3/2);
					printf("Middle point: %d.\r\n", snapshot_raw_data[snapshot_width*snapshot_height/2+snapshot_width/2]);
				}
			}
			if(yuv_snapshot_isp_deinit() < 0)
				printf("%s: yuv_snapshot_isp_deinit fail \r\n", __FUNCTION__);
		}

		if(snapshot_format == ISP_FORMAT_YUV422_SEMIPLANAR)
		{
			if(yuv422_snapshot_isp_config(snapshot_width, snapshot_height, 1, 0) < 0)
				printf("%s: yuv422_snapshot_isp_config fail \r\n", __FUNCTION__);
			else {
				if(yuv422_snapshot_isp(&snapshot_raw_data) < 0)
					printf("%s: get image from camera fail \r\n", __FUNCTION__);
				else {
					save_file("snapshot_ycc422.raw", snapshot_raw_data, snapshot_width*snapshot_height*2);
					printf("Middle point: %d.\r\n", snapshot_raw_data[snapshot_width*snapshot_height/2+snapshot_width/2]);
				}
			}
			if(yuv422_snapshot_isp_deinit() < 0)
				printf("%s: yuv422_snapshot_isp_deinit fail \r\n", __FUNCTION__);
		}

		if(snapshot_format == ISP_FORMAT_BAYER_PATTERN)
		{
			if(bayer_snapshot_isp_config(snapshot_width, snapshot_height, 1, 0, snapshot_bayer_type) < 0)
				printf("%s: bayer_snapshot_isp_config fail \r\n", __FUNCTION__);
			else {
				if(bayer_snapshot_isp(&snapshot_raw_data) < 0)
					printf("%s: get image from camera fail \r\n", __FUNCTION__);
				else {
					save_file("snapshot_bayer.raw", snapshot_raw_data, snapshot_width*snapshot_height*2);
					printf("Middle point: %d.\r\n", snapshot_raw_data[snapshot_width*snapshot_height/2+snapshot_width/2]);
				}
			}
			if(bayer_snapshot_isp_deinit() < 0)
				printf("%s: bayer_snapshot_isp_deinit fail \r\n", __FUNCTION__);
		}
		snapshot_raw_data = NULL;
		printf("[snapshot_info] snapshot done.\r\n");
	}

exit:
	if(g_snapshot_sema) {
		vSemaphoreDelete(g_snapshot_sema);
		g_snapshot_sema = NULL;
	}


	vTaskDelete(NULL);
}

void example_snapshot(void)
{
	if(xTaskCreate(example_snapshot_thread, ((const char *)"example_snapshot_thread"), 1024, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
		printf("\n\r%s xTaskCreate(example_snapshot_thread) failed", __FUNCTION__);
}

#endif /* CONFIG_EXAMPLE_SNAPSHOT */

