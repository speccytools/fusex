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
    SPECTRANEXT_CMD_XFS_READ = 8,
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

#include "spectranext_controller_layout.h"

typedef struct spectranext_state_t
{
    uint8_t controller_status;
    int8_t connection_status;
    uint32_t ipv4_host;
} spectranext_state_t;

extern spectranext_state_t spectranext_state;

#define SNX_CTRL_DEBUG(...) ((void)0)

int spectranext_controller_default_mount(void);

int spectranext_enginecall_dispatch(const char *input_file, const char *output_file, const char *operation);

typedef struct
{
    char input_file[128];
    char output_file[128];
    char operation[256];
    int result;
} spectranext_enginecall_args_t;
