#include "config.h"

#include "vfile.h"
#include "vfile_ext.h"

#include "peripherals/fs/xfs_engines.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

// Small buffer for short responses (errors, OK, file sizes, etc.)
static char short_response_buf[128];

static struct xfs_engine_mount_t vfile_xfs_ram_mount = {
    .engine = &xfs_overlay_engine,
    .mount_data = NULL  // Will be initialized in ensure_mounted()
};

// Single active handle (file or directory)
static struct xfs_handle_t active_handle;
static enum { HANDLE_NONE, HANDLE_FILE, HANDLE_DIR } handle_type = HANDLE_NONE;
static uint32_t file_position = 0;  // Current position for sequential reads/writes

// Helper: hex nibble to value
static inline int hex_nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Helper: parse hex u32 from string
static const char* parse_hex_u32(const char* p, const char* end, uint32_t* out)
{
    uint32_t v = 0;
    int any = 0;
    while (p < end)
    {
        int n = hex_nib(*p);
        if (n < 0) break;
        v = (v << 4) | (uint32_t)n;
        any = 1;
        p++;
    }
    if (!any) return NULL;
    *out = v;
    return p;
}

// Helper: decode ascii-hex string to regular string
static int hex_decode_string(const char* hex, char* out, size_t out_size)
{
    const size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0 || hex_len / 2 >= out_size)
    {
        return -1;
    }
    
    for (size_t i = 0; i < hex_len; i += 2)
    {
        int hi = hex_nib(hex[i]);
        int lo = hex_nib(hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            return -1;
        }
        out[i / 2] = (char)((hi << 4) | lo);
    }
    out[hex_len / 2] = '\0';
    return 0;
}

