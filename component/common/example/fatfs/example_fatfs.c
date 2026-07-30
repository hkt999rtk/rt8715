#include <cmsis.h>
#include "FreeRTOS.h"
#include "task.h"
#include <platform/platform_stdlib.h>
#include "basic_types.h"
#include "platform_opts.h"
#include "section_config.h"

#if CONFIG_EXAMPLE_FATFS

#if CONFIG_FATFS_EN
#include "ff.h"
#include <fatfs_ext/inc/ff_driver.h>

/************************ Config for Ameba-Pro ************************/
#if defined(CONFIG_PLATFORM_8195BHP)
#define STACK_SIZE		4096

#define USE_FATFS_API		1	// process file I/O with FatFs API
#define USE_C_STD_FILE		0  	// process file I/O with <stdio.h>
#if USE_C_STD_FILE
	#include <stdio.h>
	#include "fatfs_wrap.h"
#endif

#if FATFS_DISK_SD
	#include "sdio_combine.h"
	#include "sdio_host.h"
	#include <disk_if/inc/sdcard.h>
	#include "fatfs_sdcard_api.h"
#endif

#if FATFS_DISK_FLASH
#if (_MAX_SS != 4096)
	#error set _MAX_SS to 4096 in ffconf.h to define maximum supported range of sector size for flash memory. See the description below the MACRO for details.
#endif
#if (_USE_MKFS != 1)
	#error define _USE_MKFS MACRO to 1 in ffconf.h for on-board flash memory to enable f_mkfs() which creates FATFS volume on Flash.
#endif
#include "flash_api.h"
#include <disk_if/inc/flash_fatfs.h>
#include "fatfs_flash_api.h"
#endif

#if FATFS_DISK_SD && FATFS_DISK_FLASH
#if (_VOLUMES != 2)
	#error set _VOLUMES to 2 in ffconf.h to support dual file system.
#endif
#endif
#endif // defined(CONFIG_PLATFORM_8195BHP)

/************************ Config for Ameba-D ************************/
#if defined(CONFIG_PLATFORM_8721D)
#if FATFS_DISK_SD
#include <disk_if/inc/sdcard.h>
#elif FATFS_DISK_USB
#include "usb.h"
#include <disk_if/inc/usbdisk.h>
_Sema       us_sto_rdy_sema;
#endif
#define STACK_SIZE		2048
#define TEST_SIZE		(512)

u8 WRBuf[TEST_SIZE];
u8 RDBuf[TEST_SIZE];
#endif // defined(CONFIG_PLATFORM_8721D)

void example_fatfs_SD(void);
void example_fatfs_FLASH(void);
#if USE_FATFS_API
FRESULT list_files_fatfs(char *);  						// list all file and directory in the path
FRESULT del_dir_fatfs(const TCHAR *path, int del_self);  	// delete all file and directory in the path
#elif USE_C_STD_FILE
FRESULT list_files_std(char *);
FRESULT del_dir_std(const TCHAR *path, int del_self);
#endif
FIL     m_file;

/* For Ameba-Pro */
#if defined(CONFIG_PLATFORM_8195BHP)
#define TEST_BUF_SIZE	(512)

#if FATFS_DISK_SD
fatfs_sd_params_t fatfs_sd;
#endif
#if FATFS_DISK_FLASH
fatfs_flash_params_t fatfs_flash;
#endif



void example_fatfs_thread(void* param){
  
	FRESULT res;
	
//// Initialization for SD card and Flash file system ////
#if FATFS_DISK_SD
	res = (FRESULT)fatfs_sd_init();
	if(res < 0){
		printf("fatfs_sd_init fail (%d)\n", res);
	}
#endif	
#if FATFS_DISK_FLASH
	res = fatfs_flash_init();
	if(res < 0){
		printf("fatfs_flash_init fail (%d)\n", res);
	}
#endif	

//// Start read/write example ////
#if FATFS_DISK_SD
	example_fatfs_SD();
#endif	
#if FATFS_DISK_FLASH
	example_fatfs_FLASH();
#endif	

//// Close file system ////
#if FATFS_DISK_SD
    fatfs_sd_close();
#endif	
#if FATFS_DISK_FLASH
    fatfs_flash_close();
#endif	
           
	vTaskDelete(NULL);
}
#endif


