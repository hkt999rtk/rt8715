#include "vfs.h"

#include "basic_types.h"
#include "cmsis_compiler.h"
#include "FreeRTOS.h"
#include "stdio_port_func.h"
#include "task.h"

#include <stdlib.h>
#include <string.h>

extern void *pvPortReAlloc(void *ptr, size_t size);

typedef struct {
	vfs_file info;
	struct dirent entry;
} vfs_dir_stream;

static int is_stdio(FILE *stream)
{
	return stream == stdin || stream == stdout || stream == stderr;
}

static vfs_file *get_file(FILE *stream)
{
	vfs_file *info = (vfs_file *)stream;

	if (stream == NULL || is_stdio(stream) ||
	    info->magic != CARBOX_VFS_FILE_MAGIC) {
		return NULL;
	}
	return info;
}

static vfs_dir_stream *get_dir(DIR *dir)
{
	vfs_dir_stream *stream = (vfs_dir_stream *)dir;

	if (dir == NULL || stream->info.magic != CARBOX_VFS_DIR_MAGIC) {
		return NULL;
	}
	return stream;
}

static int mode_requests_write(const char *mode)
{
	return mode == NULL || mode[0] == '\0' || mode[0] != 'r' ||
	       strchr(mode, '+') != NULL || strchr(mode, 'w') != NULL ||
	       strchr(mode, 'a') != NULL || strchr(mode, 'x') != NULL;
}

static int resolve(const char *path, char *translated, int *vfs_id,
		   int *user_id, vfs_opt **driver, const user_config **user)
{
	if (vfs_resolve_path(path, translated, CARBOX_VFS_PATH_MAX,
			     vfs_id, user_id) != 0) {
		return -1;
	}
	*driver = vfs_get_driver(*vfs_id);
	*user = vfs_get_user(*user_id);
	return *driver != NULL && *user != NULL ? 0 : -1;
}

FILE *__wrap_fopen(const char *filename, const char *mode)
{
	vfs_file *info;
	vfs_opt *driver;
	const user_config *user;
	char translated[CARBOX_VFS_PATH_MAX];
	int vfs_id;
	int user_id;

	if (resolve(filename, translated, &vfs_id, &user_id, &driver, &user) != 0 ||
	    driver->open == NULL || (user->flag == VFS_RO && mode_requests_write(mode))) {
		return NULL;
	}
	info = pvPortMalloc(sizeof(*info));
	if (info == NULL) {
		return NULL;
	}
	memset(info, 0, sizeof(*info));
	info->magic = CARBOX_VFS_FILE_MAGIC;
	info->vfs_id = vfs_id;
	info->user_id = user_id;
	info->interface_id = user->vfs_interface_type;
	memcpy(info->name, translated, strlen(translated) + 1);
	if (driver->open(info->name, mode, info) != 0) {
		info->magic = 0;
		vPortFree(info);
		return NULL;
	}
	return (FILE *)info;
}

int __wrap_fclose(FILE *stream)
{
	vfs_file *info;
	vfs_opt *driver;
	int ret;

	if (is_stdio(stream)) {
		return 0;
	}
	info = get_file(stream);
	driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;
	if (driver == NULL || driver->close == NULL) {
		return EOF;
	}
	ret = driver->close(info);
	info->magic = 0;
	vPortFree(info);
	return ret == 0 ? 0 : EOF;
}

size_t __wrap_fread(void *ptr, size_t size, size_t count, FILE *stream)
{
	vfs_file *info = get_file(stream);
	vfs_opt *driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;
	int ret;

	if (ptr == NULL || size == 0 || count == 0 || count > (size_t)-1 / size ||
	    driver == NULL || driver->read == NULL) {
		return 0;
	}
	ret = driver->read(ptr, size * count, info);
	return ret > 0 ? (size_t)ret / size : 0;
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
	vfs_file *info;
	vfs_opt *driver;
	const user_config *user;
	int ret;

	if (ptr == NULL || size == 0 || count == 0 || count > (size_t)-1 / size) {
		return 0;
	}
	if (stream == stdout || stream == stderr) {
		const unsigned char *bytes = ptr;
		size_t len = size * count;
		size_t i;

		taskENTER_CRITICAL();
		for (i = 0; i < len; i++) {
			stdio_port_putc(bytes[i]);
			if (bytes[i] == '\n') {
				stdio_port_putc('\r');
			}
		}
		taskEXIT_CRITICAL();
		return count;
	}
	info = get_file(stream);
	driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;
	user = info != NULL ? vfs_get_user(info->user_id) : NULL;
	if (driver == NULL || user == NULL || user->flag == VFS_RO ||
	    driver->write == NULL) {
		return 0;
	}
	ret = driver->write(ptr, size * count, info);
	return ret > 0 ? (size_t)ret / size : 0;
}