// Helper: encode string to ascii-hex
static void hex_encode_string(const char* str, char* out, size_t out_size)
{
    static const char hex_chars[] = "0123456789abcdef";
    size_t len = strlen(str);
    if (len * 2 >= out_size)
    {
        len = (out_size - 1) / 2;
    }
    
    for (size_t i = 0; i < len; i++)
    {
        out[i * 2] = hex_chars[(str[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex_chars[str[i] & 0xF];
    }
    out[len * 2] = '\0';
}

// Helper: convert XFS error to errno
static int xfs_errno_to_gdb_errno(int16_t xfs_err)
{
    switch (xfs_err)
    {
        case XFS_ERR_OK: return 0;
        case XFS_ERR_IO: return 5;  // EIO
        case XFS_ERR_CORRUPT: return 5;  // EIO
        case XFS_ERR_NOENT: return 2;  // ENOENT
        case XFS_ERR_EXIST: return 17;  // EEXIST
        case XFS_ERR_NOTDIR: return 20;  // ENOTDIR
        case XFS_ERR_ISDIR: return 21;  // EISDIR
        case XFS_ERR_NOTEMPTY: return 39;  // ENOTEMPTY
        case XFS_ERR_BADF: return 9;  // EBADF
        case XFS_ERR_FBIG: return 27;  // EFBIG
        case XFS_ERR_INVAL: return 22;  // EINVAL
        case XFS_ERR_NOSPC: return 28;  // ENOSPC
        case XFS_ERR_NOMEM: return 12;  // ENOMEM
        default: return 5;  // EIO
    }
}

// Helper: ensure RAM filesystem is mounted
static int ensure_mounted(void)
{
    return vfile_ext_ensure_mounted(&vfile_xfs_ram_mount);
}

// Helper: clear active handle
static int16_t clear_handle(void)
{
    int16_t result = 0;
    if (handle_type == HANDLE_FILE)
    {
        result = xfs_overlay_engine.close(&vfile_xfs_ram_mount, &active_handle);
    }
    else if (handle_type == HANDLE_DIR)
    {
        result = xfs_overlay_engine.closedir(&vfile_xfs_ram_mount, &active_handle);
    }

    xfs_overlay_engine.free_handle(&vfile_xfs_ram_mount, &active_handle);

    handle_type = HANDLE_NONE;
    file_position = 0;  // Reset position when handle is cleared
    return result;
}

// Helper: encode binary data with binary-escaped encoding
static size_t encode_binary_escaped(const uint8_t* data, size_t len, char* out, size_t out_size)
{
    size_t out_pos = 0;
    for (size_t i = 0; i < len && out_pos + 2 < out_size; i++)
    {
        uint8_t b = data[i];
        if (b == '}' || b == '#' || b == '$' || b == '*')
        {
            if (out_pos + 2 >= out_size) break;
            out[out_pos++] = '}';
            out[out_pos++] = (char)(b ^ 0x20);
        }
        else
        {
            out[out_pos++] = (char)b;
        }
    }
    return out_pos;
}

// vFile:open - vFile:open:<fd>,<flags>,<mode>,<path>
static char* handle_vfile_open(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        return "F-1,5";
    }
    
    // Clear any existing handle
    if (handle_type != HANDLE_NONE)
    {
        (void)clear_handle();  // Ignore errors when clearing for new open
    }
    
    const char* end = args + n;
    const char* p = args;
    
    // Parse fd (ignored, we use single handle)
    uint32_t fd;
    p = parse_hex_u32(p, end, &fd);
    if (!p || p >= end || *p != ',') return "F-1,22";  // EINVAL
    p++;
    
    // Parse flags
    uint32_t flags;
    p = parse_hex_u32(p, end, &flags);
    if (!p || p >= end || *p != ',') return "F-1,22";  // EINVAL
    p++;
    
    // Parse mode (ignored)
    uint32_t mode;
    p = parse_hex_u32(p, end, &mode);
    if (!p || p >= end || *p != ',') return "F-1,22";  // EINVAL
    p++;
    
    // Parse path (ascii-hex)
    char path[128];
    if (hex_decode_string(p, path, sizeof(path)) != 0)
    {
        return "F-1,22";  // EINVAL
    }
    
    // Convert flags to XFS flags
    int xfs_flags = 0;
    uint8_t accmode = flags & 0x0003;
    if (accmode == 0x0001) xfs_flags = XFS_O_WRONLY;
    else if (accmode == 0x0002) xfs_flags = XFS_O_RDWR;
    else xfs_flags = XFS_O_RDONLY;
    
    if (flags & 0x0008) xfs_flags |= XFS_O_APPEND;
    if (flags & 0x0100) xfs_flags |= XFS_O_CREAT;
    if (flags & 0x0200) xfs_flags |= XFS_O_CREAT | XFS_O_TRUNC;
    
    // Initialize handle (zero out entire structure)
    memset(&active_handle, 0, sizeof(active_handle));
    active_handle.type = XFS_HANDLE_TYPE_FILE;
    const int16_t result = xfs_overlay_engine.open(&vfile_xfs_ram_mount, &active_handle, path, xfs_flags);
    if (result == XFS_ERR_OK)
    {
        handle_type = HANDLE_FILE;
        file_position = 0;  // Reset position on open
        snprintf(short_response_buf, sizeof(short_response_buf), "F%x", 1);  // Return fd=1
        return short_response_buf;
    }

    const int errno_val = xfs_errno_to_gdb_errno(result);
    snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
    return short_response_buf;
}

// vFile:close - vFile:close:<fd>
static char* handle_vfile_close(const char* args, uint32_t n)
{
    const char* end = args + n;
    uint32_t fd;
    if (!parse_hex_u32(args, end, &fd))
    {
        return "F-1,22";  // EINVAL
    }
    
    if (handle_type != HANDLE_FILE)
    {
        return "F-1,9";  // EBADF
    }
    
    int16_t result = clear_handle();
    if (result != 0)
    {
        // Close/sync failed - report error
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
        return short_response_buf;
    }
    
    return "F0";
}

// vFile:pread - vFile:pread:<fd>,<count> (sequential read, no offset)
static char* handle_vfile_pread(const char* args, uint32_t n)
{
    if (handle_type != HANDLE_FILE)
    {
        return "F-1,9";  // EBADF
    }
    
    const char* end = args + n;
    const char* p = args;
    
    // Parse fd
    uint32_t fd;
    p = parse_hex_u32(p, end, &fd);
    if (!p || p >= end || *p != ',') return "F-1,22";  // EINVAL
    p++;
    
    // Parse count
    uint32_t count;
    p = parse_hex_u32(p, end, &count);
    if (!p) return "F-1,22";  // EINVAL
    
    // Limit count to fit in RSP packet (account for hex encoding overhead)
    // Response format: hex data only (2 hex digits per byte, no count prefix)
    char* response_buf = vfile_ext_get_response_buf();
    const size_t buf_size = vfile_ext_get_response_buf_size();
    const uint32_t max_binary = (buf_size - 1) / 2;  // Reserve 1 byte for null terminator
    if (count > max_binary) count = max_binary;
    
    // Read data
    const int16_t bytes_read = xfs_overlay_engine.read(&vfile_xfs_ram_mount, &active_handle, (uint8_t*)response_buf, count);
    if (bytes_read < 0)
    {
        const int errno_val = xfs_errno_to_gdb_errno(bytes_read);
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
        return short_response_buf;
    }
    
    // Calculate how many bytes we can actually encode (must be complete bytes = pairs of hex digits)
    // Response format: just hex data (no count prefix)
    // Reserve 1 byte for null terminator
    const size_t available = buf_size - 1;
    int16_t bytes_to_encode = bytes_read;
    if (available < (size_t)bytes_read * 2)
    {
        // Truncate to fit - must be even number of hex digits (complete bytes)
        bytes_to_encode = (int16_t)(available / 2);
    }
    
    // Update position for next sequential read
    file_position += (uint32_t)bytes_to_encode;
    
    // Encode response in-place: just hex data (no count prefix)
    bin_to_hex((const uint8_t*)response_buf, (uint16_t)bytes_to_encode, response_buf);
    
    return response_buf;
}

// vFile:pwrite - vFile:pwrite:<fd>,<hex-data> (sequential write, no offset)
static char* handle_vfile_pwrite(const char* args, uint32_t n)
{
    if (handle_type != HANDLE_FILE)
    {
        return "F-1,9";  // EBADF
    }
    
    // Validate args and length
    if (!args || n == 0) return "F-1,22";  // EINVAL
    
    const char* end = args + n;
    const char* p = args;
    
    // Parse fd
    uint32_t fd;
    p = parse_hex_u32(p, end, &fd);
    if (!p || p >= end || *p != ',') return "F-1,22";  // EINVAL
    p++;
    
    // Validate we have data after the comma
    if (p >= end) return "F-1,22";  // EINVAL
    
    // Decode hex data (two hex digits per byte)
    const size_t hex_len = (size_t)(end - p);
    if (hex_len == 0) return "F-1,22";  // EINVAL - no data
    if (hex_len % 2 != 0) return "F-1,22";  // EINVAL - odd number of hex digits
    const size_t decoded_len = hex_len / 2;
    
    // Use tmpbuf for decoding to avoid buffer overflow in recv_data
    // tmpbuf is large enough (128KB = 0x20000) and shared with vfile_ext
    char* decode_buf = vfile_ext_get_response_buf();
    size_t decode_buf_size = vfile_ext_get_response_buf_size();
    
    // Limit decoded_len to what fits in decode buffer
    size_t actual_decoded_len = decoded_len;
    if (actual_decoded_len > decode_buf_size) {
        actual_decoded_len = decode_buf_size;
    }
    
    // Limit hex_len to match (must be even)
    size_t actual_hex_len = actual_decoded_len * 2;
    if (actual_hex_len > hex_len) {
        actual_hex_len = hex_len;
        actual_decoded_len = actual_hex_len / 2;
    }
    
    // Validate p is within bounds before decoding
    // p points into args, which should be within the packet buffer
    if (p < args || p + actual_hex_len > args + n) {
        return "F-1,22";  // EINVAL - bounds check failed
    }
    
    // Decode hex data into tmpbuf (safe, large buffer)
    hex_to_bin(p, (uint16_t)actual_decoded_len, (uint8_t*)decode_buf);
    
    // Write data from decode buffer
    const int16_t bytes_written = xfs_overlay_engine.write(&vfile_xfs_ram_mount, &active_handle, (const uint8_t*)decode_buf, actual_decoded_len);
    if (bytes_written < 0)
    {
        const int errno_val = xfs_errno_to_gdb_errno(bytes_written);
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
        return short_response_buf;
    }
    
    // Update position for next sequential write
    file_position += (uint32_t)bytes_written;
    
    snprintf(short_response_buf, sizeof(short_response_buf), "F%x", bytes_written);
    return short_response_buf;
}

// vFile:size - vFile:size:<path>
static char* handle_vfile_size(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", 5);  // EIO
        return short_response_buf;
    }
    
    // Parse path (ascii-hex)
    char path[256];
    if (hex_decode_string(args, path, sizeof(path)) != 0)
    {
        return "F-1,22";  // EINVAL
    }
    struct xfs_stat_info info;
    const int16_t result = xfs_overlay_engine.stat(&vfile_xfs_ram_mount, path, &info);
    if (result == XFS_ERR_OK)
    {
        snprintf(short_response_buf, sizeof(short_response_buf), "F%lx", (unsigned long)info.size);
        return short_response_buf;
    }
    else
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
        return short_response_buf;
    }
}

