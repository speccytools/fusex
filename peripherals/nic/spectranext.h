#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPECTRANEXT_STATUS_IN_PROGRESS (0xFFu)
#define SPECTRANEXT_STATUS_SUCCESS (0u)
#define SPECTRANEXT_STATUS_ERROR (1u)

enum spectranext_cmd_t
{
    SPECTRANEXT_CMD_GET_CONTROLLER_STATUS = 0,
    SPECTRANEXT_CMD_WIFI_SCAN_ACCESS_POINTS = 1,
    SPECTRANEXT_CMD_WIFI_GET_ACCESS_POINT = 2,
    SPECTRANEXT_CMD_WIFI_CONNECT_ACCESS_POINT = 3,
    SPECTRANEXT_CMD_WIFI_DISCONNECT = 4,
    SPECTRANEXT_CMD_DNS_GETHOSTBYNAME = 5,
    SPECTRANEXT_CMD_ENGINECALL = 6,
    SPECTRANEXT_CMD_GET_MESSAGE = 7,
    SPECTRANEXT_CMD_XFS_READ = 8
};

#define WIFI_CONTROLLER_STATUS_OFFLINE (0u)
#define WIFI_CONTROLLER_STATUS_BUSY_UPDATING (1u)
#define WIFI_CONTROLLER_STATUS_OPERATIONAL (2u)

#define WIFI_SCAN_NONE (0)
#define WIFI_SCAN_SCANNING (1)
#define WIFI_SCAN_COMPLETE (2)
#define WIFI_SCAN_FAILURE (-1)

#define WIFI_CONNECT_DISCONNECTED (0)
#define WIFI_CONNECT_CONNECTING (1)
#define WIFI_CONNECT_CONNECT_SUCCESS (2)
#define WIFI_CONNECT_CONNECT_IP_OBTAINED (3)

#define GETHOSTBYNAME_STATUS_NONE (0)
#define GETHOSTBYNAME_STATUS_SUCCESS (1)
#define GETHOSTBYNAME_STATUS_HOST_NOT_FOUND (-1)
#define GETHOSTBYNAME_STATUS_TIMEOUT (-2)
#define GETHOSTBYNAME_STATUS_SYSTEM_FAILURE (-3)

#define SPECTRANEXT_CONTROLLER_PAGE 0x48

#define SPECTRANEXT_SCAN_AP_MAX 64

#define SPECTRANEXT_MESSAGE_MAX 128

/* Maximum XFS path supplied by the Z80, including its NUL terminator. */
#define SPECTRANEXT_XFS_READ_PATH_MAX 128

#define SPECTRANEXT_CMD_REG_IDLE 0xFFu

#pragma pack(push, 1)

typedef struct spectranext_get_status_out_s
{
    uint8_t controller_status;
    int8_t wifi_connection;
    uint32_t ipv4;
} spectranext_get_status_out_t;

_Static_assert(sizeof(spectranext_get_status_out_t) == 6u, "get_status payload size");
_Static_assert(offsetof(spectranext_get_status_out_t, controller_status) == 0u, "");
_Static_assert(offsetof(spectranext_get_status_out_t, wifi_connection) == 1u, "");
_Static_assert(offsetof(spectranext_get_status_out_t, ipv4) == 2u, "");

typedef union spectranext_workspace
{
    struct
    {
        spectranext_get_status_out_t out;
    } get_controller_status;

    struct
    {
        union
        {
            struct
            {
                char host[64];
            } in;
            struct
            {
                uint32_t ipv4;
            } out;
        } io;
    } dns;

    struct
    {
        union
        {
            struct
            {
                char ssid[64];
                char password[64];
            } in;
        } io;
    } wifi_connect;

    struct
    {
        union
        {
            struct
            {
                uint8_t ap_index;
            } in;
            struct
            {
                char ap_name[64];
            } out;
        } io;
    } wifi_get_ap;

    struct
    {
        union
        {
            struct
            {
                uint8_t scan_count;
            } out;
        } io;
    } wifi_scan;

    struct
    {
        struct
        {
            char input_file[128];
            char output_file[128];
            char operation[256];
        } io;
    } enginecall;

    struct
    {
        struct
        {
            char message[SPECTRANEXT_MESSAGE_MAX];
            uint8_t pending;
        } out;
    } get_message;

    /* XFS_READ input and output. */
    struct
    {
        struct
        {
            char source_filename[SPECTRANEXT_XFS_READ_PATH_MAX];
            uint32_t source_offset;
            uint8_t target_first_page;
            uint16_t target_first_page_offset;
            uint32_t maximum_data;
        } in;
        struct
        {
            uint32_t bytes_read;
        } out;
    } xfs_read;

    char page[4096 - 2];
} spectranext_workspace_t;

_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.source_filename) == 0u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.source_offset) == 128u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.target_first_page) == 132u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.target_first_page_offset) == 133u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.maximum_data) == 135u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.out.bytes_read) == 139u, "");

struct spectranext_controller_t
{
    uint8_t command;
    uint8_t status;
    spectranext_workspace_t workspace;
};

_Static_assert(sizeof(struct spectranext_controller_t) == 4096, "Controller is not of correct size");

#pragma pack(pop)

typedef struct spectranext_state_t
{
    uint8_t controller_status;
    int8_t connection_status;
    uint32_t ipv4_host;
} spectranext_state_t;

extern spectranext_state_t spectranext_state;

#define SNX_CTRL_DEBUG(...) ((void)0)

int spectranext_enginecall_dispatch(const char *input_file, const char *output_file, const char *operation);

typedef struct
{
    char input_file[128];
    char output_file[128];
    char operation[256];
    int result;
} spectranext_enginecall_args_t;

extern spectranext_enginecall_args_t spectranext_enginecall_args;