/* For Ameba-D */
#if defined(CONFIG_PLATFORM_8721D)
void example_fatfs_thread(void* param){
	
	int	a = 0;
	int drv_num = 0;
	int Fatfs_ok = 0;

	FRESULT res; 
	FATFS	m_fs;
	FIL 	m_file;

	char	logical_drv[4]; /* root diretor */
	char	path[64];
	char	filename[64] = "TEST.TXT";
	int		br,bw;
	int		test_result = 1;
	int		ret = 0;
	int		flash = 0;
	
	u8 test_info[]="\"Ameba test fatfs sd card ~~~~\"";

#if FATFS_DISK_USB
	_usb_init();
	ret = wait_usb_ready();
	if(ret != USB_INIT_OK){
		if(ret == USB_NOT_ATTACHED)
			printf("\r\n NO USB device attached\n");
		else
			printf("\r\n USB init fail\n");
		goto exit;
	}
	usb_hcd_post_init();
	rtw_init_sema(&us_sto_rdy_sema, 0);
#elif FATFS_DISK_SD
	ConfigDebug[LEVEL_ERROR] |= BIT(MODULE_SDIO);
	ConfigDebug[LEVEL_WARN] |= BIT(MODULE_SDIO);
#endif
	//1 register disk driver to fatfs
	printf("Register disk driver to Fatfs.\n");
#if FATFS_DISK_USB
	drv_num = FATFS_RegisterDiskDriver(&USB_disk_Driver);
#elif FATFS_DISK_SD
	drv_num = FATFS_RegisterDiskDriver(&SD_disk_Driver);
#endif

	if(drv_num < 0){
		printf("Rigester disk driver to FATFS fail.\n");
	}else{
		Fatfs_ok = 1;

		logical_drv[0] = drv_num + '0';
		logical_drv[1] = ':';
		logical_drv[2] = '/';
		logical_drv[3] = 0;
	}
	//1 Fatfs write and read test 
	if(Fatfs_ok){

		printf("FatFS Write/Read test begin......\n\n");
		
		res = f_mount(&m_fs, logical_drv, 1);
		if(res) {
			printf("FATFS mount logical drive fail.\n");
			goto fail;
		}

		// write and read test
		strcpy(path, logical_drv);

		sprintf(&path[strlen(path)],"%s",filename);

		//Open source file
		res = f_open(&m_file, path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);
		if(res){
			printf("open file (%s) fail.\n", filename);
			goto fail;
		}

		printf("Test file name:%s\n\n",filename);

		// clean write and read buffer
		memset(&WRBuf[0], 0x00, TEST_SIZE);
		memset(&RDBuf[0], 0x00, TEST_SIZE);

		strcpy(&WRBuf[0], &test_info[0]);
		
		do{
			res = f_write(&m_file, WRBuf, strlen(WRBuf), (u32*)&bw);
			if(res){
				f_lseek(&m_file, 0); 
				printf("Write error.\n");
			}
			printf("Write %d bytes.\n", bw);
		} while (bw < strlen(WRBuf));

		printf("Write content:\n%s\n", WRBuf);
		printf("\n");
		
		/* move the file pointer to the file head*/
		res = f_lseek(&m_file, 0); 

		do{
			res = f_read(&m_file, RDBuf, strlen(WRBuf), (u32*)&br);
			if(res){
				f_lseek(&m_file, 0);
				printf("Read error.\n");
			}
			printf("Read %d bytes.\n", br);
		}while(br < strlen(WRBuf));
	
		printf("Read content:\n%s\n", RDBuf);

		// close source file
		res = f_close(&m_file);
		if(res){
			printf("close file (%s) fail.\n", filename);
		}

		//
		if(f_mount(NULL, logical_drv, 1) != FR_OK){
			printf("FATFS unmount logical drive fail.\n");
		}
		
		if(FATFS_UnRegisterDiskDriver(drv_num)) 
			printf("Unregister disk driver from FATFS fail.\n");
	}

fail:
#if FATFS_DISK_USB	
	// deinit usb driver
#elif FATFS_DISK_SD
	SD_DeInit();
#endif

exit:
	vTaskDelete(NULL);
}
#endif


