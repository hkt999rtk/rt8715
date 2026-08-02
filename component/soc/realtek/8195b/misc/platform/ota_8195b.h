#ifndef OTA_8195B_H
#define OTA_8195B_H

#include <FreeRTOS.h>
#include <task.h>
#include <platform_stdlib.h>
#include <flash_api.h>
#include <lwip/sockets.h>

/************************Related setting****************************/
/*
 * Keep the vendor defaults for normal firmware, while allowing the
 * immutable recovery image to compile only the raw TCP OTA path. Recovery
 * has a strict 512 KiB budget and has no USB or filesystem dependency.
 */
#ifndef CONFIG_OTA_HTTP_UPDATE
#define CONFIG_OTA_HTTP_UPDATE 1
#endif

#ifndef CONFIG_OTA_DFU_UPDATE
#define CONFIG_OTA_DFU_UPDATE 1
#endif

#if CONFIG_OTA_HTTP_UPDATE
#define HTTP_OTA_UPDATE
#endif

#if CONFIG_OTA_DFU_UPDATE
#define DFU_OTA_UPDATE
#endif

#if FATFS_DISK_SD
#define SDCARD_OTA_UPDATE
#endif

#define BUF_SIZE		4096
#define HEADER_BAK_LEN	32
/*******************************************************************/


/****************Define the structures used*************************/
typedef struct{
	uint32_t	ip_addr;
	uint16_t	port;
}update_cfg_local_t;

typedef struct {
	uint32_t	status_code;
	uint32_t	header_len;
	uint8_t		*body;
	uint32_t	body_len;
	uint8_t		*header_bak;
	uint32_t	parse_status;
} http_response_result_t;

typedef union { 
	uint32_t u; 
	unsigned char c[4]; 
} _file_checksum;

/*******************************************************************/


/****************General functions used by ota update***************/
extern flash_t flash_ota;
void *update_malloc(unsigned int size);
void update_free(void *buf);
void ota_platform_reset(void);
int update_ota_connect_server(update_cfg_local_t *cfg);
uint32_t update_ota_prepare_addr(void);
int update_ota_erase_upg_region(uint32_t img_len, uint32_t NewFWLen, uint32_t NewFWAddr);
int update_ota_signature(unsigned char* sig_backup, uint32_t NewFWAddr);
/*******************************************************************/


/*******************Functions called by AT CMD**********************/
void cmd_update(int argc, char **argv);
void cmd_ota_image(bool cmd);
/*******************************************************************/


/*************************************************************************************************
** Function Name  : update_ota_local
** Description    : Starting a thread of OTA updating through socket
** Input          : ip:The IP address of OTA server
**					port:The Port of OTA server
** Return         : 0: Task created OK
**					-1: Task created failed
**************************************************************************************************/
int update_ota_local(char *ip, int port);


#ifdef HTTP_OTA_UPDATE
int update_ota_http_connect_server(int server_socket, char *host, int port);

/*************************************************************************************************
** Function Name  : http_update_ota
** Description    : The process of OTA updating through http protocol
** Input          : cfg:struct update_cfg_local_t
** Return         : NULL
**************************************************************************************************/
int http_update_ota(char *host, int port, char *resource);
#endif

#ifdef SDCARD_OTA_UPDATE
/*************************************************************************************************
** Function Name  : sdcard_update_ota
** Description    : The process of OTA updating through SD card
** Input          : filename: target OTA file name. e.g. ota_app.bin
** Return         : 0: OTA success
**************************************************************************************************/
int sdcard_update_ota(char* filename);
#endif

#endif