int __wrap_fseek(FILE *stream, long offset, int origin)
{
	vfs_file *info = get_file(stream);
	vfs_opt *driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;

	return driver != NULL && driver->seek != NULL ?
		driver->seek(offset, origin, info) : -1;
}

void __wrap_rewind(FILE *stream)
{
	(void)__wrap_fseek(stream, 0, SEEK_SET);
}

int __wrap_fflush(FILE *stream)
{
	vfs_file *info;
	vfs_opt *driver;

	if (stream == NULL || is_stdio(stream)) {
		return 0;
	}
	info = get_file(stream);
	driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;
	return driver != NULL && driver->sync != NULL && driver->sync(info) == 0 ? 0 : EOF;
}

int __wrap_feof(FILE *stream)
{
	vfs_file *info = get_file(stream);
	vfs_opt *driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;

	return driver != NULL && driver->eof != NULL ? driver->eof(info) : 0;
}

int __wrap_ferror(FILE *stream)
{
	vfs_file *info = get_file(stream);
	vfs_opt *driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;

	return driver != NULL && driver->error != NULL ? driver->error(info) : 1;
}

long __wrap_ftell(FILE *stream)
{
	vfs_file *info = get_file(stream);
	vfs_opt *driver = info != NULL ? vfs_get_driver(info->vfs_id) : NULL;

	return driver != NULL && driver->tell != NULL ? driver->tell(info) : -1;
}

int __wrap_fputc(int character, FILE *stream)
{
	unsigned char byte = (unsigned char)character;

	return __wrap_fwrite(&byte, 1, 1, stream) == 1 ? character : EOF;
}

int __wrap_fputs(const char *str, FILE *stream)
{
	size_t len;

	if (str == NULL) {
		return EOF;
	}
	len = strlen(str);
	return __wrap_fwrite(str, 1, len, stream) == len ? (int)len : EOF;
}

char *__wrap_fgets(char *str, int num, FILE *stream)
{
	int pos = 0;

	if (str == NULL || num <= 1) {
		return NULL;
	}
	while (pos < num - 1 && __wrap_fread(&str[pos], 1, 1, stream) == 1) {
		if (str[pos++] == '\n') {
			break;
		}
	}
	if (pos == 0) {
		return NULL;
	}
	str[pos] = '\0';
	return str;
}

static int resolve_mutable(const char *path, char *translated, int *vfs_id,
			   int *user_id, vfs_opt **driver)
{
	const user_config *user;

	if (resolve(path, translated, vfs_id, user_id, driver, &user) != 0 ||
	    user->flag == VFS_RO) {
		return -1;
	}
	return 0;
}

int __wrap_remove(const char *filename)
{
	char path[CARBOX_VFS_PATH_MAX];
	vfs_opt *driver;
	int vfs_id;
	int user_id;

	return resolve_mutable(filename, path, &vfs_id, &user_id, &driver) == 0 &&
	       driver->remove != NULL ? driver->remove(path) : -1;
}

int __wrap_rename(const char *oldname, const char *newname)
{
	char old_path[CARBOX_VFS_PATH_MAX];
	char new_path[CARBOX_VFS_PATH_MAX];
	vfs_opt *old_driver;
	vfs_opt *new_driver;
	int old_vfs;
	int new_vfs;
	int old_user;
	int new_user;

	if (resolve_mutable(oldname, old_path, &old_vfs, &old_user, &old_driver) != 0 ||
	    resolve_mutable(newname, new_path, &new_vfs, &new_user, &new_driver) != 0 ||
	    old_vfs != new_vfs || old_user != new_user || old_driver != new_driver ||
	    old_driver->rename == NULL) {
		return -1;
	}
	return old_driver->rename(old_path, new_path);
}

DIR *__wrap_opendir(const char *name)
{
	vfs_dir_stream *stream;
	vfs_opt *driver;
	const user_config *user;
	char path[CARBOX_VFS_PATH_MAX];
	int vfs_id;
	int user_id;

	if (resolve(name, path, &vfs_id, &user_id, &driver, &user) != 0 ||
	    driver->opendir == NULL) {
		return NULL;
	}
	stream = pvPortMalloc(sizeof(*stream));
	if (stream == NULL) {
		return NULL;
	}
	memset(stream, 0, sizeof(*stream));
	stream->info.magic = CARBOX_VFS_DIR_MAGIC;
	stream->info.vfs_id = vfs_id;
	stream->info.user_id = user_id;
	stream->info.interface_id = user->vfs_interface_type;
	memcpy(stream->info.name, path, strlen(path) + 1);
	if (driver->opendir(stream->info.name, &stream->info) != 0) {
		stream->info.magic = 0;
		vPortFree(stream);
		return NULL;
	}
	return (DIR *)stream;
}

