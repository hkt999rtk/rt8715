#ifndef CARBOX_VFS_COMPAT_H
#define CARBOX_VFS_COMPAT_H

#include <stddef.h>

#define CARBOX_VFS_COMPAT_PATH_MAX 512

int carbox_vfs_translate_path(const char *path, char *out, size_t out_len);
int carbox_vfs_is_registered(void);
const char *carbox_vfs_get_drive(void);

#endif /* CARBOX_VFS_COMPAT_H */