// vFile:exists - vFile:exists:<path>
static char* handle_vfile_exists(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", 5);  // EIO
        return short_response_buf;
    }
    
    // Parse path (ascii-hex)
    char path[256];
    if (hex_decode_string(args, path, sizeof(path)) != 0)
    {
        return "F-1,22";  // EINVAL
    }
    struct xfs_stat_info info;
    const int16_t result = xfs_overlay_engine.stat(&vfile_xfs_ram_mount, path, &info);
    if (result == XFS_ERR_OK)
    {
        return "F,1";
    }
    else if (result == XFS_ERR_NOENT)
    {
        return "F,0";
    }
    else
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
        return short_response_buf;
    }
}

// vFile:unlink - vFile:unlink:<path>
static char* handle_vfile_unlink(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", 5);  // EIO
        return short_response_buf;
    }
    
    // Parse path (ascii-hex)
    char path[256];
    if (hex_decode_string(args, path, sizeof(path)) != 0)
    {
        return "F-1,22";  // EINVAL
    }
    const int16_t result = xfs_overlay_engine.unlink(&vfile_xfs_ram_mount, path);
    if (result == XFS_ERR_OK)
    {
        return "F0";
    }
    else
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "F-1,%d", errno_val);
        return short_response_buf;
    }
}

// vSpectranext:reboot
static char* handle_vspectranext_reboot(const char* args, uint32_t n)
{
    (void)args;
    (void)n;
    // Send response manually before rebooting
    vfile_ext_send_message((const uint8_t*)"OK", 2);
    // Small delay to ensure response is sent
    vfile_ext_delay_ms(10);
    vfile_ext_trigger_reset();
    return NULL;  // Response already sent
}