#if FATFS_DISK_SD
void example_fatfs_SD(void)
{
	char WRBuf[TEST_BUF_SIZE];
        char RDBuf[TEST_BUF_SIZE];

	char test_info[] = "\"Ameba test dual fatfs sd card ~~~~\"";
    
	FRESULT res;


	char path[64], path_dir[64];
	char sd_fn[64] = "sd_file.txt";   // specifies the file name to open or create
	char sd_dir[64] = "sd_dir";   // specifies the directory name to create.
	int br,bw;

	printf("\n\r=== FATFS Example (SD card) ===\n\r");
	fatfs_sd_get_param(&fatfs_sd); // get SD card parameter

	//// delete all file and directory in SD card ////
	printf("\n\r=== Clear files ===\n\n\r");
#if USE_FATFS_API
	del_dir_fatfs(fatfs_sd.drv, 0);	 
#elif USE_C_STD_FILE
	del_dir_std(fatfs_sd.drv, 0);
#endif

	printf("\n\r=== SD card FS Read/Write test ===\n\r");  
	memset(path, 0, sizeof(path)); 
	snprintf(path, sizeof(path), "%s%s", fatfs_sd.drv, sd_fn);	// set file path (path = 0:/sd_file.txt)

	//// Open file (SD card) ////
#if USE_FATFS_API
	res = f_open(&m_file, path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);  // if open successfully, f_open will returns 0
	if(res){
		printf("open file (%s) fail.\n", sd_fn);
		return;
	}
#elif USE_C_STD_FILE
	m_std_file = fopen(path, "a+");
	if(!m_std_file){
		printf("open file (%s) fail.\n", sd_fn);
		return;
	}
#endif
	printf("Test file name: %s\n\n\r",sd_fn);
	memset(&WRBuf[0], 0x00, TEST_BUF_SIZE);
	memset(&RDBuf[0], 0x00, TEST_BUF_SIZE);
	strcpy(&WRBuf[0], &test_info[0]);
	
	//// Write to file (SD card) ////
#if USE_FATFS_API
	do{
		res = f_write(&m_file, WRBuf, strlen(WRBuf), (u32*)&bw);  // write the string into file
		if(res){
			f_lseek(&m_file, 0); 
			printf("Write error.\n");
		}
		printf("Write %d bytes.\n\r", bw);
	} while (bw < strlen(WRBuf));
	f_lseek(&m_file, 0); // move the file pointer to the file head
#elif USE_C_STD_FILE
	do{
		bw = fwrite(WRBuf, 1, strlen(WRBuf), m_std_file);
		if(bw < 0){
			fseek(m_std_file, 0, SEEK_SET); 
			printf("Write error.\n");
		}
		printf("Write %d bytes.\n\r", bw);
	} while (bw < strlen(WRBuf));
	//fprintf(m_std_file, "%s", WRBuf);  // this is used to write in file with specific format
	res = fseek(m_std_file, 0, SEEK_SET); // move the file pointer to the file head
#endif
	printf("Write content:\n%s\n\n\r", WRBuf);

	//// Read from file (SD card) ////
#if USE_FATFS_API
	do{
		res = f_read(&m_file, RDBuf, strlen(WRBuf), (u32*)&br);  // read the string from the file
		if(res){
			f_lseek(&m_file, 0);
			printf("Read error.\n");
		}
		printf("Read %d bytes.\n\r", br);
	}while(br < strlen(WRBuf));
#elif USE_C_STD_FILE
	br = fread(RDBuf, 1, TEST_BUF_SIZE, m_std_file);
	if(br < 0){
		fseek(m_std_file, 0, SEEK_SET); 
		printf("Read error.\n");
	}
	printf("Read %d bytes.\n\r", br);
	/*while(fscanf(m_std_file, "%[^\n]", RDBuf)!=EOF){    // this is used to read from file with specific format
		printf("Read content (by fscanf):\n%s\n\r", RDBuf );  
	} */ 
#endif
	printf("Read content:\n%s\n\r", RDBuf);

	//// Close file (SD card) ////
#if USE_FATFS_API
	res = f_close(&m_file);
	if(res){
		printf("close file (%s) fail.\n", sd_fn);
	}
#elif USE_C_STD_FILE
	res = fclose(m_std_file);
	if(res){
		printf("close file (%s) fail.\n", sd_fn);
	}
#endif

	//// Create directory (SD card) ////
	memset(path_dir, 0, sizeof(path_dir)); 
	snprintf(path_dir, sizeof(path_dir), "%s%s", fatfs_sd.drv, sd_dir);  // set directory path (path_dir = 0:/sd_dir)
	printf("Create directory: %s \n\r", path_dir);
#if USE_FATFS_API
	f_mkdir(path_dir);	
#elif USE_C_STD_FILE	
	res = mkdir(path_dir, 0);
#endif

	//// List all files (SD card) ////
	printf("\n\r=== List files ===\n\r");	
	printf("List files in SD card: %s\n\r", fatfs_sd.drv);
#if USE_FATFS_API
	res = list_files_fatfs(fatfs_sd.drv);
#elif USE_C_STD_FILE
	res = list_files_std(fatfs_sd.drv);
#endif
	if(res){
		printf("list all files in SD card fail (%d)\n\r", res);
	}
	printf("\n\r");

	


	return;
}
#endif	


