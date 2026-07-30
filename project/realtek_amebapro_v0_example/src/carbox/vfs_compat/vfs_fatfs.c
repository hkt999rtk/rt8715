#include "vfs.h"

#include "fatfs_flash_api.h"
#include "FreeRTOS.h"

#include <string.h>

static fatfs_flash_params_t g_fatfs_flash;
static int g_fatfs_mounted;

static int fatfs_make_path(const char *suffix, char *out, size_t out_len)
{
	size_t drive_len;
	size_t suffix_len;
	size_t i;

	if (!g_fatfs_mounted || suffix == NULL || out == NULL) {
		return -1;
	}
	drive_len = strlen(g_fatfs_flash.drv);
	suffix_len = strlen(suffix);
	if (drive_len + suffix_len + 1 > out_len) {
		return -1;
	}
	memcpy(out, g_fatfs_flash.drv, drive_len);
	for (i = 0; i < suffix_len; i++) {
		out[drive_len + i] = suffix[i] == '\\' ? '/' : suffix[i];
	}
	out[drive_len + suffix_len] = '\0';
	return 0;
}

static int fatfs_mount_backend(int interface)
{
	int ret;

	if (interface != VFS_INF_FLASH) {
		return -1;
	}
	if (g_fatfs_mounted) {
		return 0;
	}
	ret = fatfs_flash_init();
	if (ret != 0 || fatfs_flash_get_param(&g_fatfs_flash) != 0) {
		return -1;
	}
	g_fatfs_mounted = 1;
	return 0;
}

static int fatfs_unmount_backend(int interface)
{
	(void)interface;
	if (!g_fatfs_mounted) {
		return 0;
	}
	g_fatfs_mounted = 0;
	memset(&g_fatfs_flash, 0, sizeof(g_fatfs_flash));
	return fatfs_flash_close();
}

static int fatfs_format_backend(int interface)
{
	(void)interface;
	return -1;
}

static int fatfs_mode_is_read_only(const char *mode)
{
	const char *p;

	if (mode == NULL || mode[0] != 'r') {
		return 0;
	}
	for (p = mode; *p != '\0'; p++) {
		if (*p == '+' || *p == 'w' || *p == 'a' || *p == 'x') {
			return 0;
		}
	}
	return 1;
}