// vSpectranext:autoboot
static char* handle_vspectranext_autoboot(const char* args, uint32_t n)
{
    (void)args;
    (void)n;
    
    vfile_ext_autoboot();
    return "OK";
}

static char* handle_vspectranext_status(const char* args, uint32_t n)
{
    (void)args;
    (void)n;

    const int mount_result = ensure_mounted();
    if (mount_result != XFS_ERR_OK)
        return "E5";

    struct xfs_stats stats;
    memset(&stats, 0, sizeof(stats));
    const int16_t result = xfs_overlay_engine.stats
        ? xfs_overlay_engine.stats(&vfile_xfs_ram_mount, &stats) : XFS_ERR_NOATTR;
    if (result != XFS_ERR_OK && result != XFS_ERR_NOATTR)
    {
        snprintf(short_response_buf, sizeof(short_response_buf), "E%d",
            xfs_errno_to_gdb_errno(result));
        return short_response_buf;
    }

    char* response = vfile_ext_get_response_buf();
    snprintf(response, vfile_ext_get_response_buf_size(),
        "OK;version=1;fs.total_known=%u;fs.used_known=%u;fs.free_known=%u"
        ";fs.total=%" PRIu64 ";fs.used=%" PRIu64 ";fs.free=%" PRIu64
        ";usb.present=0;usb.mounted=0;usb.total=0;usb.free_known=0;usb.used=0;usb.free=0",
        stats.total_known, stats.used_known, stats.free_known,
        stats.total_bytes, stats.used_bytes, stats.free_bytes);
    return response;
}

// vSpectranext:opendir - vSpectranext:opendir:<path>
static char* handle_vspectranext_opendir(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        return "E5";  // EIO
    }
    
    // Clear any existing handle
    if (handle_type != HANDLE_NONE)
    {
        (void)clear_handle();  // Ignore errors when clearing for new open
    }
    
    // Parse path (ascii-hex)
    char path[256];
    if (hex_decode_string(args, path, sizeof(path)) != 0)
    {
        return "E22";  // EINVAL
    }
    
    // Initialize handle (zero out entire structure)
    memset(&active_handle, 0, sizeof(active_handle));
    active_handle.type = XFS_HANDLE_TYPE_DIR;
    const int16_t result = xfs_overlay_engine.opendir(&vfile_xfs_ram_mount, &active_handle, path);
    if (result == XFS_ERR_OK)
    {
        handle_type = HANDLE_DIR;
        return "OK";
    }
    else
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "E%d", errno_val);
        return short_response_buf;
    }
}

