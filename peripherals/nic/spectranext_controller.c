#include "config.h"

#include "spectranext_controller.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <string.h>

#include "engines/engine.h"

#include "libspectrum.h"
#include "memory_pages.h"
#include "peripherals/fs/xfs.h"
#include "peripherals/fs/xfs_engines.h"
#include "peripherals/spectranet.h"

#define SPECTRANEXT_DOT_PAGE_FIRST 0xD8u
#define SPECTRANEXT_DOT_PAGE_COUNT 2u

spectranext_enginecall_args_t spectranext_enginecall_args;

spectranext_state_t spectranext_state = {
    .controller_status = WIFI_CONTROLLER_STATUS_OPERATIONAL,
    .connection_status = WIFI_CONNECT_CONNECT_IP_OBTAINED,
    .ipv4_host = 0x7f000001u,
};

static char scan_ap_names[SPECTRANEXT_SCAN_AP_MAX][64];
static uint8_t scan_ap_count;

static char pending_message[SPECTRANEXT_MESSAGE_MAX];
static bool message_pending;

volatile struct spectranext_controller_t spectranext_controller = {
    .command = SPECTRANEXT_CMD_REG_IDLE,
    .status = SPECTRANEXT_STATUS_SUCCESS,
};

/*
 * DOT lookup is controller-owned, rather than tied to the Z80's selected
 * mountpoint. Mount the default RAM-over-ROMFS overlay for this short-lived
 * operation, then fill the two dedicated DOT RAM pages directly.
 */
static int16_t spectranext_load_dot(const char *name)
{
    char path[XFS_PATH_MAX];
    const size_t prefix_len = sizeof("/bin/") - 1u;
    size_t name_len = 0;

    if (name == NULL || name[0] == '\0')
        return XFS_ERR_INVAL;

    memcpy(path, "/bin/", prefix_len);
    while (name[name_len] != '\0')
    {
        if (name_len >= sizeof(path) - prefix_len - 1u)
            return XFS_ERR_NAMETOOLONG;

        unsigned char c = (unsigned char)name[name_len];
        if (c == '/' || c == '\\')
            return XFS_ERR_INVAL;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + ('a' - 'A'));
        path[prefix_len + name_len++] = (char)c;
    }
    path[prefix_len + name_len] = '\0';

    struct xfs_engine_mount_t mount = { .engine = &xfs_overlay_engine };
    int16_t err = mount.engine->mount(mount.engine, "ram", "/", &mount);
    if (err != XFS_ERR_OK)
        return err;

    struct xfs_handle_t handle = { .type = XFS_HANDLE_TYPE_FILE };
    err = mount.engine->open(&mount, &handle, path, XFS_O_RDONLY);
    if (err == XFS_ERR_OK)
    {
        uint8_t *destination[SPECTRANEXT_DOT_PAGE_COUNT];
        uint8_t page;
        for (page = 0; page < SPECTRANEXT_DOT_PAGE_COUNT; ++page)
        {
            destination[page] = spectranet_ram_page(SPECTRANEXT_DOT_PAGE_FIRST + page);
            if (destination[page] == NULL)
            {
                err = XFS_ERR_IO;
                break;
            }
            memset(destination[page], 0, 0x1000u);
        }

        for (page = 0; err == XFS_ERR_OK && page < SPECTRANEXT_DOT_PAGE_COUNT; ++page)
        {
            const int32_t bytes_read = mount.engine->read(&mount, &handle, destination[page], 0x1000u);
            if (bytes_read < 0)
            {
                err = (int16_t)bytes_read;
                break;
            }
            if ((uint32_t)bytes_read < 0x1000u)
                break;
        }

        const int16_t close_err = mount.engine->close(&mount, &handle);
        if (err == XFS_ERR_OK && close_err != XFS_ERR_OK)
            err = close_err;
        mount.engine->free_handle(&mount, &handle);
    }

    mount.engine->unmount(mount.engine, &mount);
    return err;
}

int spectranext_enginecall_dispatch(const char *input_file, const char *output_file, const char *operation)
{
    char opbuf[256];
    strncpy(opbuf, operation, sizeof(opbuf) - 1u);
    opbuf[sizeof(opbuf) - 1u] = '\0';

    char *argv[ENGINE_MAX_ARGS];
    const int argc = engine_argv_parse(opbuf, argv, ENGINE_MAX_ARGS);
    if (argc < 1)
        return -6;

    if (strcmp(argv[0], "jsonpath") == 0 || strcmp(argv[0], "json") == 0)
        return engine_json_call(input_file, output_file, argc, argv);
    if (strcmp(argv[0], "xpath") == 0)
        return engine_xpath_call(input_file, output_file, argc, argv);
    return -1;
}

