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
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "engines/engine.h"
#include "controller_job.h"

#include "libspectrum.h"
#include "memory_pages.h"
#include "peripherals/fs/xfs.h"
#include "peripherals/fs/xfs_engines.h"
#include "peripherals/spectranet.h"
#include "ui/ui.h"
#include "utils.h"

#define SPECTRANEXT_RAM_PAGE_FIRST 0xC0u
#define SPECTRANEXT_RAM_PAGE_LAST 0xDFu
#define SPECTRANEXT_RAM_PAGE_COUNT (SPECTRANEXT_RAM_PAGE_LAST - SPECTRANEXT_RAM_PAGE_FIRST + 1u)

/* Worker-owned snapshots. Publication is confined to the emulation thread. */
struct controller_request {
    spectranext_workspace_t workspace;
    uint8_t command, status, channel, default_mount;
};
static struct controller_request requests[2];
static _Thread_local int worker_mount = -1;
static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static void execute_request(void *argument);
static void spectranext_controller_process_command(unsigned channel);


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

/* A completed response is immutable until the caller submits the next request. */
static void update_controller_jobs(void)
{
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (!controller_job_poll(channel)) continue;
        struct controller_request *request = &requests[channel];
        volatile spectranext_workspace_t *workspace = channel
            ? &spectranext_controller.workspace1 : &spectranext_controller.workspace;
        memcpy((void *)workspace, &request->workspace, sizeof(*workspace));
        if (channel) spectranext_controller.status1 = request->status;
        else spectranext_controller.status = request->status;
    }
}

/*
 * XFS_READ is controller-owned: mount the default RAM-over-ROMFS overlay for
 * this short-lived operation, resolving the caller's absolute path unchanged.
 */
static int16_t spectranext_xfs_read(struct controller_request *request, const char *path,
                                    const uint32_t source_offset,
                                    const uint8_t target_first_page,
                                    const uint16_t target_first_page_offset,
                                    const uint32_t maximum_data,
                                    uint32_t *const bytes_read_out)
{
    if (path == NULL || path[0] == '\0' || path[0] != '/' ||
        source_offset > INT32_MAX ||
        target_first_page_offset >= 0x1000u)
        return XFS_ERR_INVAL;

    const bool controller_destination = target_first_page == SPECTRANEXT_CONTROLLER_PAGE;
    if (controller_destination)
    {
        const unsigned base = request->channel ? 0xC00u : 0x800u;
        if (target_first_page_offset < base + 0x100u ||
            target_first_page_offset >= base + sizeof(request->workspace) ||
            maximum_data > base + sizeof(request->workspace) - target_first_page_offset)
            return XFS_ERR_INVAL;
    }
    else if (target_first_page >= SPECTRANEXT_RAM_PAGE_FIRST &&
             target_first_page <= SPECTRANEXT_RAM_PAGE_LAST)
    {
        const uint32_t destination_offset =
            (uint32_t)(target_first_page - SPECTRANEXT_RAM_PAGE_FIRST) * 0x1000u + target_first_page_offset;
        const uint32_t destination_capacity = SPECTRANEXT_RAM_PAGE_COUNT * 0x1000u - destination_offset;
        if (maximum_data > destination_capacity)
            return XFS_ERR_INVAL;
    }
    else
        return XFS_ERR_INVAL;

    *bytes_read_out = 0;

    struct xfs_engine_mount_t mount = { .engine = &xfs_overlay_engine };
    int16_t err = mount.engine->mount(mount.engine, "ram", "/", NULL, NULL, &mount);
    if (err != XFS_ERR_OK)
        return err;

    struct xfs_handle_t handle = { .type = XFS_HANDLE_TYPE_FILE };
    err = mount.engine->open(&mount, &handle, path, XFS_O_RDONLY);
    if (err == XFS_ERR_OK)
    {
        if (mount.engine->lseek(&mount, &handle, (int32_t)source_offset, XFS_SEEK_SET) < 0)
            err = XFS_ERR_IO;

        uint8_t page = target_first_page;
        uint16_t page_offset = target_first_page_offset;
        uint32_t remaining = maximum_data;
        while (err == XFS_ERR_OK && remaining != 0)
        {
            uint8_t *const destination = controller_destination
                ? (uint8_t *)&request->workspace
                : spectranet_ram_page(page);
            if (destination == NULL)
            {
                err = XFS_ERR_IO;
                break;
            }
            const uint32_t chunk_size = remaining < 0x1000u - page_offset
                ? remaining : 0x1000u - page_offset;
            const int32_t read_result = mount.engine->read(&mount, &handle, controller_destination ? (uint8_t *)&request->workspace + page_offset - (request->channel ? 0xC00u : 0x800u) : destination + page_offset, chunk_size);
            if (read_result < 0)
            {
                err = (int16_t)read_result;
                break;
            }
            *bytes_read_out += (uint32_t)read_result;
            if ((uint32_t)read_result < chunk_size)
                break;
            remaining -= (uint32_t)read_result;
            ++page;
            page_offset = 0;
        }

        const int16_t close_err = mount.engine->close(&mount, &handle);
        if (err == XFS_ERR_OK && close_err != XFS_ERR_OK)
            err = close_err;
        mount.engine->free_handle(&mount, &handle);
    }

    mount.engine->unmount(mount.engine, &mount);
    return err;
}

