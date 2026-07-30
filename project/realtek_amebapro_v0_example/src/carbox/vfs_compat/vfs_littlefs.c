#include "vfs.h"

#include "carbox_flash_layout.h"
#include "carbox_littlefs.h"

#include <string.h>

static int littlefs_make_path(const char *suffix, char *out, size_t out_len)
{
	size_t suffix_len;
	size_t i;

	if (suffix == NULL || out == NULL) {
		return -1;
	}
	suffix_len = strlen(suffix);
	if (suffix_len + 2 > out_len) {
		return -1;
	}
	out[0] = '/';
	for (i = 0; i < suffix_len; i++) {
		out[i + 1] = suffix[i] == '\\' ? '/' : suffix[i];
	}
	out[suffix_len + 1] = '\0';
	return 0;
}

static int littlefs_mount_backend(int interface)
{
	return interface == VFS_INF_FLASH ? carbox_littlefs_mount() : -1;
}

static int littlefs_unmount_backend(int interface)
{
	(void)interface;
	return carbox_littlefs_unmount();
}

static int littlefs_format_backend(int interface)
{
	return interface == VFS_INF_FLASH ? carbox_littlefs_format() : -1;
}

static int littlefs_mode_flags(const char *mode)
{
	int flags;

	if (mode == NULL || mode[0] == '\0') {
		return -1;
	}
	if (strchr(mode, '+') != NULL) {
		flags = CARBOX_LFS_O_RDWR;
	} else if (mode[0] == 'r') {
		flags = CARBOX_LFS_O_RDONLY;
	} else if (mode[0] == 'w' || mode[0] == 'a') {
		flags = CARBOX_LFS_O_WRONLY;
	} else {
		return -1;
	}
	if (mode[0] == 'w') {
		flags |= CARBOX_LFS_O_CREAT | CARBOX_LFS_O_TRUNC;
	} else if (mode[0] == 'a') {
		flags |= CARBOX_LFS_O_CREAT | CARBOX_LFS_O_APPEND;
	}
	if (strchr(mode, 'x') != NULL) {
		flags |= CARBOX_LFS_O_EXCL;
	}
	return flags;
}

static int littlefs_open_backend(const char *path, const char *mode, vfs_file *finfo)
{
	int flags = littlefs_mode_flags(mode);

	if (flags < 0) {
		return -1;
	}
	finfo->file = carbox_littlefs_open(path, flags);
	return finfo->file == NULL ? -1 : 0;
}

static int littlefs_read_backend(void *buf, size_t len, vfs_file *finfo)
{
	int ret = carbox_littlefs_read((carbox_littlefs_file_t *)finfo->file, buf, len);

	if (ret < 0) {
		finfo->error = ret;
	}
	return ret;
}

static int littlefs_write_backend(const void *buf, size_t len, vfs_file *finfo)
{
	int ret = carbox_littlefs_write((carbox_littlefs_file_t *)finfo->file, buf, len);

	if (ret < 0) {
		finfo->error = ret;
	}
	return ret;
}

static int littlefs_close_backend(vfs_file *finfo)
{
	int ret = carbox_littlefs_close((carbox_littlefs_file_t *)finfo->file);
	finfo->file = NULL;
	return ret;
}

static int littlefs_seek_backend(long offset, int origin, vfs_file *finfo)
{
	if ((long)(int32_t)offset != offset) {
		return -1;
	}
	return carbox_littlefs_seek((carbox_littlefs_file_t *)finfo->file,
				     (int32_t)offset, origin) < 0 ? -1 : 0;
}

static long littlefs_tell_backend(vfs_file *finfo)
{
	return carbox_littlefs_tell((carbox_littlefs_file_t *)finfo->file);
}

static int littlefs_sync_backend(vfs_file *finfo)
{
	return carbox_littlefs_sync((carbox_littlefs_file_t *)finfo->file);
}

static int littlefs_eof_backend(vfs_file *finfo)
{
	int pos = carbox_littlefs_tell((carbox_littlefs_file_t *)finfo->file);
	int size = carbox_littlefs_size((carbox_littlefs_file_t *)finfo->file);
	return pos >= 0 && size >= 0 && pos >= size;
}

static int littlefs_error_backend(vfs_file *finfo)
{
	return finfo->error;
}

static int littlefs_remove_backend(const char *path)
{
	return carbox_littlefs_remove(path);
}

static int littlefs_rename_backend(const char *old_path, const char *new_path)
{
	return carbox_littlefs_rename(old_path, new_path);
}

static int littlefs_mkdir_backend(const char *path)
{
	return carbox_littlefs_mkdir(path);
}

static int littlefs_stat_backend(const char *path, struct stat *buf)
{
	uint32_t size;
	int is_dir;

	if (buf == NULL || carbox_littlefs_stat(path, &size, &is_dir) != 0) {
		return -1;
	}
	memset(buf, 0, sizeof(*buf));
	buf->st_mode = (is_dir ? S_IFDIR : S_IFREG) | 0777;
	buf->st_size = size;
	buf->st_blksize = CARBOX_FLASH_SECTOR_SIZE;
	buf->st_blocks = (size + 511U) / 512U;
	return 0;
}

static int littlefs_opendir_backend(const char *path, vfs_file *finfo)
{
	finfo->file = carbox_littlefs_opendir(path);
	return finfo->file == NULL ? -1 : 0;
}

static int littlefs_readdir_backend(vfs_file *finfo, struct dirent *entry)
{
	uint32_t size;
	int is_dir;
	int ret;

	memset(entry, 0, sizeof(*entry));
	ret = carbox_littlefs_readdir((carbox_littlefs_dir_t *)finfo->file,
				       entry->d_name, sizeof(entry->d_name),
				       &is_dir, &size);
	if (ret <= 0) {
		return ret;
	}
	entry->d_namlen = strlen(entry->d_name);
	entry->d_reclen = (unsigned short)sizeof(*entry);
	entry->d_type = is_dir ? DT_DIR : DT_REG;
	return 1;
}

static int littlefs_closedir_backend(vfs_file *finfo)
{
	int ret = carbox_littlefs_closedir((carbox_littlefs_dir_t *)finfo->file);
	finfo->file = NULL;
	return ret;
}

vfs_opt littlefs_drv = {
	.open = littlefs_open_backend,
	.read = littlefs_read_backend,
	.write = littlefs_write_backend,
	.close = littlefs_close_backend,
	.seek = littlefs_seek_backend,
	.tell = littlefs_tell_backend,
	.sync = littlefs_sync_backend,
	.eof = littlefs_eof_backend,
	.error = littlefs_error_backend,
	.remove = littlefs_remove_backend,
	.rename = littlefs_rename_backend,
	.mkdir = littlefs_mkdir_backend,
	.stat = littlefs_stat_backend,
	.opendir = littlefs_opendir_backend,
	.readdir = littlefs_readdir_backend,
	.closedir = littlefs_closedir_backend,
	.mount = littlefs_mount_backend,
	.unmount = littlefs_unmount_backend,
	.format = littlefs_format_backend,
	.make_path = littlefs_make_path,
	.tag = "LittleFS",
};
