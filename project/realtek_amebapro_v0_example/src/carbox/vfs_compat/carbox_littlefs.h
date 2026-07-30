#ifndef CARBOX_LITTLEFS_H
#define CARBOX_LITTLEFS_H

#include <stddef.h>
#include <stdint.h>

typedef struct carbox_littlefs_file carbox_littlefs_file_t;
typedef struct carbox_littlefs_dir carbox_littlefs_dir_t;

enum carbox_littlefs_open_flags {
	CARBOX_LFS_O_RDONLY = 0x01,
	CARBOX_LFS_O_WRONLY = 0x02,
	CARBOX_LFS_O_RDWR = 0x03,
	CARBOX_LFS_O_CREAT = 0x0100,
	CARBOX_LFS_O_EXCL = 0x0200,
	CARBOX_LFS_O_TRUNC = 0x0400,
	CARBOX_LFS_O_APPEND = 0x0800,
};

int carbox_littlefs_mount(void);
int carbox_littlefs_unmount(void);
int carbox_littlefs_format(void);
int carbox_littlefs_is_mounted(void);

carbox_littlefs_file_t *carbox_littlefs_open(const char *path, int flags);
int carbox_littlefs_close(carbox_littlefs_file_t *file);
int carbox_littlefs_read(carbox_littlefs_file_t *file, void *buf, size_t len);
int carbox_littlefs_write(carbox_littlefs_file_t *file, const void *buf, size_t len);
int carbox_littlefs_seek(carbox_littlefs_file_t *file, int32_t offset, int whence);
int carbox_littlefs_tell(carbox_littlefs_file_t *file);
int carbox_littlefs_size(carbox_littlefs_file_t *file);
int carbox_littlefs_sync(carbox_littlefs_file_t *file);

int carbox_littlefs_remove(const char *path);
int carbox_littlefs_rename(const char *old_path, const char *new_path);
int carbox_littlefs_mkdir(const char *path);
int carbox_littlefs_stat(const char *path, uint32_t *size, int *is_dir);

carbox_littlefs_dir_t *carbox_littlefs_opendir(const char *path);
int carbox_littlefs_readdir(carbox_littlefs_dir_t *dir, char *name, size_t name_len,
					 int *is_dir, uint32_t *size);
int carbox_littlefs_closedir(carbox_littlefs_dir_t *dir);

#endif /* CARBOX_LITTLEFS_H */
