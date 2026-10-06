/* Controller ABI snapshot from Spectranext common/spectranext_controller_layout.h.
 * Keep this copy in sync when updating the bundled controller ROM.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Controller page $48 maps at $2000. All offsets below are page-relative.
 * Channel 0 is the existing blocking SDK/ROM ABI. Channel 1 is raw access only.
 * Both use identical payload layouts and support the same commands.
 * Own a workspace until status is no longer FF; read its response before reuse.
 * Submit by writing status FF, then command. Firmware consumes command to FF,
 * executes, writes the response, then publishes status 0 (success) or 1 (error).
 * Each payload has 1020 bytes; $2BFC-$2BFF are reserved so both payload
 * types are identical. The last four page bytes contain the two register pairs.
 * Never submit or modify a busy channel. No mode switch is required.
 */
#define SPECTRANEXT_CONTROLLER_PAGE 0x48
#define SPECTRANEXT_CONTROLLER_PAGE_SIZE 0x1000u
#define SPECTRANEXT_CONTROLLER_CODE_SIZE 0x800u
#define SPECTRANEXT_CONTROLLER_WORKSPACE_OFFSET 0x800u
#define SPECTRANEXT_CONTROLLER_WORKSPACE_SIZE 0x3FCu
#define SPECTRANEXT_CONTROLLER_COMMAND_OFFSET 0xFFEu
#define SPECTRANEXT_CONTROLLER_STATUS_OFFSET 0xFFFu
#define SPECTRANEXT_CONTROLLER_CHANNEL1_WORKSPACE_OFFSET 0xC00u
#define SPECTRANEXT_CONTROLLER_CHANNEL1_COMMAND_OFFSET 0xFFCu
#define SPECTRANEXT_CONTROLLER_CHANNEL1_STATUS_OFFSET 0xFFDu
/* XFS_READ may target only its own workspace, after the first 256 bytes.
 * Ordinary Spectranet RAM destinations can still span consecutive 4 KB pages.
 */
#define SPECTRANEXT_CONTROLLER_XFS_READ_BUFFER_OFFSET 0x900u
#define SPECTRANEXT_SCAN_AP_MAX 64
#define SPECTRANEXT_MESSAGE_MAX 128
#define SPECTRANEXT_CMD_REG_IDLE 0xFFu
#ifndef SPECTRANEXT_XFS_READ_PATH_MAX
#define SPECTRANEXT_XFS_READ_PATH_MAX 128
#endif

#pragma pack(push, 1)

/** GET_CONTROLLER_STATUS output only — offsets must match Z80 WS_* in spectranext.asm. */
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
                uint8_t bssid[6];
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
                uint8_t bssid[6];
                int8_t rssi;
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
            int8_t result;
        } io;
    } enginecall;

    /** GET_MESSAGE output — null-terminated message copied to Z80 from workspace staging. */
    struct
    {
        struct
        {
            char message[SPECTRANEXT_MESSAGE_MAX];
            char pending;
        } out;
    } get_message;

    /** XFS_READ input and output. */
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

    char page[SPECTRANEXT_CONTROLLER_WORKSPACE_SIZE];
} spectranext_workspace_t;

_Static_assert(offsetof(spectranext_workspace_t, wifi_connect.io.in.bssid) == 128u, "");
_Static_assert(offsetof(spectranext_workspace_t, wifi_get_ap.io.out.bssid) == 64u, "");
_Static_assert(offsetof(spectranext_workspace_t, wifi_get_ap.io.out.rssi) == 70u, "");

_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.source_filename) == 0u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.source_offset) == 128u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.target_first_page) == 132u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.target_first_page_offset) == 133u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.in.maximum_data) == 135u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.out.bytes_read) == 139u, "");
_Static_assert(offsetof(spectranext_workspace_t, xfs_read.out.bytes_read) +
               sizeof(((spectranext_workspace_t *)0)->xfs_read.out.bytes_read) <=
               SPECTRANEXT_CONTROLLER_XFS_READ_BUFFER_OFFSET - SPECTRANEXT_CONTROLLER_CODE_SIZE,
               "XFS_READ data must not overlap its result");

struct spectranext_controller_t
{
    uint8_t code[SPECTRANEXT_CONTROLLER_CODE_SIZE];
    spectranext_workspace_t workspace;
    uint8_t reserved[4];
    spectranext_workspace_t workspace1;
    uint8_t command1;
    uint8_t status1;
    uint8_t command;
    uint8_t status;
};

_Static_assert(sizeof(struct spectranext_controller_t) == 4096, "Controller is not of correct size");
_Static_assert(offsetof(struct spectranext_controller_t, workspace) == 0x800u, "Workspace offset");
_Static_assert(offsetof(struct spectranext_controller_t, command) == 0xFFEu, "Command offset");
_Static_assert(offsetof(struct spectranext_controller_t, status) == 0xFFFu, "Status offset");


_Static_assert(offsetof(struct spectranext_controller_t, workspace1) == 0xC00u, "Channel 1 workspace");
_Static_assert(offsetof(struct spectranext_controller_t, command1) == 0xFFCu, "Channel 1 command");
_Static_assert(offsetof(struct spectranext_controller_t, status1) == 0xFFDu, "Channel 1 status");

#pragma pack(pop)