static void spectranext_set_status(uint8_t status)
{
    spectranext_controller.status = status;
}

bool spectranext_controller_post_message_bytes(const uint8_t *message, size_t length)
{
    if (message == NULL && length != 0u)
        return false;

    if (length >= SPECTRANEXT_MESSAGE_MAX)
        length = SPECTRANEXT_MESSAGE_MAX - 1u;

    if (length != 0u)
        memcpy(pending_message, message, length);
    pending_message[length] = '\0';
    message_pending = true;

    return true;
}

bool spectranext_controller_post_message(const char *message)
{
    if (message == NULL)
        return false;

    return spectranext_controller_post_message_bytes((const uint8_t *)message, strlen(message));
}

void spectranext_controller_clear_messages(void)
{
    pending_message[0] = '\0';
    message_pending = false;
}

static void spectranext_controller_get_message(void)
{
    memset((void *)&spectranext_controller.workspace.get_message.out, 0,
           sizeof(spectranext_controller.workspace.get_message.out));

    if (message_pending)
    {
        memcpy((void *)spectranext_controller.workspace.get_message.out.message, pending_message,
               sizeof(spectranext_controller.workspace.get_message.out.message));
        spectranext_controller.workspace.get_message.out.pending = 1u;
        spectranext_controller_clear_messages();
    }

    spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
}