static int enginecall_dispatch(const char *input_file, const char *output_file, const char *operation)
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
    if (strcmp(argv[0], "cp") == 0)
        return engine_cp_call(input_file, output_file, argc, argv);
    if (strcmp(argv[0], "lz4") == 0)
        return engine_lz4_call(input_file, output_file, argc, argv);
    if (strcmp(argv[0], "rm") == 0)
        return engine_rm_call(input_file, output_file, argc, argv);
    return -1;
}

int spectranext_controller_default_mount(void)
{
    if (worker_mount >= 0) return worker_mount;
    const uint8_t *ram = spectranet_ram_page(0xC0);
    return ram ? ram[0xF6F] : -1;
}

int spectranext_enginecall_dispatch(const char *input, const char *output, const char *operation)
{
    pthread_mutex_lock(&engine_lock);
    int result = enginecall_dispatch(input, output, operation);
    pthread_mutex_unlock(&engine_lock);
    return result;
}

static void spectranext_set_status(struct controller_request *request, uint8_t status)
{
    request->status = status;
}

bool spectranext_controller_post_message_bytes(const uint8_t *message, size_t length)
{
    if (message == NULL && length != 0u)
        return false;

    if (length >= SPECTRANEXT_MESSAGE_MAX)
        length = SPECTRANEXT_MESSAGE_MAX - 1u;

    pthread_mutex_lock(&state_lock);
    if (length != 0u)
        memcpy(pending_message, message, length);
    pending_message[length] = '\0';
    message_pending = true;
    pthread_mutex_unlock(&state_lock);

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

static void spectranext_controller_get_message(struct controller_request *request)
{
    memset((void *)&request->workspace.get_message.out, 0,
           sizeof(request->workspace.get_message.out));

    if (message_pending)
    {
        memcpy((void *)request->workspace.get_message.out.message, pending_message,
               sizeof(request->workspace.get_message.out.message));
        request->workspace.get_message.out.pending = 1u;
        spectranext_controller_clear_messages();
    }

    spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
}

static void execute_request(void *argument)
{
    struct controller_request *request = argument;
    const uint8_t cmd = request->command;
    /* Network status and message operations share state; engines/XFS_READ do not. */
    const bool state_command = cmd != SPECTRANEXT_CMD_ENGINECALL && cmd != SPECTRANEXT_CMD_XFS_READ;
    if (state_command) pthread_mutex_lock(&state_lock);

    switch (cmd)
    {
        /* The shared controller ROM serves GET_VERSION (15) directly. */
        case SPECTRANEXT_CMD_GET_CONTROLLER_STATUS:
            request->workspace.get_controller_status.out.controller_status =
                spectranext_state.controller_status;
            request->workspace.get_controller_status.out.wifi_connection =
                spectranext_state.connection_status;
            request->workspace.get_controller_status.out.ipv4 = spectranext_state.ipv4_host;
            spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_WIFI_SCAN_ACCESS_POINTS:
            scan_ap_count = 1;
            strncpy(scan_ap_names[0], "spectranext", sizeof(scan_ap_names[0]) - 1u);
            scan_ap_names[0][sizeof(scan_ap_names[0]) - 1u] = '\0';
            request->workspace.wifi_scan.io.out.scan_count =
                (uint8_t)(scan_ap_count > 255u ? 255u : scan_ap_count);
            spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_WIFI_GET_ACCESS_POINT:
        {
            const uint8_t idx = request->workspace.wifi_get_ap.io.in.ap_index;
            if ((uint16_t)idx >= (uint16_t)scan_ap_count)
            {
                spectranext_set_status(request, SPECTRANEXT_STATUS_ERROR);
                break;
            }
            strncpy((char *)request->workspace.wifi_get_ap.io.out.ap_name, scan_ap_names[idx],
                    sizeof(request->workspace.wifi_get_ap.io.out.ap_name) - 1u);
            request->workspace.wifi_get_ap.io.out.ap_name
                [sizeof(request->workspace.wifi_get_ap.io.out.ap_name) - 1u] = '\0';
            spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
            break;
        }

        case SPECTRANEXT_CMD_WIFI_CONNECT_ACCESS_POINT:
            spectranext_state.connection_status = WIFI_CONNECT_CONNECT_SUCCESS;
            spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_WIFI_DISCONNECT:
            spectranext_state.connection_status = WIFI_CONNECT_DISCONNECTED;
            spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
            break;

        case SPECTRANEXT_CMD_DNS_GETHOSTBYNAME:
        {
            char host[64];
            memcpy(host, (const void *)request->workspace.dns.io.in.host, 63);
            host[63] = '\0';

            if (host[0] == '\0')
            {
                request->workspace.dns.io.out.ipv4 = 0;
                spectranext_set_status(request, SPECTRANEXT_STATUS_ERROR);
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
                request->workspace.dns.io.out.ipv4 = 0;
                spectranext_set_status(request, SPECTRANEXT_STATUS_ERROR);
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
                request->workspace.dns.io.out.ipv4 = 0;
                spectranext_set_status(request, SPECTRANEXT_STATUS_ERROR);
                break;
            }

            request->workspace.dns.io.out.ipv4 = ipv4_host;
            spectranext_state.ipv4_host = ipv4_host;
            spectranext_set_status(request, SPECTRANEXT_STATUS_SUCCESS);
            break;
        }

        case SPECTRANEXT_CMD_ENGINECALL:
        {
            request->workspace.enginecall.io.input_file[127] = '\0';
            request->workspace.enginecall.io.output_file[127] = '\0';
            request->workspace.enginecall.io.operation[255] = '\0';
            worker_mount = request->default_mount;
            int result = spectranext_enginecall_dispatch(request->workspace.enginecall.io.input_file,
                request->workspace.enginecall.io.output_file,
                request->workspace.enginecall.io.operation);
            worker_mount = -1;
            request->workspace.enginecall.io.result = (int8_t)result;
            spectranext_set_status(request, result == 0 ? SPECTRANEXT_STATUS_SUCCESS : SPECTRANEXT_STATUS_ERROR);
            break;
        }

        case SPECTRANEXT_CMD_GET_MESSAGE:
            spectranext_controller_get_message(request);
            break;

        case SPECTRANEXT_CMD_XFS_READ:
        {
            char path[SPECTRANEXT_XFS_READ_PATH_MAX];
            memcpy(path, (const void *)request->workspace.xfs_read.in.source_filename,
                   sizeof(path));
            if (memchr(path, '\0', sizeof(path)) == NULL)
            {
                spectranext_set_status(request, SPECTRANEXT_STATUS_ERROR);
                break;
            }

            uint32_t bytes_read = 0;
            const int16_t result = spectranext_xfs_read(
                request, path,
                request->workspace.xfs_read.in.source_offset,
                request->workspace.xfs_read.in.target_first_page,
                request->workspace.xfs_read.in.target_first_page_offset,
                request->workspace.xfs_read.in.maximum_data,
                &bytes_read);
            request->workspace.xfs_read.out.bytes_read = bytes_read;
            spectranext_set_status(request,
                result == XFS_ERR_OK ? SPECTRANEXT_STATUS_SUCCESS : SPECTRANEXT_STATUS_ERROR);
            break;
        }

        default:
            spectranext_set_status(request, SPECTRANEXT_STATUS_ERROR);
            break;
    }
    if (state_command) pthread_mutex_unlock(&state_lock);
}

void spectranext_controller_init(void)
{
    controller_job_cancel_and_wait();
    controller_job_poll(0);
    controller_job_poll(1);
    utils_file controller_binary = { 0 };

    memset((void *)&spectranext_controller, 0, sizeof(spectranext_controller));
    if (utils_read_auxiliary_file("spxcontroller.bin", &controller_binary,
                                  UTILS_AUXILIARY_ROM) != 0)
    {
        ui_error(UI_ERROR_ERROR, "couldn't find Spectranext controller binary ('spxcontroller.bin')");
    }
    else if (controller_binary.length > sizeof(spectranext_controller.code))
    {
        ui_error(UI_ERROR_ERROR, "Spectranext controller binary is too large (%lu bytes)",
                 (unsigned long)controller_binary.length);
        utils_close_file(&controller_binary);
    }
    else
    {
        memcpy((void *)spectranext_controller.code, controller_binary.buffer,
               controller_binary.length);
        utils_close_file(&controller_binary);
    }

    spectranext_controller.command = spectranext_controller.command1 = SPECTRANEXT_CMD_REG_IDLE;
    spectranext_controller.status = spectranext_controller.status1 = SPECTRANEXT_STATUS_SUCCESS;
    spectranext_state.controller_status = WIFI_CONTROLLER_STATUS_OPERATIONAL;
    spectranext_state.connection_status = WIFI_CONNECT_CONNECT_IP_OBTAINED;
    spectranext_state.ipv4_host = 0x7f000001u;
    scan_ap_count = 0;
    spectranext_controller_post_message("FuseX: OK\n");
}

libspectrum_byte spectranext_controller_read(memory_page *page, libspectrum_word address)
{
    update_controller_jobs();
    libspectrum_word offset = address & 0xfff;
    uint8_t *registers = (uint8_t *)&spectranext_controller;
    if (offset >= sizeof(spectranext_controller))
        return 0xff;
    return registers[offset];
}

static void spectranext_controller_process_command(unsigned channel)
{
    struct controller_request *request = &requests[channel];
    volatile spectranext_workspace_t *workspace = channel
        ? &spectranext_controller.workspace1 : &spectranext_controller.workspace;
    memcpy(&request->workspace, (const void *)workspace, sizeof(*workspace));
    request->channel = channel;
    request->command = channel ? spectranext_controller.command1 : spectranext_controller.command;
    request->status = SPECTRANEXT_STATUS_IN_PROGRESS;
    const uint8_t *ram = spectranet_ram_page(0xC0);
    request->default_mount = ram ? ram[0xF6F] : 0;
    if (channel) {
        spectranext_controller.command1 = SPECTRANEXT_CMD_REG_IDLE;
        spectranext_controller.status1 = SPECTRANEXT_STATUS_IN_PROGRESS;
    } else {
        spectranext_controller.command = SPECTRANEXT_CMD_REG_IDLE;
        spectranext_controller.status = SPECTRANEXT_STATUS_IN_PROGRESS;
    }
    if (controller_job_start(channel, execute_request, request) != 0) {
        if (channel) spectranext_controller.status1 = SPECTRANEXT_STATUS_ERROR;
        else spectranext_controller.status = SPECTRANEXT_STATUS_ERROR;
    }
}

void spectranext_controller_write(memory_page *page, libspectrum_word address, libspectrum_byte b)
{
    update_controller_jobs();
    const unsigned offset = address & 0xfff;
    if (offset < SPECTRANEXT_CONTROLLER_CODE_SIZE) return;
    /* A submitted workspace belongs to its worker until status is published. */
    const unsigned channel = offset >= 0xC00 && offset < 0xFFE;
    if (controller_job_is_running(channel)) return;
    uint8_t *registers = (uint8_t *)&spectranext_controller;
    const uint8_t old_value = registers[offset];
    registers[offset] = b;
    if ((offset == 0xFFE || offset == 0xFFC) &&
        old_value == SPECTRANEXT_CMD_REG_IDLE && b != SPECTRANEXT_CMD_REG_IDLE)
        spectranext_controller_process_command(offset == 0xFFC);
}