#if FATFS_DISK_FLASH
void example_fatfs_FLASH(void)
{
	u8 WRBuf[TEST_BUF_SIZE];
    u8 RDBuf[TEST_BUF_SIZE];

	u8 test_info2[] = "\"Ameba test dual fatfs flash ~~~~\"";
    
	FRESULT res;
	FILE  *m_std_file;

	char path[64], path_dir[64];
	char flash_fn[64] = "flash_file.txt";  // specifies the file name to open or create
	char flash_dir[64] = "flash_dir";   // specifies the directory name to create.
	int br,bw;
	
	printf("\n\r=== FATFS Example (Flash Disk) ===\n\r");
	fatfs_flash_get_param(&fatfs_flash); // get Flash disk parameter
	
	//// delete all file and directory in Flash Disk ////
	printf("\n\r=== Clear files ===\n\n\r");
#if USE_FATFS_API
	del_dir_fatfs(fatfs_flash.drv, 0);	 
#elif USE_C_STD_FILE
	del_dir_std(fatfs_flash.drv, 0);
#endif

	printf("\n\r=== Flash FS Read/Write test ===\n\r");
	memset(path, 0, sizeof(path)); 
	snprintf(path, sizeof(path), "%s%s", fatfs_flash.drv, flash_fn);	// set file path (path = 0:/flash_file.txt)
	
	//// Open file (Flash Disk) ////
#if USE_FATFS_API
	res = f_open(&m_file, path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);
	if(res){
		printf("open file (%s) fail.\n", flash_fn);
		return;
	}
#elif USE_C_STD_FILE
	m_std_file = fopen(path, "a+");
	if(!m_std_file){
		printf("open file (%s) fail.\n", flash_fn);
		return;
	}
#endif
	printf("Test file name: %s\n\n\r",flash_fn);
	memset(&WRBuf[0], 0x00, TEST_BUF_SIZE);
	memset(&RDBuf[0], 0x00, TEST_BUF_SIZE);	
	strcpy(&WRBuf[0], &test_info2[0]);

	//// Write to file (Flash Disk) ////
#if USE_FATFS_API
	do{
		res = f_write(&m_file, WRBuf, strlen(WRBuf), (u32*)&bw);
		if(res){
			f_lseek(&m_file, 0); 
			printf("Write error.\n");
		}
		printf("Write %d bytes.\n\r", bw);
	} while (bw < strlen(WRBuf));
	res = f_lseek(&m_file, 0); // move the file pointer to the file head
#elif USE_C_STD_FILE
	do{
		bw = fwrite(WRBuf, 1, strlen(WRBuf), m_std_file);
		if(bw < 0){
			fseek(m_std_file, 0, SEEK_SET); 
			printf("Write error.\n");
		}
		printf("Write %d bytes.\n\r", bw);
	} while (bw < strlen(WRBuf));
	//fprintf(m_std_file, "%s", WRBuf);  // this is used to write in file with specific format
	res = fseek(m_std_file, 0, SEEK_SET); // move the file pointer to the file head
#endif
	printf("Write content:\n%s\n\n\r", WRBuf);

	//// Read from file (Flash Disk) ////
#if USE_FATFS_API
	do{
		res = f_read(&m_file, RDBuf, strlen(WRBuf), (u32*)&br);
		if(res){
			f_lseek(&m_file, 0);
			printf("Read error.\n");
		}
		printf("Read %d bytes.\n\r", br);
	}while(br < strlen(WRBuf));
#elif USE_C_STD_FILE
	br = fread(RDBuf, 1, TEST_BUF_SIZE, m_std_file);
	if(br < 0){
		fseek(m_std_file, 0, SEEK_SET); 
		printf("Read error.\n");
	}
	printf("Read %d bytes.\n\r", br);
	/*while(fscanf(m_std_file, "%[^\n]", RDBuf)!=EOF){    // this is used to read from file with specific format
		printf("Read content (by fscanf):\n%s\n\r", RDBuf );  
	} */ 
#endif
	printf("Read content:\n%s\n\r", RDBuf);
	
	
	//// Close file (Flash Disk) ////
#if USE_FATFS_API
	res = f_close(&m_file);
	if(res){
		printf("close file (%s) fail.\n", flash_fn);
	}
#elif USE_C_STD_FILE
	res = fclose(m_std_file);
	if(res){
		printf("close file (%s) fail.\n", flash_fn);
	}
#endif
	
	//// Create directory (Flash Disk) ////
	memset(path_dir, 0, sizeof(path_dir)); 
	snprintf(path_dir, sizeof(path_dir), "%s%s", fatfs_flash.drv, flash_dir);  // set directory path (path_dir = 0:/flash_dir)
	printf("Create directory: %s \n\r", path_dir);
#if USE_FATFS_API
	f_mkdir(path_dir);
#elif USE_C_STD_FILE
	res = mkdir(path_dir, 0);
#endif
	
	//// List all files (SD card) ////
	printf("\n\r=== List files ===\n\r");
	printf("List files in flash: %s\n\r", fatfs_flash.drv);
#if USE_FATFS_API
	res = list_files_fatfs(fatfs_flash.drv);
#elif USE_C_STD_FILE
	res = list_files_std(fatfs_flash.drv);
#endif
	if(res){
		printf("list all files in flash fail (%d)\n\r", res);
	}
	printf("\n\r");


fail:
	
	return;
}
#endif	

