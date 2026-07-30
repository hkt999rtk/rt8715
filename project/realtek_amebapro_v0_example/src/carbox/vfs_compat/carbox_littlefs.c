#include "carbox_littlefs.h"

#include "carbox_flash_layout.h"
#include "device_lock.h"
#include "flash_api.h"
#include "osdep_service.h"

#include "lfs.h"

#include <string.h>

#define CARBOX_LFS_BLOCK_SIZE CARBOX_FLASH_SECTOR_SIZE
#define CARBOX_LFS_BLOCK_COUNT (CARBOX_LITTLEFS_SIZE / CARBOX_LFS_BLOCK_SIZE)
#define CARBOX_LFS_CACHE_SIZE CARBOX_LFS_BLOCK_SIZE
#define CARBOX_LFS_LOOKAHEAD_SIZE 32U

struct carbox_littlefs_file {
	lfs_file_t file;
};

struct carbox_littlefs_dir {
	lfs_dir_t dir;
};

static lfs_t g_lfs;
static _mutex g_lfs_mutex;
static int g_lfs_mutex_ready;
static int g_lfs_mounted;
static flash_t g_lfs_flash;
static int g_lfs_flash_ready;

static int carbox_lfs_read(const struct lfs_config *cfg, lfs_block_t block,
					lfs_off_t off, void *buffer, lfs_size_t size);
static int carbox_lfs_prog(const struct lfs_config *cfg, lfs_block_t block,
					lfs_off_t off, const void *buffer, lfs_size_t size);
static int carbox_lfs_erase(const struct lfs_config *cfg, lfs_block_t block);
static int carbox_lfs_sync(const struct lfs_config *cfg);
static int carbox_lfs_lock(const struct lfs_config *cfg);
static int carbox_lfs_unlock(const struct lfs_config *cfg);

static const struct lfs_config g_lfs_cfg = {
	.read = carbox_lfs_read,
	.prog = carbox_lfs_prog,
	.erase = carbox_lfs_erase,
	.sync = carbox_lfs_sync,
	.lock = carbox_lfs_lock,
	.unlock = carbox_lfs_unlock,
	.read_size = CARBOX_LFS_BLOCK_SIZE,
	.prog_size = CARBOX_LFS_BLOCK_SIZE,
	.block_size = CARBOX_LFS_BLOCK_SIZE,
	.block_count = CARBOX_LFS_BLOCK_COUNT,
	.block_cycles = 100,
	.cache_size = CARBOX_LFS_CACHE_SIZE,
	.lookahead_size = CARBOX_LFS_LOOKAHEAD_SIZE,
};

static void carbox_lfs_flash_init(void)
{
	if (!g_lfs_flash_ready) {
		flash_init(&g_lfs_flash);
		g_lfs_flash_ready = 1;
	}
}

static int carbox_lfs_range_valid(lfs_block_t block, lfs_off_t off, lfs_size_t size)
{
	if (block >= CARBOX_LFS_BLOCK_COUNT || off > CARBOX_LFS_BLOCK_SIZE) {
		return 0;
	}
	return size <= (CARBOX_LFS_BLOCK_SIZE - off);
}

static uint32_t carbox_lfs_address(lfs_block_t block, lfs_off_t off)
{
	return CARBOX_LITTLEFS_BASE + block * CARBOX_LFS_BLOCK_SIZE + off;
}