// vSpectranext:readdir - vSpectranext:readdir
static char* handle_vspectranext_readdir(const char* args, uint32_t n)
{
    (void)args;
    (void)n;
    
    if (handle_type != HANDLE_DIR)
    {
        return "E9";  // EBADF
    }
    struct xfs_stat_info info;
    const int16_t result = xfs_overlay_engine.readdir(&vfile_xfs_ram_mount, &active_handle, &info);
    if (result < 0)
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "E%d", errno_val);
        return short_response_buf;
    }
    
    if (result == 0)
    {
        // End of directory
        return "";
    }
    
    // Format: FOK,<name>,<type>,<size>
    // Name is ascii-hex encoded (can be up to 512 bytes for hex-encoded 256-byte name)
    // Use response buffer for this potentially large response
    char* response_buf = vfile_ext_get_response_buf();
    const size_t buf_size = vfile_ext_get_response_buf_size();
    char name_hex[512];
    hex_encode_string(info.name, name_hex, sizeof(name_hex));
    
    char type_char = (info.type == XFS_TYPE_DIR) ? 'D' : 'F';
    snprintf(response_buf, buf_size, "FOK,%s,%c,%lx",
             name_hex, type_char, (unsigned long)info.size);
    return response_buf;
}

// vSpectranext:closedir - vSpectranext:closedir
static char* handle_vspectranext_closedir(const char* args, uint32_t n)
{
    (void)args;
    (void)n;
    
    if (handle_type != HANDLE_DIR)
    {
        return "E9";  // EBADF
    }
    
    int16_t result = clear_handle();
    if (result != 0)
    {
        // Close failed - report error
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "E%d", errno_val);
        return short_response_buf;
    }
    
    return "OK";
}

// vSpectranext:mv - vSpectranext:mv:<old-path>,<new-path>
static char* handle_vspectranext_mv(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        return "E5";  // EIO
    }
    
    const char* end = args + n;
    const char* p = args;
    
    // Parse old path (ascii-hex)
    // Find comma separator
    const char* comma = p;
    while (comma < end && *comma != ',')
    {
        comma++;
    }
    if (comma >= end)
    {
        return "E22";  // EINVAL
    }
    
    char old_path[256];
    size_t old_path_hex_len = comma - p;
    if (old_path_hex_len >= sizeof(old_path) * 2)
    {
        return "E22";  // EINVAL
    }
    
    // Copy hex string temporarily
    char old_path_hex[512];
    if (old_path_hex_len >= sizeof(old_path_hex))
    {
        return "E22";  // EINVAL
    }
    memcpy(old_path_hex, p, old_path_hex_len);
    old_path_hex[old_path_hex_len] = '\0';
    
    if (hex_decode_string(old_path_hex, old_path, sizeof(old_path)) != 0)
    {
        return "E22";  // EINVAL
    }
    
    // Parse new path (ascii-hex)
    p = comma + 1;
    char new_path[256];
    if (hex_decode_string(p, new_path, sizeof(new_path)) != 0)
    {
        return "E22";  // EINVAL
    }
    const int16_t result = xfs_overlay_engine.rename(&vfile_xfs_ram_mount, old_path, new_path);
    if (result == XFS_ERR_OK)
    {
        return "OK";
    }
    const int errno_val = xfs_errno_to_gdb_errno(result);
    snprintf(short_response_buf, sizeof(short_response_buf), "E%d", errno_val);
    return short_response_buf;
}

// vSpectranext:mkdir - vSpectranext:mkdir:<path>
static char* handle_vspectranext_mkdir(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        return "E5";  // EIO
    }
    
    // Parse path (ascii-hex)
    char path[256];
    if (hex_decode_string(args, path, sizeof(path)) != 0)
    {
        return "E22";  // EINVAL
    }
    const int16_t result = xfs_overlay_engine.mkdir(&vfile_xfs_ram_mount, path);
    if (result == XFS_ERR_OK)
    {
        return "OK";
    }
    else
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "E%d", errno_val);
        return short_response_buf;
    }
}

// vSpectranext:rmdir - vSpectranext:rmdir:<path>
static char* handle_vspectranext_rmdir(const char* args, uint32_t n)
{
    if (ensure_mounted() != 0)
    {
        return "E5";  // EIO
    }
    
    // Parse path (ascii-hex)
    char path[256];
    if (hex_decode_string(args, path, sizeof(path)) != 0)
    {
        return "E22";  // EINVAL
    }
    const int16_t result = xfs_overlay_engine.rmdir(&vfile_xfs_ram_mount, path);
    if (result == XFS_ERR_OK)
    {
        return "OK";
    }
    else
    {
        int errno_val = xfs_errno_to_gdb_errno(result);
        snprintf(short_response_buf, sizeof(short_response_buf), "E%d", errno_val);
        return short_response_buf;
    }
}