#if USE_FATFS_API
void print_file_info_fatfs(FILINFO fileinfo, char *fn, char* path)
{
	char info[256];
	char fname[64];
	memset(fname, 0, sizeof(fname));
	snprintf(fname, sizeof(fname), "%s", fn);	
	
	snprintf(info, sizeof(info), 
		"%c%c%c%c%c  %u/%02u/%02u  %02u:%02u  %9llu  %30s  %30s", 
		(fileinfo.fattrib & AM_DIR) ? 'D' : '-',
		(fileinfo.fattrib & AM_RDO) ? 'R' : '-',
		(fileinfo.fattrib & AM_HID) ? 'H' : '-',
		(fileinfo.fattrib & AM_SYS) ? 'S' : '-',
		(fileinfo.fattrib & AM_ARC) ? 'A' : '-',
		(fileinfo.fdate >> 9) + 1980,
		(fileinfo.fdate >> 5) & 15,
		fileinfo.fdate & 31,
		(fileinfo.ftime >> 11),
		(fileinfo.ftime >> 5) & 63,
		fileinfo.fsize,
		fn,
		path);
	printf("%s\n\r", info);

}

FRESULT list_files_fatfs(char* list_path)
{
	DIR m_dir;
	FILINFO m_fileinfo;
	FRESULT res;
	char *filename;
#if _USE_LFN
	char fname_lfn[_MAX_LFN + 1];
	m_fileinfo.lfname = fname_lfn;
	m_fileinfo.lfsize = sizeof(fname_lfn);
#endif
	char cur_path[64];

	// open directory
	res = f_opendir(&m_dir, list_path);

	if(res == FR_OK)
	{
		for (;;) {
			strcpy(cur_path, list_path);
			// read directory and store it in file info object
			res = f_readdir(&m_dir, &m_fileinfo);
			if (res != FR_OK || m_fileinfo.fname[0] == 0) {
				break;
			}
#if _USE_LFN
			filename = *m_fileinfo.lfname ? m_fileinfo.lfname : m_fileinfo.fname;
#else
			filename = m_fileinfo.fname;
#endif
			if (*filename == '.' || (filename[0] == '.' && filename[1] == '.')){
				continue;
			}

			// check if the object is directory
			if(m_fileinfo.fattrib & AM_DIR){
				sprintf(&cur_path[strlen(list_path)], "/%s", filename);
				print_file_info_fatfs(m_fileinfo, filename, cur_path);
				res = list_files_fatfs(cur_path);
				//strcpy(list_path, cur_path);
				if (res != FR_OK) {
					break;
				}
				//list_path[strlen(list_path)] = 0;
			}
			else {
				print_file_info_fatfs(m_fileinfo, filename, cur_path);
			}
		}
	}

	// close directory
	res = f_closedir(&m_dir);
	if(res){
		printf("close directory fail: %d\n", res);
	}
	return res;
}

