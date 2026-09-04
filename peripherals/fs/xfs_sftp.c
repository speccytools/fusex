#include "config.h"

#include "xfs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "compat.h"
#include "../security/ssh.h"

#define SFTP_DEFAULT_PORT 22
#define SFTP_HOST_MAX 128
#define SFTP_USER_MAX 64
#define SFTP_PASSWORD_MAX 96
#define SFTP_SEEK_CACHE_MAX 256

struct sftp_engine_mount_data_t
{
    compat_socket_t socket_fd;
    LIBSSH2_SESSION* session;
    LIBSSH2_SFTP* sftp;
    char host[SFTP_HOST_MAX];
    char username[SFTP_USER_MAX];
    char password[SFTP_PASSWORD_MAX];
    char root[XFS_PATH_MAX];
    uint16_t port;
};

struct sftp_seek_cache_entry
{
    uint64_t backend;
    uint32_t frontend;
    struct xfs_stat_info info;
};

struct sftp_engine_handle_t
{
    LIBSSH2_SFTP_HANDLE* handle;
    struct sftp_seek_cache_entry seek_cache[SFTP_SEEK_CACHE_MAX];
    uint64_t backend_position;
    uint32_t current_position;
    uint32_t cache_count;
    uint8_t end_of_directory;
};

static int16_t sftp_error(const struct sftp_engine_mount_data_t* mount)
{
    const unsigned long error = mount && mount->sftp ? libssh2_sftp_last_error(mount->sftp) : 0;

    switch (error)
    {
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH: return XFS_ERR_NOENT;
    case LIBSSH2_FX_PERMISSION_DENIED: return XFS_ERR_IO;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS: return XFS_ERR_EXIST;
    case LIBSSH2_FX_DIR_NOT_EMPTY: return XFS_ERR_NOTEMPTY;
    case LIBSSH2_FX_INVALID_FILENAME: return XFS_ERR_INVAL;
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
    case LIBSSH2_FX_QUOTA_EXCEEDED: return XFS_ERR_NOSPC;
    default: return XFS_ERR_IO;
    }
}