static void spectranext_controller_process_command(void)
{
    const uint8_t cmd = spectranext_controller.command;
    spectranext_controller.command = SPECTRANEXT_CMD_REG_IDLE;

    switch (cmd)
    {
        case SPECTRANEXT_CMD_GET_CONTROLLER_STATUS:
            spectranext_controller.workspace.get_controller_status.out.controller_status =
                spectranext_state.controller_status;
            spectranext_controller.workspace.get_controller_status.out.wifi_connection =
                spectranext_state.connection_status;
            spectranext_controller.workspace.get_controller_status.out.ipv4 = spectranext_state.ipv4_host;
            spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_WIFI_SCAN_ACCESS_POINTS:
            scan_ap_count = 1;
            strncpy(scan_ap_names[0], "spectranext", sizeof(scan_ap_names[0]) - 1u);
            scan_ap_names[0][sizeof(scan_ap_names[0]) - 1u] = '\0';
            spectranext_controller.workspace.wifi_scan.io.out.scan_count =
                (uint8_t)(scan_ap_count > 255u ? 255u : scan_ap_count);
            spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_WIFI_GET_ACCESS_POINT:
        {
            const uint8_t idx = spectranext_controller.workspace.wifi_get_ap.io.in.ap_index;
            if ((uint16_t)idx >= (uint16_t)scan_ap_count)
            {
                spectranext_set_status(SPECTRANEXT_STATUS_ERROR);
                break;
            }
            strncpy((char *)spectranext_controller.workspace.wifi_get_ap.io.out.ap_name, scan_ap_names[idx],
                    sizeof(spectranext_controller.workspace.wifi_get_ap.io.out.ap_name) - 1u);
            spectranext_controller.workspace.wifi_get_ap.io.out.ap_name
                [sizeof(spectranext_controller.workspace.wifi_get_ap.io.out.ap_name) - 1u] = '\0';
            spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
            break;
        }

        case SPECTRANEXT_CMD_WIFI_CONNECT_ACCESS_POINT:
            spectranext_state.connection_status = WIFI_CONNECT_CONNECT_SUCCESS;
            spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_WIFI_DISCONNECT:
            spectranext_state.connection_status = WIFI_CONNECT_DISCONNECTED;
            spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_DNS_GETHOSTBYNAME:
        {
            char host[64];
            memcpy(host, (const void *)spectranext_controller.workspace.dns.io.in.host, 63);
            host[63] = '\0';

            if (host[0] == '\0')
            {
                spectranext_controller.workspace.dns.io.out.ipv4 = 0;
                spectranext_set_status(SPECTRANEXT_STATUS_ERROR);
                break;
            }

            struct addrinfo hints;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;

            struct addrinfo *res = NULL;
            const int gai_err = getaddrinfo(host, NULL, &hints, &res);
            if (gai_err != 0 || res == NULL)
            {
                spectranext_controller.workspace.dns.io.out.ipv4 = 0;
                spectranext_set_status(SPECTRANEXT_STATUS_ERROR);
                if (res)
                    freeaddrinfo(res);
                break;
            }

            uint32_t ipv4_host = 0;
            int found = 0;
            for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next)
            {
                if (rp->ai_family == AF_INET && rp->ai_addr != NULL)
                {
                    const struct sockaddr_in *sin = (const struct sockaddr_in *)rp->ai_addr;
                    ipv4_host = (uint32_t)sin->sin_addr.s_addr;
                    found = 1;
                    break;
                }
            }
            freeaddrinfo(res);

            if (!found)
            {
                spectranext_controller.workspace.dns.io.out.ipv4 = 0;
                spectranext_set_status(SPECTRANEXT_STATUS_ERROR);
                break;
            }

            spectranext_controller.workspace.dns.io.out.ipv4 = ipv4_host;
            spectranext_state.ipv4_host = ipv4_host;
            spectranext_set_status(SPECTRANEXT_STATUS_SUCCESS);
            break;
        }

        case SPECTRANEXT_CMD_ENGINECALL:
            memcpy(spectranext_enginecall_args.input_file,
                   (const void *)spectranext_controller.workspace.enginecall.io.input_file,
                   sizeof(spectranext_enginecall_args.input_file) - 1u);
            spectranext_enginecall_args.input_file[sizeof(spectranext_enginecall_args.input_file) - 1u] = '\0';

            memcpy(spectranext_enginecall_args.output_file,
                   (const void *)spectranext_controller.workspace.enginecall.io.output_file,
                   sizeof(spectranext_enginecall_args.output_file) - 1u);
            spectranext_enginecall_args.output_file[sizeof(spectranext_enginecall_args.output_file) - 1u] = '\0';

            memcpy(spectranext_enginecall_args.operation,
                   (const void *)spectranext_controller.workspace.enginecall.io.operation,
                   sizeof(spectranext_enginecall_args.operation) - 1u);
            spectranext_enginecall_args.operation[sizeof(spectranext_enginecall_args.operation) - 1u] = '\0';

            spectranext_enginecall_args.result = spectranext_enginecall_dispatch(
                spectranext_enginecall_args.input_file,
                spectranext_enginecall_args.output_file,
                spectranext_enginecall_args.operation);

            spectranext_set_status((uint8_t)(int8_t)spectranext_enginecall_args.result);
            break;

        case SPECTRANEXT_CMD_GET_MESSAGE:
            spectranext_controller_get_message();
            break;

        case SPECTRANEXT_CMD_LOAD_DOT:
        {
            char name[SPECTRANEXT_LOAD_DOT_NAME_MAX];
            memcpy(name, (const void *)spectranext_controller.workspace.load_dot.in.name,
                   sizeof(name) - 1u);
            name[sizeof(name) - 1u] = '\0';
            spectranext_set_status(
                spectranext_load_dot(name) == XFS_ERR_OK ? SPECTRANEXT_STATUS_SUCCESS : SPECTRANEXT_STATUS_ERROR);
            break;
        }

        default:
            spectranext_set_status(SPECTRANEXT_STATUS_ERROR);
            break;
    }
}

void spectranext_controller_init(void)
{
    spectranext_controller.command = SPECTRANEXT_CMD_REG_IDLE;
    spectranext_controller.status = SPECTRANEXT_STATUS_SUCCESS;
    spectranext_state.controller_status = WIFI_CONTROLLER_STATUS_OPERATIONAL;
    spectranext_state.connection_status = WIFI_CONNECT_CONNECT_IP_OBTAINED;
    spectranext_state.ipv4_host = 0x7f000001u;
    scan_ap_count = 0;
    spectranext_controller_post_message("FuseX: OK\n");
}

libspectrum_byte spectranext_controller_read(memory_page *page, libspectrum_word address)
{
    libspectrum_word offset = address & 0xfff;
    uint8_t *registers = (uint8_t *)&spectranext_controller;
    if (offset >= sizeof(spectranext_controller))
        return 0xff;
    return registers[offset];
}

void spectranext_controller_write(memory_page *page, libspectrum_word address, libspectrum_byte b)
{
    libspectrum_word offset = address & 0xfff;
    if (offset >= sizeof(spectranext_controller))
        return;

    uint8_t *registers = (uint8_t *)&spectranext_controller;
    const uint8_t old_command = registers[0];
    registers[offset] = b;

    if (offset == 0 && old_command == SPECTRANEXT_CMD_REG_IDLE && b != SPECTRANEXT_CMD_REG_IDLE)
    {
        spectranext_controller_process_command();
    }
}