FRESULT del_dir_fatfs(const TCHAR *path, int del_self)  
{  
    FRESULT res;  
    DIR   m_dir;    
    FILINFO m_fileinfo;     
	char *filename;
    char file[_MAX_LFN + 1];  
#if _USE_LFN
	char fname_lfn[_MAX_LFN + 1];
	m_fileinfo.lfname = fname_lfn;
	m_fileinfo.lfsize = sizeof(fname_lfn);
#endif

    res = f_opendir(&m_dir, path);  
	if(res == FR_OK) {
		for (;;) {
			// read directory and store it in file info object
			res = f_readdir(&m_dir, &m_fileinfo);
			if (res != FR_OK || m_fileinfo.fname[0] == 0) {
				break;
			}
#if _USE_LFN
			filename = *m_fileinfo.lfname ? m_fileinfo.lfname : m_fileinfo.fname;
#else
			filename = m_fileinfo.fname;
#endif
			if (*filename == '.' || (filename[0] == '.' && filename[1] == '.')) {
				continue;
			}

			printf("del: %s\n\r", filename);
			sprintf((char*)file, "%s/%s", path, filename);  

			if (m_fileinfo.fattrib & AM_DIR) {  
            	res = del_dir_fatfs(file, 1);  
        	}  
        	else { 
            	res = f_unlink(file);  
        	}  	
		}
	}
	
    // close directory
	res = f_closedir(&m_dir);

	// delete self? 
    if(res == FR_OK) {
		if(del_self == 1)
			res = f_unlink(path);  
    }

	return res;  
}  
#endif	

#if USE_C_STD_FILE
char *print_file_info_std(struct dirent *entry, char* path)
{
	char info[512];

	snprintf(info, sizeof(info), 
		"%c  %9u  %30s  %30s", 
		(entry->d_type == DT_DIR) ? 'D' : 'F',
		entry->d_reclen,
		entry->d_name,
		path);
	printf("%s\n\r", info);
	return info;
}

FRESULT list_files_std(char* list_path)
{
	FRESULT res;    
    DIR *m_dir;    
    char *filename;
    char path[1024];
    char file[512];  
    struct dirent *entry;

    m_dir = opendir(list_path);
	sprintf(path, "%s", list_path); 
	
	if(m_dir) {
		for (;;) {
			// read directory
			entry = readdir(m_dir);
			if(!entry) {
			  break;
			}
			
			filename = entry->d_name;
			if (*filename == '.' || *filename == '..') {
				continue;
			}
			if (entry->d_type == DT_DIR) {  
				sprintf((char*)file, "%s/%s", path, filename); 
				print_file_info_std(entry, path);
				res = list_files_std(file);
				if (res != FR_OK) {
					break;
				}
            }
			else { 
				print_file_info_std(entry, path);
			}  	
		}
	}
	// close directory
	res = closedir(m_dir);
    
	return res;  
}

FRESULT del_dir_std(const char *list_path, int del_self)  
{  
    FRESULT res;    
    DIR *m_dir;
	
    char *filename;
    char path[1024];
    char file[1024];  
    struct dirent *entry;
      
	m_dir = opendir(list_path);
	sprintf(path, "%s", list_path); 

	if(m_dir) {
		for (;;) {
			// read directory and store it in file info object
			entry = readdir(m_dir);
			if(!entry){
				break;
			}
			
			filename = entry->d_name;
			if (*filename == '.' || *filename == '..') {
				continue;
			}

			sprintf((char*)file, "%s/%s", path, filename);  
			if (entry->d_type == DT_DIR) {  
				printf("del dir: %s\n\r", file);
				del_dir_std(file, 1);  
			}  
			else { 
				printf("del file: %s\n\r", file);
				remove(file);  
			}  	
		}
	}
	
    // close directory
	res = closedir(m_dir);

	// delete self? 
    if(res == FR_OK) {
		if(del_self == 1)
			res = remove(path);  
    }
	return res;  
}  
#endif	


void example_fatfs(void)
{
	if(xTaskCreate(example_fatfs_thread, ((const char*)"example_fatfs_thread"), STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
		printf("\n\r%s xTaskCreate(example_fatfs_thread) failed", __FUNCTION__);
}
#endif
#endif /* CONFIG_EXAMPLE_FATFS */
