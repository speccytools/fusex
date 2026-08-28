#include "config.h"
#include "compat.h"  /* Include before system headers to get compat/getopt.h */
#include "xfs.h"
#include "xfs_worker.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#ifdef WIN32
#include <io.h>
#include <fcntl.h>  /* For O_* constants */
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#ifndef O_BINARY
#define O_BINARY 0  /* Fallback if not defined (Unix doesn't need it) */
#endif
#else
#include <unistd.h>
#include <fcntl.h>
#define O_BINARY 0  /* Not needed on Unix */
#endif
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

#include "libspectrum.h"
#include "ui/ui.h"

// Convert POSIX errno to XFS error code (use negative errno)
static int16_t xfs_error_from_errno(int err)
{
    return (err != 0) ? -err : 0;
}

// Helper function to build a full path
// Returns pointer to static buffer (not thread-safe, but we're single-threaded)
static char* build_path(const char* path)
{
    static char full_path[PATH_MAX];
    // Ensure path starts with / (like RP2350 version)
    if (path[0] != '/')
    {
        // Prepend / if missing
        if (xfs_base_path[0] != '\0')
        {
            snprintf(full_path, sizeof(full_path), "%s/%s", xfs_base_path, path);
        }
        else
        {
            full_path[0] = '/';
            strncpy(full_path + 1, path, sizeof(full_path) - 2);
            full_path[sizeof(full_path) - 1] = '\0';
        }
    }
    else
    {
        if (xfs_base_path[0] != '\0')
        {
            snprintf(full_path, sizeof(full_path), "%s%s", xfs_base_path, path);
        }
        else
        {
            strncpy(full_path, path, sizeof(full_path) - 1);
            full_path[sizeof(full_path) - 1] = '\0';
        }
    }
    return full_path;
}

// Mount data structure
struct xfs_fs_mount_data_t
{
    char base_path[PATH_MAX];
};

// File handle structure
struct xfs_fs_file_handle_t
{
    int fd;
};

// Directory handle structure
struct xfs_fs_dir_handle_t
{
    DIR *dir;
    char path[PATH_MAX];
    /* XFS exposes a logical cursor, whereas POSIX telldir cookies are opaque
     * and can exceed 16 bits. Keep an XFS-visible logical entry index instead. */
    uint32_t position;
};

static inline struct xfs_fs_mount_data_t* get_mount_data(const struct xfs_engine_mount_t* mount)
{
    return (struct xfs_fs_mount_data_t*)mount->mount_data;
}

static inline struct xfs_fs_file_handle_t* get_file_handle(const struct xfs_handle_t* handle)
{
    return (struct xfs_fs_file_handle_t*)handle->data;
}

static inline struct xfs_fs_dir_handle_t* get_dir_handle(const struct xfs_handle_t* handle)
{
    return (struct xfs_fs_dir_handle_t*)handle->data;
}

// Mount function
static int16_t fs_mount(const struct xfs_engine_t* engine, const char* hostname, const char* path, struct xfs_engine_mount_t* out_mount)
{
    XFS_DEBUG("fs: mount hostname='%s' path='%s'\n", hostname ? hostname : "(null)", path ? path : "(null)");
    
    struct xfs_fs_mount_data_t* mount_data = libspectrum_malloc(sizeof(struct xfs_fs_mount_data_t));
    if (!mount_data) {
        return XFS_ERR_NOMEM;
    }
    
    // Store base path
    if (xfs_base_path[0] != '\0') {
        strncpy(mount_data->base_path, xfs_base_path, sizeof(mount_data->base_path) - 1);
        mount_data->base_path[sizeof(mount_data->base_path) - 1] = '\0';
    } else {
        mount_data->base_path[0] = '\0';
    }
    
    out_mount->mount_data = mount_data;
    
    XFS_DEBUG("fs: mount success base_path='%s'\n", mount_data->base_path);
    return XFS_ERR_OK;
}

static uint8_t fs_is_mounted(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* mount)
{
    return mount->mount_data != NULL;
}

static void fs_unmount(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* mount)
{
    if (mount->mount_data) {
        libspectrum_free(mount->mount_data);
        mount->mount_data = NULL;
    }
}