static int hex_value(const char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_component(char* out, const size_t out_size, const char* start, const size_t length)
{
    size_t written = 0;
    for (size_t i = 0; i < length; i++)
    {
        unsigned char value = (unsigned char)start[i];
        if (value == '%' && i + 2 < length)
        {
            const int hi = hex_value(start[i + 1]);
            const int lo = hex_value(start[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            value = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (value == 0 || written + 1 >= out_size) return -1;
        out[written++] = (char)value;
    }
    out[written] = '\0';
    return 0;
}

static int parse_host(const char* hostname, struct sftp_engine_mount_data_t* mount)
{
    const char* host_start = hostname;
    const char* host_end = hostname + strlen(hostname);
    const char* port_start = NULL;
    if (*host_start == '[')
    {
        const char* close = strchr(host_start + 1, ']');
        if (!close || close == host_start + 1) return -1;
        if (close[1] == ':') port_start = close + 2;
        else if (close[1] != '\0') return -1;
        host_start++;
        host_end = close;
    }
    else
    {
        const char* host_colon = strrchr(host_start, ':');
        if (host_colon)
        {
            host_end = host_colon;
            port_start = host_colon + 1;
        }
    }

    if (host_end == host_start || decode_component(mount->host, sizeof(mount->host),
        host_start, (size_t)(host_end - host_start)) != 0)
        return -1;

    mount->port = SFTP_DEFAULT_PORT;
    if (port_start)
    {
        char* end = NULL;
        errno = 0;
        const unsigned long port = strtoul(port_start, &end, 10);
        if (errno || !end || *end || port == 0 || port > 65535) return -1;
        mount->port = (uint16_t)port;
    }
    return 0;
}

static int normalize_root(const char* path, char* out, const size_t out_size)
{
    if (!path || !path[0]) path = "/";
    const int written = path[0] == '/' ? snprintf(out, out_size, "%s", path) :
        snprintf(out, out_size, "/%s", path);
    if (written < 0 || (size_t)written >= out_size) return -1;
    size_t length = strlen(out);
    while (length > 1 && out[length - 1] == '/') out[--length] = '\0';
    return 0;
}

static int remote_path(const struct sftp_engine_mount_data_t* mount, const char* path,
    char* out, const size_t out_size)
{
    while (path && *path == '/') path++;
    const int written = !path || !*path ? snprintf(out, out_size, "%s", mount->root) :
        strcmp(mount->root, "/") == 0 ? snprintf(out, out_size, "/%s", path) :
        snprintf(out, out_size, "%s/%s", mount->root, path);
    return written < 0 || (size_t)written >= out_size ? -1 : 0;
}

static compat_socket_t connect_tcp(const char* host, const uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo* addresses = NULL;
    char service[8];
    compat_socket_t result = compat_socket_invalid;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &addresses) != 0) return compat_socket_invalid;

    for (const struct addrinfo* address = addresses; address; address = address->ai_next)
    {
        compat_socket_t fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == compat_socket_invalid || (intptr_t)fd < 0) continue;
        compat_socket_blocking_mode(fd, 0);
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
        {
            result = fd;
            break;
        }
        compat_socket_close(fd);
    }
    freeaddrinfo(addresses);
    return result;
}

static void disconnect_sftp(struct sftp_engine_mount_data_t* mount)
{
    if (!mount) return;
    if (mount->sftp) libssh2_sftp_shutdown(mount->sftp);
    mount->sftp = NULL;
    if (mount->session)
    {
        libssh2_session_disconnect(mount->session, "SFTP filesystem unmounted");
        libssh2_session_free(mount->session);
    }
    mount->session = NULL;
    if (mount->socket_fd != compat_socket_invalid) compat_socket_close(mount->socket_fd);
    mount->socket_fd = compat_socket_invalid;
    memset(mount->password, 0, sizeof(mount->password));
}

static void fill_stat(struct xfs_stat_info* info, const LIBSSH2_SFTP_ATTRIBUTES* attrs,
    const char* name)
{
    memset(info, 0, sizeof(*info));
    info->storage = FS_STORAGE_RAM;
    if ((attrs->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs->permissions))
        info->type = XFS_TYPE_DIR;
    else
        info->type = XFS_TYPE_REG;
    if (attrs->flags & LIBSSH2_SFTP_ATTR_SIZE)
        info->size = attrs->filesize > UINT32_MAX ? UINT32_MAX : (uint32_t)attrs->filesize;
    if (attrs->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
    {
        info->atime = attrs->atime;
        info->mtime = attrs->mtime;
    }
    if (name)
    {
        strncpy(info->name, name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
    }
}

static int16_t sftp_mount(const struct xfs_engine_t* engine, const char* hostname,
    const char* path, const char* username, const char* password,
    struct xfs_engine_mount_t* out_mount)
{
    (void)engine;
    if (!hostname || !path || !username || !username[0] || !password || !out_mount)
        return XFS_ERR_INVAL;
    struct sftp_engine_mount_data_t* mount = calloc(1, sizeof(*mount));
    if (!mount) return XFS_ERR_NOMEM;
    mount->socket_fd = compat_socket_invalid;

    if (parse_host(hostname, mount) != 0 ||
        decode_component(mount->username, sizeof(mount->username), username, strlen(username)) != 0 ||
        decode_component(mount->password, sizeof(mount->password), password, strlen(password)) != 0 ||
        normalize_root(path, mount->root, sizeof(mount->root)) != 0)
    {
        free(mount);
        return XFS_ERR_INVAL;
    }
    if (ssh_socket_init() != 0) goto io_error;
    mount->socket_fd = connect_tcp(mount->host, mount->port);
    if (mount->socket_fd == compat_socket_invalid) goto io_error;
    mount->session = libssh2_session_init();
    if (!mount->session) goto memory_error;
    libssh2_session_set_blocking(mount->session, 1);
    if (libssh2_session_handshake(mount->session, mount->socket_fd) != 0) goto io_error;
    if (ssh_session_verify_or_trust_host(mount->session, mount->host, mount->port) != 0) goto io_error;
    if (libssh2_userauth_password(mount->session, mount->username, mount->password) != 0) goto io_error;
    memset(mount->password, 0, sizeof(mount->password));
    mount->sftp = libssh2_sftp_init(mount->session);
    if (!mount->sftp) goto io_error;

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    if (libssh2_sftp_stat(mount->sftp, mount->root, &attrs) != 0) goto path_error;
    if (!(attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISDIR(attrs.permissions))
    {
        disconnect_sftp(mount);
        free(mount);
        return XFS_ERR_NOTDIR;
    }
    out_mount->mount_data = mount;
    return XFS_ERR_OK;

path_error:
    {
        const int16_t error = sftp_error(mount);
        disconnect_sftp(mount);
        free(mount);
        return error;
    }
memory_error:
    disconnect_sftp(mount);
    free(mount);
    return XFS_ERR_NOMEM;
io_error:
    disconnect_sftp(mount);
    free(mount);
    return XFS_ERR_IO;
}

static void sftp_unmount(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* engine_mount)
{
    (void)engine;
    xfs_close_handles_for_mount(engine_mount);
    struct sftp_engine_mount_data_t* mount = engine_mount ? engine_mount->mount_data : NULL;
    disconnect_sftp(mount);
    free(mount);
    if (engine_mount) engine_mount->mount_data = NULL;
}

static void sftp_mount_info(const struct xfs_engine_mount_t* engine_mount, char* buffer, size_t size)
{
    if (!buffer || !size) return;
    buffer[0] = '\0';
    const struct sftp_engine_mount_data_t* mount = engine_mount ? engine_mount->mount_data : NULL;
    if (!mount) return;
    const bool ipv6 = strchr(mount->host, ':') != NULL;
    if (mount->port == SFTP_DEFAULT_PORT)
        snprintf(buffer, size, ipv6 ? "sftp://%s@[%s]%s" : "sftp://%s@%s%s",
            mount->username, mount->host, mount->root);
    else
        snprintf(buffer, size, ipv6 ? "sftp://%s@[%s]:%u%s" : "sftp://%s@%s:%u%s",
            mount->username, mount->host, mount->port, mount->root);
}

static int16_t sftp_open(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    const char* path, const int flags)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    if (!mount || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;

    unsigned long open_flags = 0;
    switch (flags & 3)
    {
    case XFS_O_WRONLY: open_flags |= LIBSSH2_FXF_WRITE; break;
    case XFS_O_RDWR: open_flags |= LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE; break;
    default: open_flags |= LIBSSH2_FXF_READ; break;
    }
    if (flags & XFS_O_CREAT) open_flags |= LIBSSH2_FXF_CREAT;
    if (flags & XFS_O_EXCL) open_flags |= LIBSSH2_FXF_EXCL;
    if (flags & XFS_O_TRUNC) open_flags |= LIBSSH2_FXF_TRUNC;
    if (flags & XFS_O_APPEND) open_flags |= LIBSSH2_FXF_APPEND;

    struct sftp_engine_handle_t* file = calloc(1, sizeof(*file));
    if (!file) return XFS_ERR_NOMEM;
    file->handle = libssh2_sftp_open(mount->sftp, remote, open_flags, 0644);
    if (!file->handle)
    {
        const int16_t error = sftp_error(mount);
        free(file);
        return error;
    }
    handle->type = XFS_HANDLE_TYPE_FILE;
    handle->data = file;
    return XFS_ERR_OK;
}

static int32_t sftp_read(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    void* buffer, uint32_t size)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* file = handle ? handle->data : NULL;
    if (!file || !file->handle) return XFS_ERR_BADF;
    const ssize_t result = libssh2_sftp_read(file->handle, buffer, size);
    return result < 0 ? XFS_ERR_IO : (int32_t)result;
}

static int32_t sftp_write(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    const void* buffer, uint32_t size)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* file = handle ? handle->data : NULL;
    if (!file || !file->handle) return XFS_ERR_BADF;
    const ssize_t result = libssh2_sftp_write(file->handle, buffer, size);
    return result < 0 ? XFS_ERR_IO : (int32_t)result;
}

static int16_t sftp_close(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* file = handle ? handle->data : NULL;
    if (!file || !file->handle) return XFS_ERR_OK;
    const int result = libssh2_sftp_close(file->handle);
    file->handle = NULL;
    return result == 0 ? XFS_ERR_OK : XFS_ERR_IO;
}

static int32_t sftp_lseek(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    int32_t offset, uint8_t whence)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* file = handle ? handle->data : NULL;
    if (!file || !file->handle) return XFS_ERR_BADF;
    libssh2_uint64_t base = 0;
    if (whence == XFS_SEEK_CUR) base = libssh2_sftp_tell64(file->handle);
    else if (whence == XFS_SEEK_END)
    {
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        if (libssh2_sftp_fstat(file->handle, &attrs) != 0 || !(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
            return XFS_ERR_IO;
        base = attrs.filesize;
    }
    else if (whence != XFS_SEEK_SET) return XFS_ERR_INVAL;
    const int64_t position = (int64_t)base + offset;
    if (position < 0 || position > INT32_MAX) return XFS_ERR_INVAL;
    libssh2_sftp_seek64(file->handle, (libssh2_uint64_t)position);
    return (int32_t)position;
}

static int16_t sftp_opendir(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    const char* path)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    if (!mount || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;
    struct sftp_engine_handle_t* dir = calloc(1, sizeof(*dir));
    if (!dir) return XFS_ERR_NOMEM;
    dir->handle = libssh2_sftp_opendir(mount->sftp, remote);
    if (!dir->handle)
    {
        const int16_t error = sftp_error(mount);
        free(dir);
        return error;
    }
    handle->type = XFS_HANDLE_TYPE_DIR;
    handle->data = dir;
    return XFS_ERR_OK;
}

static int16_t sftp_readdir(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    struct xfs_stat_info* info)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* dir = handle ? handle->data : NULL;
    if (!dir || !dir->handle || !info) return XFS_ERR_BADF;

    if (dir->current_position < dir->cache_count)
    {
        *info = dir->seek_cache[dir->current_position].info;
        dir->current_position++;
        return 1;
    }
    if (dir->end_of_directory) return 0;
    if (dir->cache_count >= SFTP_SEEK_CACHE_MAX) return XFS_ERR_NOSPC;

    for (;;)
    {
        char name[256];
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        const int result = libssh2_sftp_readdir(dir->handle, name, sizeof(name) - 1, &attrs);
        if (result == 0)
        {
            dir->end_of_directory = 1;
            return 0;
        }
        if (result < 0) return XFS_ERR_IO;
        dir->backend_position++;
        name[result] = '\0';
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        fill_stat(info, &attrs, name);

        struct sftp_seek_cache_entry* entry = &dir->seek_cache[dir->cache_count];
        entry->backend = dir->backend_position;
        entry->frontend = dir->cache_count + 1;
        entry->info = *info;
        dir->cache_count++;
        dir->current_position++;
        return 1;
    }
}

static int16_t sftp_telldir(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    uint32_t* position)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* dir = handle ? handle->data : NULL;
    if (!dir || !dir->handle || !position) return XFS_ERR_BADF;
    *position = dir->current_position;
    return XFS_ERR_OK;
}

static int16_t sftp_seekdir(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle,
    uint32_t position)
{
    (void)engine_mount;
    struct sftp_engine_handle_t* dir = handle ? handle->data : NULL;
    if (!dir || !dir->handle) return XFS_ERR_BADF;
    if (position == 0)
    {
        dir->current_position = 0;
        return XFS_ERR_OK;
    }
    for (uint32_t i = 0; i < dir->cache_count; i++)
    {
        if (dir->seek_cache[i].frontend == position)
        {
            dir->current_position = position;
            return XFS_ERR_OK;
        }
    }
    return XFS_ERR_INVAL;
}

static int16_t sftp_closedir(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle)
{
    return sftp_close(engine_mount, handle);
}

static int16_t sftp_stat(const struct xfs_engine_mount_t* engine_mount, const char* path,
    struct xfs_stat_info* info)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    if (!mount || !info || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;
    if (libssh2_sftp_stat(mount->sftp, remote, &attrs) != 0) return sftp_error(mount);
    const char* name = strrchr(remote, '/');
    fill_stat(info, &attrs, name && name[1] ? name + 1 : "/");
    return XFS_ERR_OK;
}

static int16_t sftp_unlink(const struct xfs_engine_mount_t* engine_mount, const char* path)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    if (!mount || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;
    return libssh2_sftp_unlink(mount->sftp, remote) == 0 ? XFS_ERR_OK : sftp_error(mount);
}

static int16_t sftp_mkdir(const struct xfs_engine_mount_t* engine_mount, const char* path)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    if (!mount || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;
    return libssh2_sftp_mkdir(mount->sftp, remote, 0755) == 0 ? XFS_ERR_OK : sftp_error(mount);
}

static int16_t sftp_rmdir(const struct xfs_engine_mount_t* engine_mount, const char* path)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    if (!mount || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;
    return libssh2_sftp_rmdir(mount->sftp, remote) == 0 ? XFS_ERR_OK : sftp_error(mount);
}

static int16_t sftp_chdir(const struct xfs_engine_mount_t* engine_mount, const char* path)
{
    struct xfs_stat_info info;
    const int16_t result = sftp_stat(engine_mount, path, &info);
    if (result != XFS_ERR_OK) return result;
    return info.type == XFS_TYPE_DIR ? XFS_ERR_OK : XFS_ERR_NOTDIR;
}

static int16_t sftp_getcwd(const struct xfs_engine_mount_t* engine_mount, char* buffer, uint16_t size)
{
    if (!engine_mount || !buffer || !size || !engine_mount->cwd) return XFS_ERR_INVAL;
    if (strlen(engine_mount->cwd) >= size) return XFS_ERR_NAMETOOLONG;
    strcpy(buffer, engine_mount->cwd);
    return XFS_ERR_OK;
}

static int16_t sftp_rename(const struct xfs_engine_mount_t* engine_mount, const char* old_path,
    const char* new_path)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char old_remote[XFS_PATH_MAX * 2];
    char new_remote[XFS_PATH_MAX * 2];
    if (!mount || remote_path(mount, old_path, old_remote, sizeof(old_remote)) != 0 ||
        remote_path(mount, new_path, new_remote, sizeof(new_remote)) != 0) return XFS_ERR_NAMETOOLONG;
    const long flags = LIBSSH2_SFTP_RENAME_OVERWRITE;
    return libssh2_sftp_rename_ex(mount->sftp, old_remote, (unsigned int)strlen(old_remote),
        new_remote, (unsigned int)strlen(new_remote), flags) == 0 ? XFS_ERR_OK : sftp_error(mount);
}

