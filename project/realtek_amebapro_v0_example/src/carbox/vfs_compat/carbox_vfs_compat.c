#include "vfs.h"
#include "carbox_vfs_compat.h"

#include "osdep_service.h"

#include <stdio.h>
#include <string.h>

vfs_drv vfs;

static _mutex g_vfs_mutex;
static int g_vfs_mutex_ready;
static int g_vfs_inited;

static int vfs_find_user_by_tag(const char *tag, size_t tag_len)
{
	int i;

	for (i = 0; i < MAX_USER_SIZE; i++) {
		if (vfs.user[i].used && strlen(vfs.user[i].tag) == tag_len &&
		    strncmp(vfs.user[i].tag, tag, tag_len) == 0) {
			return i;
		}
	}
	return -1;
}

static int vfs_find_user_by_type(int vfs_type, int interface)
{
	int i;

	for (i = 0; i < MAX_USER_SIZE; i++) {
		if (vfs.user[i].used && vfs.user[i].vfs_type == vfs_type &&
		    vfs.user[i].vfs_interface_type == interface) {
			return i;
		}
	}
	return -1;
}

static int vfs_find_free_user(void)
{
	int i;

	for (i = 0; i < MAX_USER_SIZE; i++) {
		if (!vfs.user[i].used) {
			return i;
		}
	}
	return -1;
}

static int vfs_find_driver(int vfs_type)
{
	unsigned int i;

	for (i = 0; i < vfs.nbr; i++) {
		if (vfs.drv[i] != NULL && vfs.drv[i]->vfs_type == vfs_type) {
			return (int)i;
		}
	}
	return -1;
}

static int vfs_register_driver(int vfs_type)
{
	vfs_opt *driver;

	if (vfs.nbr >= MAX_FS_SIZE) {
		return -1;
	}
	if (vfs_type == VFS_FATFS) {
		driver = &fatfs_drv;
	} else if (vfs_type == VFS_LITTLEFS) {
		driver = &littlefs_drv;
	} else {
		return -1;
	}

	driver->vfs_type = vfs_type;
	driver->mount_count = 0;
	vfs.drv[vfs.nbr] = driver;
	return (int)vfs.nbr++;
}

void vfs_init(void)
{
	if (g_vfs_inited) {
		return;
	}
	memset(&vfs, 0, sizeof(vfs));
	if (!g_vfs_mutex_ready) {
		rtw_mutex_init(&g_vfs_mutex);
		g_vfs_mutex_ready = 1;
	}
	g_vfs_inited = 1;
}

void vfs_deinit(void)
{
	unsigned int i;

	if (!g_vfs_inited) {
		return;
	}
	for (i = 0; i < vfs.nbr; i++) {
		if (vfs.drv[i] != NULL && vfs.drv[i]->mount_count > 0) {
			vfs.drv[i]->unmount(VFS_INF_FLASH);
			vfs.drv[i]->mount_count = 0;
		}
	}
	memset(&vfs, 0, sizeof(vfs));
	g_vfs_inited = 0;
}

int vfs_status(void)
{
	return g_vfs_inited;
}

int vfs_user_register(const char *prefix, int vfs_type, int interface,
			      char region, char flag)
{
	int driver_id;
	int user_id;
	int ret = -1;
	size_t tag_len;
	vfs_opt *driver;

	if (!g_vfs_inited) {
		vfs_init();
	}
	if (prefix == NULL || interface != VFS_INF_FLASH ||
	    (vfs_type != VFS_FATFS && vfs_type != VFS_LITTLEFS) ||
	    (flag != VFS_RO && flag != VFS_RW)) {
		return -1;
	}
	tag_len = strlen(prefix);
	if (tag_len == 0 || tag_len > CARBOX_VFS_TAG_MAX) {
		return -1;
	}

	rtw_mutex_get(&g_vfs_mutex);
	user_id = vfs_find_user_by_tag(prefix, tag_len);
	if (user_id >= 0) {
		ret = 0;
		goto done;
	}
	user_id = vfs_find_free_user();
	if (user_id < 0) {
		goto done;
	}
	driver_id = vfs_find_driver(vfs_type);
	if (driver_id < 0) {
		driver_id = vfs_register_driver(vfs_type);
	}
	if (driver_id < 0) {
		goto done;
	}
	driver = vfs.drv[driver_id];
	if (driver->mount_count == 0 && driver->mount(interface) != 0) {
		goto done;
	}

	memset(&vfs.user[user_id], 0, sizeof(vfs.user[user_id]));
	vfs.user[user_id].used = 1;
	vfs.user[user_id].vfs_type = vfs_type;
	vfs.user[user_id].vfs_type_id = driver_id;
	vfs.user[user_id].vfs_interface_type = interface;
	vfs.user[user_id].region = region;
	vfs.user[user_id].flag = flag;
	memcpy(vfs.user[user_id].tag, prefix, tag_len + 1);
	driver->mount_count++;
	printf("[vfs] register %s:/ -> %s, region=%d, %s\r\n",
	       prefix, driver->tag, region, flag == VFS_RO ? "ro" : "rw");
	ret = 0;

done:
	rtw_mutex_put(&g_vfs_mutex);
	return ret;
}