static int carbox_lfs_read(const struct lfs_config *cfg, lfs_block_t block,
					lfs_off_t off, void *buffer, lfs_size_t size)
{
	int ret;
	(void)cfg;
	if (!carbox_lfs_range_valid(block, off, size)) {
		return LFS_ERR_INVAL;
	}
	carbox_lfs_flash_init();
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_stream_read(&g_lfs_flash, carbox_lfs_address(block, off), size, buffer);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return ret > 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int carbox_lfs_prog(const struct lfs_config *cfg, lfs_block_t block,
					lfs_off_t off, const void *buffer, lfs_size_t size)
{
	int ret;
	(void)cfg;
	if (!carbox_lfs_range_valid(block, off, size)) {
		return LFS_ERR_INVAL;
	}
	carbox_lfs_flash_init();
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	ret = flash_stream_write(&g_lfs_flash, carbox_lfs_address(block, off), size,
					 (uint8_t *)buffer);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return ret > 0 ? LFS_ERR_OK : LFS_ERR_IO;
}

static int carbox_lfs_erase(const struct lfs_config *cfg, lfs_block_t block)
{
	(void)cfg;
	if (block >= CARBOX_LFS_BLOCK_COUNT) {
		return LFS_ERR_INVAL;
	}
	carbox_lfs_flash_init();
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_erase_sector(&g_lfs_flash, carbox_lfs_address(block, 0));
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	return LFS_ERR_OK;
}

static int carbox_lfs_sync(const struct lfs_config *cfg)
{
	(void)cfg;
	return LFS_ERR_OK;
}

static int carbox_lfs_lock(const struct lfs_config *cfg)
{
	(void)cfg;
	if (!g_lfs_mutex_ready) {
		rtw_mutex_init(&g_lfs_mutex);
		g_lfs_mutex_ready = 1;
	}
	rtw_mutex_get(&g_lfs_mutex);
	return LFS_ERR_OK;
}

static int carbox_lfs_unlock(const struct lfs_config *cfg)
{
	(void)cfg;
	rtw_mutex_put(&g_lfs_mutex);
	return LFS_ERR_OK;
}

int carbox_littlefs_mount(void)
{
	int ret;
	if (g_lfs_mounted) {
		return 0;
	}

	ret = lfs_mount(&g_lfs, &g_lfs_cfg);
	if (ret == LFS_ERR_CORRUPT) {
		printf("[littlefs] no valid filesystem, formatting partition\r\n");
		ret = lfs_format(&g_lfs, &g_lfs_cfg);
		if (ret != LFS_ERR_OK) {
			printf("[littlefs] format failed (%d)\r\n", ret);
			return ret;
		}
		ret = lfs_mount(&g_lfs, &g_lfs_cfg);
	} else if (ret != LFS_ERR_OK) {
		printf("[littlefs] mount failed (%d), partition preserved\r\n", ret);
		return ret;
	}
	if (ret == LFS_ERR_OK) {
		g_lfs_mounted = 1;
	}
	return ret;
}

int carbox_littlefs_unmount(void)
{
	int ret;
	if (!g_lfs_mounted) {
		return 0;
	}
	ret = lfs_unmount(&g_lfs);
	if (ret == LFS_ERR_OK) {
		g_lfs_mounted = 0;
	}
	return ret;
}

int carbox_littlefs_format(void)
{
	int ret;
	if (g_lfs_mounted) {
		ret = carbox_littlefs_unmount();
		if (ret != LFS_ERR_OK) {
			return ret;
		}
	}
	ret = lfs_format(&g_lfs, &g_lfs_cfg);
	if (ret != LFS_ERR_OK) {
		return ret;
	}
	return carbox_littlefs_mount();
}

int carbox_littlefs_is_mounted(void)
{
	return g_lfs_mounted;
}

carbox_littlefs_file_t *carbox_littlefs_open(const char *path, int flags)
{
	carbox_littlefs_file_t *file;
	if (!g_lfs_mounted || path == NULL) {
		return NULL;
	}
	file = pvPortMalloc(sizeof(*file));
	if (file == NULL) {
		return NULL;
	}
	memset(file, 0, sizeof(*file));
	if (lfs_file_open(&g_lfs, &file->file, path, flags) != LFS_ERR_OK) {
		vPortFree(file);
		return NULL;
	}
	return file;
}

int carbox_littlefs_close(carbox_littlefs_file_t *file)
{
	int ret;
	if (file == NULL) {
		return LFS_ERR_BADF;
	}
	ret = lfs_file_close(&g_lfs, &file->file);
	vPortFree(file);
	return ret;
}

int carbox_littlefs_read(carbox_littlefs_file_t *file, void *buf, size_t len)
{
	return file == NULL ? LFS_ERR_BADF : lfs_file_read(&g_lfs, &file->file, buf, len);
}

int carbox_littlefs_write(carbox_littlefs_file_t *file, const void *buf, size_t len)
{
	return file == NULL ? LFS_ERR_BADF : lfs_file_write(&g_lfs, &file->file, buf, len);
}

int carbox_littlefs_seek(carbox_littlefs_file_t *file, int32_t offset, int whence)
{
	return file == NULL ? LFS_ERR_BADF : lfs_file_seek(&g_lfs, &file->file, offset, whence);
}

int carbox_littlefs_tell(carbox_littlefs_file_t *file)
{
	return file == NULL ? LFS_ERR_BADF : lfs_file_tell(&g_lfs, &file->file);
}

int carbox_littlefs_size(carbox_littlefs_file_t *file)
{
	return file == NULL ? LFS_ERR_BADF : lfs_file_size(&g_lfs, &file->file);
}

int carbox_littlefs_sync(carbox_littlefs_file_t *file)
{
	return file == NULL ? LFS_ERR_BADF : lfs_file_sync(&g_lfs, &file->file);
}

int carbox_littlefs_remove(const char *path)
{
	return !g_lfs_mounted || path == NULL ? LFS_ERR_BADF : lfs_remove(&g_lfs, path);
}

int carbox_littlefs_rename(const char *old_path, const char *new_path)
{
	return !g_lfs_mounted || old_path == NULL || new_path == NULL ?
		LFS_ERR_BADF : lfs_rename(&g_lfs, old_path, new_path);
}

int carbox_littlefs_mkdir(const char *path)
{
	return !g_lfs_mounted || path == NULL ? LFS_ERR_BADF : lfs_mkdir(&g_lfs, path);
}

int carbox_littlefs_stat(const char *path, uint32_t *size, int *is_dir)
{
	struct lfs_info info;
	int ret;
	if (!g_lfs_mounted || path == NULL) return LFS_ERR_BADF;
	ret = lfs_stat(&g_lfs, path, &info);
	if (ret == LFS_ERR_OK) {
		if (size != NULL) *size = info.size;
		if (is_dir != NULL) *is_dir = info.type == LFS_TYPE_DIR;
	}
	return ret;
}

carbox_littlefs_dir_t *carbox_littlefs_opendir(const char *path)
{
	carbox_littlefs_dir_t *dir;
	if (!g_lfs_mounted || path == NULL) return NULL;
	dir = pvPortMalloc(sizeof(*dir));
	if (dir == NULL) return NULL;
	memset(dir, 0, sizeof(*dir));
	if (lfs_dir_open(&g_lfs, &dir->dir, path) != LFS_ERR_OK) {
		vPortFree(dir);
		return NULL;
	}
	return dir;
}

int carbox_littlefs_readdir(carbox_littlefs_dir_t *dir, char *name, size_t name_len,
					 int *is_dir, uint32_t *size)
{
	struct lfs_info info;
	int ret;
	if (dir == NULL || name == NULL || name_len == 0U) return LFS_ERR_BADF;
	ret = lfs_dir_read(&g_lfs, &dir->dir, &info);
	if (ret <= 0) return ret;
	if (info.name[0] == '\0') return 0;
	if (strlen(info.name) + 1U > name_len) return LFS_ERR_NAMETOOLONG;
	strcpy(name, info.name);
	if (is_dir != NULL) *is_dir = info.type == LFS_TYPE_DIR;
	if (size != NULL) *size = info.size;
	return 1;
}

int carbox_littlefs_closedir(carbox_littlefs_dir_t *dir)
{
	int ret;
	if (dir == NULL) return LFS_ERR_BADF;
	ret = lfs_dir_close(&g_lfs, &dir->dir);
	vPortFree(dir);
	return ret;
}