// Open file
static int16_t fs_open(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, const char* path, int flags)
{
    char* full_path = build_path(path);
    
    // Convert XFS flags to POSIX flags
    int open_flags = 0;
    
    // Determine access mode (check lower 2 bits)
    uint8_t accmode = flags & 0x0003;
    
    // If O_CREAT is set, force read-write access (you can't create a file read-only)
    if (flags & XFS_O_CREAT) {
        accmode = XFS_O_RDWR;
    }
    
    XFS_DEBUG("fs: open xfs_flags=0x%x accmode=%d (XFS_O_RDONLY=%d XFS_O_WRONLY=%d XFS_O_RDWR=%d)\n", 
              flags, accmode, XFS_O_RDONLY, XFS_O_WRONLY, XFS_O_RDWR);
    
    if (accmode == XFS_O_RDWR)
        open_flags = O_RDWR;
    else if (accmode == XFS_O_WRONLY)
        open_flags = O_WRONLY;
    else
        open_flags = O_RDONLY;
    
    // Add other flags
    if (flags & XFS_O_APPEND)
        open_flags |= O_APPEND;
    if (flags & XFS_O_CREAT)
        open_flags |= O_CREAT;
    if (flags & XFS_O_TRUNC)
        open_flags |= O_TRUNC;
    
#ifdef WIN32
    // On Windows, open files in binary mode to prevent \n -> \r\n conversion
    open_flags |= O_BINARY;
#endif
    
    XFS_DEBUG("fs: open open_flags=0x%x (O_RDONLY=0x%x O_WRONLY=0x%x O_RDWR=0x%x O_CREAT=0x%x O_TRUNC=0x%x)\n",
              open_flags, O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC);
    
    struct xfs_fs_file_handle_t* file_handle = libspectrum_malloc(sizeof(struct xfs_fs_file_handle_t));
    if (!file_handle) {
        return XFS_ERR_NOMEM;
    }
    
    file_handle->fd = open(full_path, open_flags, 0644);
    
    if (file_handle->fd < 0) {
        XFS_DEBUG("fs: open failed: %s\n", strerror(errno));
        libspectrum_free(file_handle);
        return xfs_error_from_errno(errno);
    }
    
    handle->type = XFS_HANDLE_TYPE_FILE;
    handle->data = file_handle;
    XFS_DEBUG("fs: open success fd=%d\n", file_handle->fd);
    return XFS_ERR_OK;
}