struct dirent *__wrap_readdir(DIR *dir)
{
	vfs_dir_stream *stream = get_dir(dir);
	vfs_opt *driver = stream != NULL ? vfs_get_driver(stream->info.vfs_id) : NULL;

	if (driver == NULL || driver->readdir == NULL ||
	    driver->readdir(&stream->info, &stream->entry) <= 0) {
		return NULL;
	}
	return &stream->entry;
}

int __wrap_closedir(DIR *dir)
{
	vfs_dir_stream *stream = get_dir(dir);
	vfs_opt *driver = stream != NULL ? vfs_get_driver(stream->info.vfs_id) : NULL;
	int ret;

	if (driver == NULL || driver->closedir == NULL) {
		return -1;
	}
	ret = driver->closedir(&stream->info);
	stream->info.magic = 0;
	vPortFree(stream);
	return ret;
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
	return strcoll((*a)->d_name, (*b)->d_name);
}

int __wrap_scandir(const char *dirpath, struct dirent ***namelist,
		   int (*filter)(const struct dirent *),
		   int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *dir;
	struct dirent *entry;
	struct dirent **list = NULL;
	int count = 0;
	int capacity = 0;
	int failed = 0;

	if (namelist == NULL || (dir = __wrap_opendir(dirpath)) == NULL) {
		return -1;
	}
	*namelist = NULL;
	while ((entry = __wrap_readdir(dir)) != NULL) {
		struct dirent **new_list;

		if (filter != NULL && !filter(entry)) {
			continue;
		}
		if (count == capacity) {
			int new_capacity = capacity == 0 ? 8 : capacity * 2;
			if (new_capacity < capacity ||
			    (size_t)new_capacity > (size_t)-1 / sizeof(*list)) {
				failed = 1;
				break;
			}
			new_list = pvPortReAlloc(list,
						(size_t)new_capacity * sizeof(*list));
			if (new_list == NULL) {
				failed = 1;
				break;
			}
			list = new_list;
			capacity = new_capacity;
		}
		list[count] = pvPortMalloc(sizeof(*list[count]));
		if (list[count] == NULL) {
			failed = 1;
			break;
		}
		memcpy(list[count++], entry, sizeof(*entry));
	}
	if (__wrap_closedir(dir) != 0) {
		failed = 1;
	}
	if (failed) {
		while (count > 0) {
			vPortFree(list[--count]);
		}
		vPortFree(list);
		return -1;
	}
	if (compar != NULL && count > 1) {
		qsort(list, (size_t)count, sizeof(*list),
		      (int (*)(const void *, const void *))compar);
	}
	*namelist = list;
	return count;
}

int __wrap_rmdir(const char *path)
{
	return __wrap_remove(path);
}

int __wrap_mkdir(const char *pathname, mode_t mode)
{
	char path[CARBOX_VFS_PATH_MAX];
	vfs_opt *driver;
	int vfs_id;
	int user_id;

	(void)mode;
	return resolve_mutable(pathname, path, &vfs_id, &user_id, &driver) == 0 &&
	       driver->mkdir != NULL ? driver->mkdir(path) : -1;
}

int __wrap_access(const char *pathname, int mode)
{
	char path[CARBOX_VFS_PATH_MAX];
	vfs_opt *driver;
	const user_config *user;
	struct stat info;
	int vfs_id;
	int user_id;

	if (resolve(pathname, path, &vfs_id, &user_id, &driver, &user) != 0 ||
	    driver->stat == NULL || ((mode & W_OK) != 0 && user->flag == VFS_RO)) {
		return -1;
	}
	return driver->stat(path, &info);
}

int __wrap_stat(const char *pathname, struct stat *buf)
{
	char path[CARBOX_VFS_PATH_MAX];
	vfs_opt *driver;
	const user_config *user;
	int vfs_id;
	int user_id;

	return buf != NULL &&
	       resolve(pathname, path, &vfs_id, &user_id, &driver, &user) == 0 &&
	       driver->stat != NULL ? driver->stat(path, buf) : -1;
}