static int fatfs_open_backend(const char *filename, const char *mode, vfs_file *finfo)
{
	FIL *file;

	if (!fatfs_mode_is_read_only(mode)) {
		return -1;
	}
	file = pvPortMalloc(sizeof(*file));
	if (file == NULL) {
		return -1;
	}
	memset(file, 0, sizeof(*file));
	if (f_open(file, filename, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
		vPortFree(file);
		return -1;
	}
	finfo->file = file;
	return 0;
}

static int fatfs_read_backend(void *buf, size_t len, vfs_file *finfo)
{
	UINT read_len = 0;
	FRESULT result = f_read((FIL *)finfo->file, buf, (UINT)len, &read_len);

	if (result != FR_OK) {
		finfo->error = result;
		return -1;
	}
	return (int)read_len;
}

static int fatfs_write_backend(const void *buf, size_t len, vfs_file *finfo)
{
	(void)buf;
	(void)len;
	finfo->error = FR_WRITE_PROTECTED;
	return -1;
}

static int fatfs_close_backend(vfs_file *finfo)
{
	FIL *file = (FIL *)finfo->file;
	FRESULT result;

	if (file == NULL) {
		return -1;
	}
	result = f_close(file);
	vPortFree(file);
	finfo->file = NULL;
	return result == FR_OK ? 0 : -1;
}

static int fatfs_seek_backend(long offset, int origin, vfs_file *finfo)
{
	FIL *file = (FIL *)finfo->file;
	long long target;

	if (origin == SEEK_SET) {
		target = offset;
	} else if (origin == SEEK_CUR) {
		target = (long long)f_tell(file) + offset;
	} else if (origin == SEEK_END) {
		target = (long long)f_size(file) + offset;
	} else {
		return -1;
	}
	if (target < 0 || target > (long long)f_size(file)) {
		return -1;
	}
	return f_lseek(file, (FSIZE_t)target) == FR_OK ? 0 : -1;
}

static long fatfs_tell_backend(vfs_file *finfo)
{
	return (long)f_tell((FIL *)finfo->file);
}

static int fatfs_sync_backend(vfs_file *finfo)
{
	(void)finfo;
	return 0;
}

static int fatfs_eof_backend(vfs_file *finfo)
{
	return f_eof((FIL *)finfo->file);
}

static int fatfs_error_backend(vfs_file *finfo)
{
	return finfo->error != 0 ? finfo->error : f_error((FIL *)finfo->file);
}

static int fatfs_read_only_operation(const char *path)
{
	(void)path;
	return -1;
}

static int fatfs_read_only_rename(const char *old_path, const char *new_path)
{
	(void)old_path;
	(void)new_path;
	return -1;
}

static int fatfs_stat_backend(const char *path, struct stat *buf)
{
	FILINFO info;

	if (buf == NULL || f_stat(path, &info) != FR_OK) {
		return -1;
	}
	memset(buf, 0, sizeof(*buf));
	buf->st_mode = (info.fattrib & AM_DIR ? S_IFDIR | 0555 : S_IFREG | 0444);
	buf->st_size = (off_t)info.fsize;
	buf->st_blksize = 512;
	buf->st_blocks = (blkcnt_t)((info.fsize + 511) / 512);
	return 0;
}

static int fatfs_opendir_backend(const char *path, vfs_file *finfo)
{
	DIR *dir = pvPortMalloc(sizeof(*dir));

	if (dir == NULL) {
		return -1;
	}
	memset(dir, 0, sizeof(*dir));
	if (f_opendir(dir, path) != FR_OK) {
		vPortFree(dir);
		return -1;
	}
	finfo->file = dir;
	return 0;
}

static int fatfs_readdir_backend(vfs_file *finfo, struct dirent *entry)
{
	FILINFO info;
	size_t name_len;

	if (entry == NULL || f_readdir((DIR *)finfo->file, &info) != FR_OK) {
		return -1;
	}
	if (info.fname[0] == '\0') {
		return 0;
	}
	name_len = strlen(info.fname);
	if (name_len >= sizeof(entry->d_name)) {
		return -1;
	}
	memset(entry, 0, sizeof(*entry));
	memcpy(entry->d_name, info.fname, name_len + 1);
	entry->d_namlen = name_len;
	entry->d_reclen = (unsigned short)sizeof(*entry);
	entry->d_type = info.fattrib & AM_DIR ? DT_DIR : DT_REG;
	return 1;
}

static int fatfs_closedir_backend(vfs_file *finfo)
{
	DIR *dir = (DIR *)finfo->file;
	FRESULT result;

	if (dir == NULL) {
		return -1;
	}
	result = f_closedir(dir);
	vPortFree(dir);
	finfo->file = NULL;
	return result == FR_OK ? 0 : -1;
}

vfs_opt fatfs_drv = {
	.open = fatfs_open_backend,
	.read = fatfs_read_backend,
	.write = fatfs_write_backend,
	.close = fatfs_close_backend,
	.seek = fatfs_seek_backend,
	.tell = fatfs_tell_backend,
	.sync = fatfs_sync_backend,
	.eof = fatfs_eof_backend,
	.error = fatfs_error_backend,
	.remove = fatfs_read_only_operation,
	.rename = fatfs_read_only_rename,
	.mkdir = fatfs_read_only_operation,
	.stat = fatfs_stat_backend,
	.opendir = fatfs_opendir_backend,
	.readdir = fatfs_readdir_backend,
	.closedir = fatfs_closedir_backend,
	.mount = fatfs_mount_backend,
	.unmount = fatfs_unmount_backend,
	.format = fatfs_format_backend,
	.make_path = fatfs_make_path,
	.tag = "FatFs",
};