int vfs_user_unregister(const char *prefix)
{
	int user_id;
	int ret = -1;
	vfs_opt *driver;
	user_config old_user;

	if (!g_vfs_inited || prefix == NULL) {
		return -1;
	}
	rtw_mutex_get(&g_vfs_mutex);
	user_id = vfs_find_user_by_tag(prefix, strlen(prefix));
	if (user_id < 0) {
		goto done;
	}
	old_user = vfs.user[user_id];
	driver = vfs.drv[old_user.vfs_type_id];
	memset(&vfs.user[user_id], 0, sizeof(vfs.user[user_id]));
	if (driver->mount_count > 0) {
		driver->mount_count--;
	}
	ret = driver->mount_count == 0 ?
		driver->unmount(old_user.vfs_interface_type) : 0;

done:
	rtw_mutex_put(&g_vfs_mutex);
	return ret;
}

char *find_vfs_tag(char region)
{
	int i;

	for (i = 0; i < MAX_USER_SIZE; i++) {
		if (vfs.user[i].used && vfs.user[i].region == region) {
			return vfs.user[i].tag;
		}
	}
	return NULL;
}

const user_config *vfs_get_user(int user_id)
{
	if (user_id < 0 || user_id >= MAX_USER_SIZE || !vfs.user[user_id].used) {
		return NULL;
	}
	return &vfs.user[user_id];
}

vfs_opt *vfs_get_driver(int vfs_id)
{
	if (vfs_id < 0 || (unsigned int)vfs_id >= vfs.nbr) {
		return NULL;
	}
	return vfs.drv[vfs_id];
}

int vfs_resolve_path(const char *path, char *translated, size_t translated_len,
			     int *vfs_id, int *user_id)
{
	const char *colon;
	const char *suffix;
	int resolved_user;
	int resolved_vfs;
	size_t tag_len;
	vfs_opt *driver;

	if (!g_vfs_inited || path == NULL || translated == NULL ||
	    translated_len == 0 || vfs_id == NULL || user_id == NULL) {
		return -1;
	}
	colon = strchr(path, ':');
	if (colon == NULL || colon == path) {
		return -1;
	}
	tag_len = (size_t)(colon - path);
	resolved_user = vfs_find_user_by_tag(path, tag_len);
	if (resolved_user < 0 && tag_len == 1 && path[0] == '0') {
		resolved_user = vfs_find_user_by_type(VFS_FATFS, VFS_INF_FLASH);
	}
	if (resolved_user < 0) {
		return -1;
	}

	resolved_vfs = vfs.user[resolved_user].vfs_type_id;
	driver = vfs_get_driver(resolved_vfs);
	if (driver == NULL || driver->make_path == NULL) {
		return -1;
	}
	suffix = colon + 1;
	while (*suffix == '/' || *suffix == '\\') {
		suffix++;
	}
	if (driver->make_path(suffix, translated, translated_len) != 0) {
		return -1;
	}
	*vfs_id = resolved_vfs;
	*user_id = resolved_user;
	return 0;
}

int carbox_vfs_translate_path(const char *path, char *out, size_t out_len)
{
	int vfs_id;
	int user_id;

	return vfs_resolve_path(path, out, out_len, &vfs_id, &user_id) == 0 ? 1 : 0;
}

int carbox_vfs_is_registered(void)
{
	int i;

	for (i = 0; i < MAX_USER_SIZE; i++) {
		if (vfs.user[i].used) {
			return 1;
		}
	}
	return 0;
}

const char *carbox_vfs_get_drive(void)
{
	int user_id = vfs_find_user_by_type(VFS_FATFS, VFS_INF_FLASH);
	static char drive[CARBOX_VFS_PATH_MAX];
	vfs_opt *driver;

	if (user_id < 0) {
		return NULL;
	}
	driver = vfs_get_driver(vfs.user[user_id].vfs_type_id);
	if (driver == NULL || driver->make_path("", drive, sizeof(drive)) != 0) {
		return NULL;
	}
	return drive;
}