static int16_t sftp_chmod(const struct xfs_engine_mount_t* engine_mount, const char* path, uint16_t mode)
{
    struct sftp_engine_mount_data_t* mount = engine_mount->mount_data;
    char remote[XFS_PATH_MAX * 2];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    if (!mount || remote_path(mount, path, remote, sizeof(remote)) != 0) return XFS_ERR_NAMETOOLONG;
    memset(&attrs, 0, sizeof(attrs));
    attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attrs.permissions = mode & 07777;
    return libssh2_sftp_setstat(mount->sftp, remote, &attrs) == 0 ? XFS_ERR_OK : sftp_error(mount);
}

static void sftp_free_handle(const struct xfs_engine_mount_t* engine_mount, struct xfs_handle_t* handle)
{
    if (!handle || !handle->data) return;
    (void)sftp_close(engine_mount, handle);
    free(handle->data);
    handle->data = NULL;
}

const struct xfs_engine_t sftp_engine = {
    .mount = sftp_mount,
    .unmount = sftp_unmount,
    .mount_info = sftp_mount_info,
    .open = sftp_open,
    .read = sftp_read,
    .write = sftp_write,
    .close = sftp_close,
    .lseek = sftp_lseek,
    .opendir = sftp_opendir,
    .readdir = sftp_readdir,
    .telldir = sftp_telldir,
    .seekdir = sftp_seekdir,
    .closedir = sftp_closedir,
    .stat = sftp_stat,
    .unlink = sftp_unlink,
    .mkdir = sftp_mkdir,
    .rmdir = sftp_rmdir,
    .chdir = sftp_chdir,
    .getcwd = sftp_getcwd,
    .rename = sftp_rename,
    .chmod = sftp_chmod,
    .free_handle = sftp_free_handle,
};