// Read from file
static int32_t fs_read(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, void* buffer, uint32_t size)
{
    struct xfs_fs_file_handle_t* file_handle = get_file_handle(handle);
    if (!file_handle) {
        return XFS_ERR_BADF;
    }
    
    ssize_t bytes_read = read(file_handle->fd, buffer, size);
    
    if (bytes_read < 0) {
        XFS_DEBUG("fs: read failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: read success bytes=%zd\n", bytes_read);
    return (int32_t)bytes_read;
}

// Write to file
static int32_t fs_write(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, const void* buffer, uint32_t size)
{
    if (!handle) {
        XFS_DEBUG("fs: write failed: handle is NULL\n");
        return XFS_ERR_BADF;
    }
    
    XFS_DEBUG("fs: write handle->type=%d handle->data=%p\n", handle->type, handle->data);
    
    if (handle->type != XFS_HANDLE_TYPE_FILE) {
        XFS_DEBUG("fs: write failed: wrong handle type=%d (expected %d)\n", handle->type, XFS_HANDLE_TYPE_FILE);
        return XFS_ERR_BADF;
    }
    
    if (!handle->data) {
        XFS_DEBUG("fs: write failed: handle->data is NULL\n");
        return XFS_ERR_BADF;
    }
    
    struct xfs_fs_file_handle_t* file_handle = get_file_handle(handle);
    if (!file_handle) {
        XFS_DEBUG("fs: write failed: file_handle is NULL\n");
        return XFS_ERR_BADF;
    }
    
    XFS_DEBUG("fs: write file_handle=%p fd=%d size=%d\n", (void*)file_handle, file_handle->fd, size);
    
    if (file_handle->fd < 0) {
        XFS_DEBUG("fs: write failed: invalid fd=%d\n", file_handle->fd);
        return XFS_ERR_BADF;
    }
    
    // Verify file descriptor is still valid by checking its flags
#ifdef WIN32
    // Windows doesn't have fcntl, assume file is writable if opened
    // We can't check the actual flags, so we'll trust the file handle state
    (void)0;  // No-op on Windows - skip flag check
#else
    int fd_flags = fcntl(file_handle->fd, F_GETFL);
    if (fd_flags < 0) {
        XFS_DEBUG("fs: write failed: fd=%d is invalid (fcntl F_GETFL failed: %s)\n", file_handle->fd, strerror(errno));
        return XFS_ERR_BADF;
    }
    
    XFS_DEBUG("fs: write fd=%d flags=0x%x (O_WRONLY=0x%x O_RDWR=0x%x O_RDONLY=0x%x O_ACCMODE=0x%x)\n", 
              file_handle->fd, fd_flags, O_WRONLY, O_RDWR, O_RDONLY, O_ACCMODE);
    
    // Check if file descriptor is opened for writing
    int accmode = fd_flags & O_ACCMODE;
    XFS_DEBUG("fs: write fd=%d accmode=0x%x\n", file_handle->fd, accmode);
    if (accmode == O_RDONLY) {
        XFS_DEBUG("fs: write failed: fd=%d opened read-only (accmode=0x%x)\n", file_handle->fd, accmode);
        return XFS_ERR_BADF;
    }
#endif
    
    ssize_t bytes_written = write(file_handle->fd, buffer, size);
    
    if (bytes_written < 0) {
        XFS_DEBUG("fs: write failed: fd=%d error=%s (errno=%d)\n", file_handle->fd, strerror(errno), errno);
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: write success bytes=%zd\n", bytes_written);
    return (int32_t)bytes_written;
}

// Close file
static int16_t fs_close(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle)
{
    struct xfs_fs_file_handle_t* file_handle = get_file_handle(handle);
    if (!file_handle) {
        return XFS_ERR_BADF;
    }
    
    int ret = close(file_handle->fd);
    libspectrum_free(file_handle);
    handle->data = NULL;
    
    if (ret != 0) {
        XFS_DEBUG("fs: close failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: close success\n");
    return XFS_ERR_OK;
}

// Seek in file
static int32_t fs_lseek(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, int32_t offset, uint8_t whence)
{
    struct xfs_fs_file_handle_t* file_handle = get_file_handle(handle);
    if (!file_handle) {
        return XFS_ERR_BADF;
    }
    
    int posix_whence;
    if (whence == XFS_SEEK_SET)
        posix_whence = SEEK_SET;
    else if (whence == XFS_SEEK_CUR)
        posix_whence = SEEK_CUR;
    else if (whence == XFS_SEEK_END)
        posix_whence = SEEK_END;
    else
        return XFS_ERR_INVAL;
    
    off_t new_pos = lseek(file_handle->fd, (off_t)offset, posix_whence);
    
    if (new_pos == (off_t)-1) {
        XFS_DEBUG("fs: lseek failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: lseek success new_pos=%ld\n", (long)new_pos);
    return (int32_t)new_pos;
}

// Open directory
static int16_t fs_opendir(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, const char* path)
{
    char* full_path = build_path(path);
    
    struct xfs_fs_dir_handle_t* dir_handle = libspectrum_malloc(sizeof(struct xfs_fs_dir_handle_t));
    if (!dir_handle) {
        return XFS_ERR_NOMEM;
    }

    /* Overlay cursors use the child engine's logical directory-entry index.
     * This allocation is not zeroed, so leaving position unset leaks an
     * allocator value through fs_telldir() and makes overlay reject entries. */
    dir_handle->position = 0;
    
    dir_handle->dir = opendir(full_path);
    
    if (dir_handle->dir == NULL) {
        XFS_DEBUG("fs: opendir %s failed: %s\n", full_path, strerror(errno));
        libspectrum_free(dir_handle);
        return xfs_error_from_errno(errno);
    }
    strncpy(dir_handle->path, full_path, sizeof(dir_handle->path) - 1);
    dir_handle->path[sizeof(dir_handle->path) - 1] = '\0';
    
    handle->type = XFS_HANDLE_TYPE_DIR;
    handle->data = dir_handle;
    XFS_DEBUG("fs: opendir %s success\n", full_path);
    return XFS_ERR_OK;
}

// Read directory entry
static int16_t fs_readdir(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, struct xfs_stat_info* info)
{
    struct xfs_fs_dir_handle_t* dir_handle = get_dir_handle(handle);
    if (!dir_handle) {
        return XFS_ERR_BADF;
    }
    
    struct dirent *entry;
    int err = 0;
    
    // Read directory entries, skipping hidden dot entries but preserving parent navigation.
    errno = 0;
    do
    {
        entry = readdir(dir_handle->dir);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                err = errno;
            }
            break; // End of directory or error
        }
    } while (entry->d_name[0] == '.' && strcmp(entry->d_name, "..") != 0);
    
    if (err != 0)
    {
        XFS_DEBUG("fs: readdir failed: %s\n", strerror(err));
        return xfs_error_from_errno(err);
    }
    
    if (entry == NULL)
    {
        // End of directory
        return 0;
    }

    memset(info, 0, sizeof(*info));
    info->storage = FS_STORAGE_FLASH;

    // Copy filename
    strncpy(info->name, entry->d_name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    
    // Determine type and size
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_handle->path, entry->d_name);
    struct stat st;
    if (stat(full_path, &st) == 0)
    {
        info->atime = (uint32_t)st.st_atime;
        info->mtime = (uint32_t)st.st_mtime;
        info->ctime = (uint32_t)st.st_ctime;

        if (S_ISDIR(st.st_mode))
        {
            info->type = XFS_TYPE_DIR;
            info->size = 0;
        }
        else
        {
            info->type = XFS_TYPE_REG;
            info->size = (uint32_t)st.st_size;
        }
    }
    else
    {
        // Default to regular file if stat fails
        info->type = XFS_TYPE_REG;
        info->size = 0;
    }
    
    XFS_DEBUG("fs: readdir success name=%s type=%d\n", info->name, info->type);
    if (dir_handle->position == UINT32_MAX)
        return XFS_ERR_INVAL;
    dir_handle->position++;
    return 1; // Success, not end of directory
}

static int16_t fs_telldir(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, uint32_t* position)
{
    (void)engine;
    struct xfs_fs_dir_handle_t* dir_handle = get_dir_handle(handle);
    if (!dir_handle || !position)
        return XFS_ERR_BADF;

    *position = dir_handle->position;
    return XFS_ERR_OK;
}

static int16_t fs_seekdir(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle, uint32_t position)
{
    (void)engine;
    struct xfs_fs_dir_handle_t* dir_handle = get_dir_handle(handle);
    if (!dir_handle)
        return XFS_ERR_BADF;

    rewinddir(dir_handle->dir);
    dir_handle->position = 0;
    struct xfs_stat_info ignored;
    while (dir_handle->position < position)
    {
        const int16_t err = fs_readdir(engine, handle, &ignored);
        if (err <= 0)
            return err < 0 ? err : XFS_ERR_INVAL;
    }
    return XFS_ERR_OK;
}

// Close directory
static int16_t fs_closedir(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle)
{
    struct xfs_fs_dir_handle_t* dir_handle = get_dir_handle(handle);
    if (!dir_handle) {
        return XFS_ERR_BADF;
    }
    
    int ret = closedir(dir_handle->dir);
    libspectrum_free(dir_handle);
    handle->data = NULL;
    
    if (ret != 0) {
        XFS_DEBUG("fs: closedir failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: closedir success\n");
    return XFS_ERR_OK;
}

// Stat file/directory
static int16_t fs_stat(const struct xfs_engine_mount_t* engine, const char* path, struct xfs_stat_info* stat_info)
{
    char* full_path = build_path(path);
    struct stat st;
    int ret = stat(full_path, &st);
    
    if (ret != 0) {
        XFS_DEBUG("fs: stat failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }

    memset(stat_info, 0, sizeof(*stat_info));
    stat_info->storage = FS_STORAGE_FLASH;
    stat_info->atime = (uint32_t)st.st_atime;
    stat_info->mtime = (uint32_t)st.st_mtime;
    stat_info->ctime = (uint32_t)st.st_ctime;
    
    // Extract filename from path
    const char* filename = strrchr(path, '/');
    if (filename) {
        filename++;
    } else {
        filename = path;
    }
    strncpy(stat_info->name, filename, sizeof(stat_info->name) - 1);
    stat_info->name[sizeof(stat_info->name) - 1] = '\0';
    
    if (S_ISDIR(st.st_mode))
    {
        stat_info->type = XFS_TYPE_DIR;
        stat_info->size = 0;
    }
    else if (S_ISREG(st.st_mode))
    {
        stat_info->type = XFS_TYPE_REG;
        stat_info->size = (uint32_t)st.st_size;
    }
    else
    {
        // Unknown type, treat as regular file
        stat_info->type = XFS_TYPE_REG;
        stat_info->size = (uint32_t)st.st_size;
    }
    
    XFS_DEBUG("fs: stat success name=%s type=%d size=%u\n", stat_info->name, stat_info->type, stat_info->size);
    return XFS_ERR_OK;
}

// Unlink file
static int16_t fs_unlink(const struct xfs_engine_mount_t* engine, const char* path)
{
    char* full_path = build_path(path);
    int ret = unlink(full_path);
    
    if (ret != 0) {
        XFS_DEBUG("fs: unlink failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: unlink success\n");
    return XFS_ERR_OK;
}

// Create directory
static int16_t fs_mkdir(const struct xfs_engine_mount_t* engine, const char* path)
{
    char* full_path = build_path(path);
    int ret = mkdir(full_path, 0755);
    
    if (ret != 0) {
        XFS_DEBUG("fs: mkdir failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: mkdir success\n");
    return XFS_ERR_OK;
}

// Remove directory
static int16_t fs_rmdir(const struct xfs_engine_mount_t* engine, const char* path)
{
    char* full_path = build_path(path);
    int ret = rmdir(full_path);
    
    if (ret != 0) {
        XFS_DEBUG("fs: rmdir failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: rmdir success\n");
    return XFS_ERR_OK;
}

// Change directory (verify it exists)
static int16_t fs_chdir(const struct xfs_engine_mount_t* engine, const char* path)
{
    char* full_path = build_path(path);
    struct stat st;
    int err = stat(full_path, &st);
    
    if (err != 0 || !S_ISDIR(st.st_mode))
    {
        XFS_DEBUG("fs: chdir failed: path not found or not a directory\n");
        return XFS_ERR_NOENT;
    }
    
    // For FUSE, we can actually change directory
    if (chdir(full_path) != 0)
    {
        XFS_DEBUG("fs: chdir failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: chdir success\n");
    return XFS_ERR_OK;
}

// Get current working directory
static int16_t fs_getcwd(const struct xfs_engine_mount_t* engine, char* buffer, uint16_t size)
{
    if (size < 2)
    {
        return XFS_ERR_INVAL;
    }
    
    // For FUSE, get actual current directory, but fallback to "/" if needed
    if (getcwd(buffer, size) == NULL)
    {
        // Fallback to root if getcwd fails
        strncpy(buffer, "/", size - 1);
        buffer[size - 1] = '\0';
    }
    
    XFS_DEBUG("fs: getcwd success path=%s\n", buffer);
    return XFS_ERR_OK;
}

// Rename file/directory
static int16_t fs_rename(const struct xfs_engine_mount_t* engine, const char* old_path, const char* new_path)
{
    char* full_old_path = build_path(old_path);
    char* full_new_path = build_path(new_path);
    int ret = rename(full_old_path, full_new_path);
    
    if (ret != 0) {
        XFS_DEBUG("fs: rename failed: %s\n", strerror(errno));
        return xfs_error_from_errno(errno);
    }
    
    XFS_DEBUG("fs: rename success\n");
    return XFS_ERR_OK;
}

static int16_t fs_chmod(const struct xfs_engine_mount_t* engine, const char* path, uint16_t mode)
{
    (void)engine;
    (void)path;
    (void)mode;

    return XFS_ERR_OK;
}

// Free handle
static void fs_free_handle(const struct xfs_engine_mount_t* engine, struct xfs_handle_t* handle)
{
    // Handles are freed in close/closedir
    // This function is called by xfs.c's free_handle if needed
    if (handle->type == XFS_HANDLE_TYPE_FILE)
    {
        struct xfs_fs_file_handle_t* file_handle = get_file_handle(handle);
        if (file_handle && file_handle->fd >= 0)
        {
            close(file_handle->fd);
            libspectrum_free(file_handle);
            handle->data = NULL;
        }
    }
    else if (handle->type == XFS_HANDLE_TYPE_DIR)
    {
        struct xfs_fs_dir_handle_t* dir_handle = get_dir_handle(handle);
        if (dir_handle && dir_handle->dir)
        {
            closedir(dir_handle->dir);
            libspectrum_free(dir_handle);
            handle->data = NULL;
        }
    }
}

// XFS filesystem engine
const struct xfs_engine_t xfs_ram_engine = {
    .user = NULL,
    .mount = fs_mount,
    .is_mounted = fs_is_mounted,
    .unmount = fs_unmount,
    .open = fs_open,
    .read = fs_read,
    .direct_read = NULL,
    .write = fs_write,
    .close = fs_close,
    .lseek = fs_lseek,
    .opendir = fs_opendir,
    .readdir = fs_readdir,
    .telldir = fs_telldir,
    .seekdir = fs_seekdir,
    .closedir = fs_closedir,
    .stat = fs_stat,
    .unlink = fs_unlink,
    .mkdir = fs_mkdir,
    .rmdir = fs_rmdir,
    .chdir = fs_chdir,
    .getcwd = fs_getcwd,
    .rename = fs_rename,
    .chmod = fs_chmod,
    .free_handle = fs_free_handle,
};
