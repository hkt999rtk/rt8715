#ifndef CARBOX_VFS_COMPAT_VFS_H
#define CARBOX_VFS_COMPAT_VFS_H

#include <stddef.h>
#include <stdio.h>

#include "fatfs_wrap.h"

#define MAX_FS_SIZE              2
#define MAX_USER_SIZE            2
#define CARBOX_VFS_TAG_MAX        15
#define CARBOX_VFS_PATH_MAX       512

#define VFS_FATFS                 0x00
#define VFS_LITTLEFS              0x01

#define VFS_INF_SD                0x00
#define VFS_INF_RAM               0x01
#define VFS_INF_FLASH             0x02

#define VFS_RW                    0
#define VFS_RO                    1

#define VFS_REGION_1              0x01
#define VFS_REGION_2              0x02

#define VFS_FAT_PREFIX            "fat"
#define VFS_PREFIX                "vfs"

#define CARBOX_VFS_FILE_MAGIC     0x56465346UL
#define CARBOX_VFS_DIR_MAGIC      0x56465344UL

typedef struct _vfs_file {
	unsigned long magic;
	int vfs_id;
	int user_id;
	int interface_id;
	int error;
	void *file;
	char name[CARBOX_VFS_PATH_MAX];
} vfs_file;

typedef struct _vfs_opt {
	int (*open)(const char *filename, const char *mode, vfs_file *finfo);
	int (*read)(void *buf, size_t len, vfs_file *finfo);
	int (*write)(const void *buf, size_t len, vfs_file *finfo);
	int (*close)(vfs_file *finfo);
	int (*seek)(long offset, int origin, vfs_file *finfo);
	long (*tell)(vfs_file *finfo);
	int (*sync)(vfs_file *finfo);
	int (*eof)(vfs_file *finfo);
	int (*error)(vfs_file *finfo);
	int (*remove)(const char *path);
	int (*rename)(const char *old_path, const char *new_path);
	int (*mkdir)(const char *path);
	int (*stat)(const char *path, struct stat *buf);
	int (*opendir)(const char *path, vfs_file *finfo);
	int (*readdir)(vfs_file *finfo, struct dirent *entry);
	int (*closedir)(vfs_file *finfo);
	int (*mount)(int interface);
	int (*unmount)(int interface);
	int (*format)(int interface);
	int (*make_path)(const char *suffix, char *out, size_t out_len);
	const char *tag;
	int vfs_type;
	int mount_count;
} vfs_opt;

typedef struct {
	int used;
	int vfs_type;
	int vfs_type_id;
	int vfs_interface_type;
	char region;
	char flag;
	char tag[CARBOX_VFS_TAG_MAX + 1];
} user_config;

typedef struct {
	vfs_opt *drv[MAX_FS_SIZE];
	unsigned int nbr;
	user_config user[MAX_USER_SIZE];
} vfs_drv;

extern vfs_drv vfs;
extern vfs_opt fatfs_drv;
extern vfs_opt littlefs_drv;

void vfs_init(void);
void vfs_deinit(void);
int vfs_status(void);
int vfs_user_register(const char *prefix, int vfs_type, int interface,
			      char region, char flag);
int vfs_user_unregister(const char *prefix);
char *find_vfs_tag(char region);

int vfs_resolve_path(const char *path, char *translated, size_t translated_len,
			     int *vfs_id, int *user_id);
const user_config *vfs_get_user(int user_id);
vfs_opt *vfs_get_driver(int vfs_id);

#endif /* CARBOX_VFS_COMPAT_VFS_H */