// Main handler for vFile and vSpectranext packets
char* vfile_handle_v(const char* name, const char* args, uint32_t n)
{
    if (!strncmp("File:", name, 5))
    {
        const char* subcmd_with_args = name + 5;
        
        // Extract subcommand (everything before the first ':')
        const char* colon = strchr(subcmd_with_args, ':');
        char subcmd_buf[64];
        const char* subcmd;
        const char* actual_args;
        uint32_t actual_args_len;
        
        if (colon)
        {
            // Subcommand has arguments after ':' - arguments are in name string
            size_t subcmd_len = colon - subcmd_with_args;
            if (subcmd_len >= sizeof(subcmd_buf))
            {
                return "F-1,22";  // EINVAL - subcommand too long
            }
            memcpy(subcmd_buf, subcmd_with_args, subcmd_len);
            subcmd_buf[subcmd_len] = '\0';
            subcmd = subcmd_buf;
            actual_args = colon + 1;
            // Since actual_args points into name, use strlen to get the length
            actual_args_len = (uint32_t)strlen(actual_args);
        }
        else
        {
            // No arguments, subcommand is the rest of the string
            subcmd = subcmd_with_args;
            actual_args = args;  // Use args parameter if provided
            actual_args_len = n;
        }
        
        if (!strcmp("open", subcmd))
        {
            return handle_vfile_open(actual_args, actual_args_len);
        }
        else if (!strcmp("close", subcmd))
        {
            return handle_vfile_close(actual_args, actual_args_len);
        }
        else if (!strcmp("pread", subcmd))
        {
            return handle_vfile_pread(actual_args, actual_args_len);
        }
        else if (!strcmp("pwrite", subcmd))
        {
            return handle_vfile_pwrite(actual_args, actual_args_len);
        }
        else if (!strcmp("size", subcmd))
        {
            return handle_vfile_size(actual_args, actual_args_len);
        }
        else if (!strcmp("exists", subcmd))
        {
            return handle_vfile_exists(actual_args, actual_args_len);
        }
        else if (!strcmp("unlink", subcmd))
        {
            return handle_vfile_unlink(actual_args, actual_args_len);
        }
    }
    else if (!strncmp("Spectranext:", name, 12))
    {
        const char* subcmd_with_args = name + 12;
        
        // Extract subcommand (everything before the first ':')
        const char* colon = strchr(subcmd_with_args, ':');
        char subcmd_buf[64];
        const char* subcmd;
        const char* actual_args;
        uint32_t actual_args_len;
        
        if (colon)
        {
            // Subcommand has arguments after ':'
            size_t subcmd_len = colon - subcmd_with_args;
            if (subcmd_len >= sizeof(subcmd_buf))
            {
                return "E22";  // EINVAL - subcommand too long
            }
            memcpy(subcmd_buf, subcmd_with_args, subcmd_len);
            subcmd_buf[subcmd_len] = '\0';
            subcmd = subcmd_buf;
            actual_args = colon + 1;
            actual_args_len = (uint32_t)(n - (actual_args - name));
        }
        else
        {
            // No arguments, subcommand is the rest of the string
            subcmd = subcmd_with_args;
            actual_args = args;  // Use args parameter if provided
            actual_args_len = n;
        }
        
        if (!strcmp("reboot", subcmd))
        {
            return handle_vspectranext_reboot(actual_args, actual_args_len);
        }
        else if (!strcmp("autoboot", subcmd))
        {
            return handle_vspectranext_autoboot(actual_args, actual_args_len);
        }
        else if (!strcmp("status", subcmd))
        {
            return handle_vspectranext_status(actual_args, actual_args_len);
        }
        else if (!strcmp("opendir", subcmd))
        {
            return handle_vspectranext_opendir(actual_args, actual_args_len);
        }
        else if (!strcmp("readdir", subcmd))
        {
            return handle_vspectranext_readdir(actual_args, actual_args_len);
        }
        else if (!strcmp("closedir", subcmd))
        {
            return handle_vspectranext_closedir(actual_args, actual_args_len);
        }
        else if (!strcmp("mv", subcmd))
        {
            return handle_vspectranext_mv(actual_args, actual_args_len);
        }
        else if (!strcmp("mkdir", subcmd))
        {
            return handle_vspectranext_mkdir(actual_args, actual_args_len);
        }
        else if (!strcmp("rmdir", subcmd))
        {
            return handle_vspectranext_rmdir(actual_args, actual_args_len);
        }
    }
    
    return "";  // Unknown command, return empty
}
