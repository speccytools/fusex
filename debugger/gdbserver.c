
#include "config.h"

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#endif

#include "debugger.h"
#include "gdbserver.h"
#include "gdbserver_remote_commands.h"
#include "packets.h"
#include "arch.h"
#include "gdbserver_utils.h"

#include "debugger_internals.h"
#include "event.h"
#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "memory.h"
#include "mempool.h"
#include "periph.h"
#include "machine.h"
#include "peripherals/spectranet.h"
#include "settings.h"
#include "utils.h"
#include "ui/ui.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"
#ifdef BUILD_SPECTRANET
#include "vfile.h"
#endif

#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include "compat.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <semaphore.h>

static int gdbserver_socket;
int gdbserver_client_socket = -1;  // Made non-static so packets.c can access it
uint8_t tmpbuf[0x20000];  // Made non-static so vfile_ext.c can access it
static volatile char gdbserver_trapped = 0;
static int gdbserver_port = 0;
static int gdbserver_requested_port = 0;
static int gdbserver_last_trap_reason = DEBUG_TRAP_REASON_SIGNAL_RECEIVED;
static volatile int gdbserver_emulating = 0;
static pthread_mutex_t emulating_mutex;
static pthread_cond_t emulating_cond;

void gdbserver_note_emulating(void)
{
    if (gdbserver_emulating) return;
    pthread_mutex_lock(&emulating_mutex);
    gdbserver_emulating = 1;
    pthread_cond_broadcast(&emulating_cond);
    pthread_mutex_unlock(&emulating_mutex);
}

static pthread_t network_thread_id;
/** Set while tearing the listener down; unblocks gdbserver_execute_on_main_thread and gdbserver_activate_with_reason. */
static volatile int gdbserver_shutting_down;
static trapped_action_t scheduled_action = NULL;
static const void* scheduled_action_data = NULL;
static void* scheduled_action_response = NULL;
static uint8_t scheduled_action_response_delivered = 0;

static pthread_cond_t trapped_cond;
static pthread_cond_t response_cond;
static pthread_mutex_t trap_process_mutex;
static pthread_mutex_t network_read_mutex;
static pthread_mutex_t network_write_mutex;
static int gdbserver_reset_event = -1;

enum spectranext_autoboot
{
  autoboot_nothing = 0,
  autoboot_force_set_once = 1,
  autoboot_force_reset_once
};

/** vSpectranext autoboot: if true at machine_reset, apply ram mount + autoboot once, then clear. */
static enum spectranext_autoboot spectranext_autoboot = autoboot_nothing;

static libspectrum_word* registers[] = {
    &AF,
    &BC,
    &DE,
    &HL,
    &SP,
    &PC,
    &IX,
    &IY,
    &AF_,
    &BC_,
    &DE_,
    &HL_,
    &CLOCKL,
    &CLOCKH
};

static uint8_t gdbserver_detrap();
static void gdbserver_send_stop_reply(int trap_reason);

void gdbserver_lock_network_write(void)
{
    pthread_mutex_lock(&network_write_mutex);
}

void gdbserver_unlock_network_write(void)
{
    pthread_mutex_unlock(&network_write_mutex);
}

static void gdbserver_send_stop_reply(int trap_reason)
{
    char tbuf[64];
    int signal;

    switch (trap_reason)
    {
        case DEBUG_TRAP_REASON_BREAKPOINT:
            signal = 5;  /* SIGTRAP */
            break;
        case DEBUG_TRAP_REASON_SIGNAL_RECEIVED:
        case DEBUG_TRAP_REASON_OTHER:
        default:
            signal = 2;  /* SIGINT */
            break;
    }

    sprintf(tbuf, "T%02xthread:p%02x.%02x;", signal, 1, 1);
    packet_send_message((const uint8_t*)tbuf, strlen(tbuf));
}

static uint8_t action_get_registers(const void* arg, void* response);
static uint8_t action_set_registers(const void* arg, void* response);
static uint8_t action_get_mem(const void* arg, void* response);
static uint8_t action_set_mem(const void* arg, void* response);
static uint8_t action_get_register(const void* arg, void* response);
static uint8_t action_set_register(const void* arg, void* response);
static uint8_t action_set_breakpoint(const void* arg, void* response);
static uint8_t action_remove_breakpoint(const void* arg, void* response);
static uint8_t action_step_instruction(const void* arg, void* response);

struct action_mem_args_t {
    size_t maddr, mlen;
    uint8_t* payload;
};

struct action_step_args_t {
    size_t addr, len;
};

struct action_register_args_t {
    int reg;
    libspectrum_word value;
};

struct action_set_registers_args_t {
    libspectrum_word regs_data[sizeof(registers) / sizeof(libspectrum_word*)];
};

struct action_breakpoint_args_t {
    size_t maddr, mlen;
};

void gdbserver_send_remote_console_output(const char *text)
{
    size_t len = strlen(text);

    if (gdbserver_client_socket == -1 || !gdbserver_debugging_enabled)
        return;

    if (1 + len * 2 >= sizeof(tmpbuf))
        len = (sizeof(tmpbuf) - 1) / 2;

    tmpbuf[0] = 'O';
    mem2hex((const uint8_t *)text, (char *)&tmpbuf[1], (int)len);
    packet_send_message(tmpbuf, 1 + len * 2);
}

static int decode_remote_command(const char *hex_command, char *command, size_t command_size)
{
    size_t hex_len = strlen(hex_command);

    if ((hex_len & 1) != 0)
        return 0;

    if (hex_len / 2 >= command_size)
        return 0;

    hex2mem(hex_command, (uint8_t *)command, (int)(hex_len / 2));
    command[hex_len / 2] = '\0';
    return 1;
}

static void process_remote_command(const char *hex_command)
{
    const struct remote_command_entry_t *entry;
    char command[256];

    if (!decode_remote_command(hex_command, command, sizeof(command))) {
        packet_send_message((const uint8_t *)"E01", 3);
        return;
    }

    for (entry = remote_commands; entry->name; entry++) {
        size_t name_len = strlen(entry->name);
        if (!strncmp(command, entry->name, name_len) &&
            (command[name_len] == '\0' || command[name_len] == ' ' || command[name_len] == '\t')) {
            const char *args = command + name_len;
            while (*args == ' ' || *args == '\t') args++;
            if (entry->handler(args))
                packet_send_message((const uint8_t *)"E01", 3);
            else
                packet_send_message((const uint8_t *)"OK", 2);
            return;
        }
    }

    packet_send_message((const uint8_t *)"E04", 3);
}

static void gdbserver_reset_event_fn(libspectrum_dword event_time, int type,
                                     void *user_data)
{
    (void)event_time;
    (void)type;
    (void)user_data;

    machine_reset(0);
}

uint8_t gdbserver_reset_via_remote_command(void)
{
    gdbserver_schedule_reset();
    return 0;
}

static void process_xfer(const char *name, char *args)
{
  const char *mode = args;
  args = strchr(args, ':');
  if( !args ) {
    packet_send_message((const uint8_t*)"", 0);
    return;
  }
  *args++ = '\0';
  
  if (!strcmp(name, "features") && !strcmp(mode, "read")) {
      packet_send_message((const uint8_t*)FEATURE_STR, strlen(FEATURE_STR));
  }
  if (!strcmp(name, "exec-file") && !strcmp(mode, "read")) {
      packet_send_message((const uint8_t*)"/fuse/emulated", strlen("/fuse/emulated"));
  }
}

static void process_query(char *payload)
{
    const char *name;
    char *args;

    if (payload == strstr(payload, "Rcmd,")) {
        process_remote_command(payload + 5);
        return;
    }

    args = strchr(payload, ':');
    if (args)
        *args++ = '\0';
    name = payload;
    if (!strcmp(name, "C"))
    {
        snprintf((char*)tmpbuf, sizeof(tmpbuf), "QCp%02x.%02x", 1, 1);
        packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
    }
    if (!strcmp(name, "Attached"))
    {
        packet_send_message((const uint8_t*)"1", 1);
    }
    if (!strcmp(name, "Offsets"))
        packet_send_message((const uint8_t*)"", 0);
    if (!strcmp(name, "Supported"))
        packet_send_message((const uint8_t*)"PacketSize=4000;qXfer:features:read+;qXfer:auxv:read+;vSpectranext+", strlen("PacketSize=4000;qXfer:features:read+;qXfer:auxv:read+;vSpectranext+"));
    if (!strcmp(name, "Symbol"))
        packet_send_message((const uint8_t*)"OK", 2);
    if (name == strstr(name, "ThreadExtraInfo"))
    {
        args = payload;
        args = 1 + strchr(args, ',');
        packet_send_message((const uint8_t*)"41414141", 8);
    }
    if (!strcmp(name, "TStatus"))
        packet_send_message((const uint8_t*)"", 0);
    if (!strcmp(name, "Xfer"))
    {
        name = args;
        args = strchr(args, ':');
        *args++ = '\0';
        return process_xfer(name, args);
    }
    if (!strcmp(name, "fThreadInfo"))
    {
        packet_send_message((const uint8_t*)"mp01.01", 7);
    }
    if (!strcmp(name, "sThreadInfo"))
        packet_send_message((const uint8_t*)"l", 1);
}

static void process_vpacket(char *payload)
{
    const char *name;
    char *args;
    size_t args_len = 0;
    args = strchr(payload, ';');
    if (args)
    {
        *args++ = '\0';
        args_len = strlen(args);  // Calculate length after null-terminating
    }
    name = payload;

    // vFile and vSpectranext are provided by the Spectranet XFS implementation.
#ifdef BUILD_SPECTRANET
    if (strncmp(name, "File:", 5) == 0 || strncmp(name, "Spectranext:", 12) == 0)
    {
        const char* response = vfile_handle_v(name, args ? args : "", (uint32_t)args_len);
        if (response)
        {
            packet_send_message((const uint8_t*)response, strlen(response));
        }
        return;
    }
#endif

    if (!strcmp("Cont", name))
    {
        if (args && args[0] == 'c')
        {
            if (gdbserver_detrap())
            {
                packet_send_message((const uint8_t*)"OK", 2);
            }
            else
            {
                packet_send_message((const uint8_t*)"E01", 3);
            }
        }
        if (args && args[0] == 's')
        {
            if (gdbserver_trapped)
            {
                gdbserver_execute_on_main_thread(action_step_instruction, NULL, NULL);
                gdbserver_detrap();
            }
        }
    }
    if (!strcmp("Cont?", name))
      packet_send_message((const uint8_t*)"vCont;c;C;s;S;", strlen("vCont;c;C;s;S;"));
    if (!strcmp("Kill", name))
    {
      packet_send_message((const uint8_t*)"OK", 2);
    }
    if (!strcmp("MustReplyEmpty", name))
      packet_send_message((const uint8_t*)"", 0);
}

static int set_register_value(int reg, libspectrum_word value)
{
    if (reg >= (sizeof(registers) / (sizeof(libspectrum_word*))))
    {
        return 1;
    }
    *registers[reg] = value;
    return 0;
}

static int get_register_value(int reg, libspectrum_word* result)
{
    if (reg >= (sizeof(registers) / (sizeof(libspectrum_word*))))
    {
        return 1;
    }
    *result = *registers[reg];
    return 0;
}

uint8_t process_packet()
{
    // Process the packet from packets subsystem
    size_t packet_len = packets_get_packet_len();
    if (packet_len == 0)
    {
        return 0;  // No packet to process
    }
    
    const uint8_t *packet_buf = packets_get_packet();
    char *recv_data = (char *)packet_buf;
    char request = recv_data[0];
    char *payload = (char *)&recv_data[1];

    switch (request)
    {
        case 'c':
        {
            gdbserver_detrap();
            break;
        }
        case 'D':
        {
            if (gdbserver_detrap())
            {
                packet_send_message((const uint8_t*)"OK", 2);
            }
            else
            {
                packet_send_message((const uint8_t*)"", 0);
            }
            break;
        }
        case 'g':
        {
            if (gdbserver_execute_on_main_thread(action_get_registers, NULL, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
            break;
        }
        case 'G':
        {
            struct action_set_registers_args_t r = {};
            if (strlen(payload) != ((sizeof(registers) / sizeof(libspectrum_word*)) * 4))
            {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
            hex2mem(payload, (void *)&r.regs_data, (sizeof(registers) / sizeof(libspectrum_word*)) * 2);
          
            if (gdbserver_execute_on_main_thread(action_set_registers, &r, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
            break;
        }
        case 'H':
        {
            packet_send_message((const uint8_t*)"OK", 2);
            break;
        }
        case 'm':
        {
            struct action_mem_args_t mem;
            if (sscanf(payload, "%zx,%zx", &mem.maddr, &mem.mlen) != 2) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
            if (mem.mlen * SZ * 2 > 0x20000) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
          
            if (gdbserver_execute_on_main_thread(action_get_mem, &mem, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
            break;
        }
        case 'M':
        {
            struct action_mem_args_t mem;
            int offset;
            if (sscanf(payload, "%zx,%zx:%n", &mem.maddr, &mem.mlen, &offset) != 2) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
            mem.payload = (uint8_t*)(payload + offset);
            if (gdbserver_execute_on_main_thread(action_set_mem, &mem, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
            break;
        }
        case 'p':
        {
            struct action_register_args_t r;
            r.reg = strtol(payload, NULL, 16);
            if (gdbserver_execute_on_main_thread(action_get_register, &r, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
            break;
        }
        case 'P':
        {
            struct action_register_args_t r;
            r.reg = strtol(payload, NULL, 16);
            if ('=' != *payload++) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
          
            hex2mem(payload, (void *)&r.value, SZ * 2);
          
            if (gdbserver_execute_on_main_thread(action_set_register, &r, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
          
            break;
        }
        case 'q':
        {
            process_query(payload);
            break;
        }
        case 's':
        {
            gdbserver_execute_on_main_thread(action_step_instruction, NULL, NULL);
            gdbserver_detrap();
            break;
        }
        case 'i':
        {
            if (gdbserver_trapped)
            {
                size_t a, b;
              
                if (sscanf(payload, "%zx,%zx", &a, &b) == 2)
                {
                    struct action_step_args_t ar;
                    ar.addr = a;
                    ar.len = b;
                    gdbserver_execute_on_main_thread(action_step_instruction, &ar, NULL);
                    gdbserver_detrap();
                }
                else if (sscanf(payload, "%zx", &a) == 1)
                {
                    struct action_step_args_t ar;
                    ar.addr = 0;
                    ar.len = a;
                    gdbserver_execute_on_main_thread(action_step_instruction, &ar, NULL);
                    gdbserver_detrap();
                }
                else
                {
                    gdbserver_execute_on_main_thread(action_step_instruction, NULL, NULL);
                    gdbserver_detrap();
                }
            }
            else
            {
                packet_send_message((const uint8_t*)"E01", 3);
            }
          
            break;
        }
        case 'v':
        {
            process_vpacket(payload);
            break;
        }
        case 'X':
        {
            size_t maddr, mlen;
            int offset, new_len;
            if (sscanf(payload, "%zx,%zx:%n", &maddr, &mlen, &offset) != 2) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
            payload += offset;
            // Calculate available length: from payload to end of packet buffer
            size_t packet_len = packets_get_packet_len();
            size_t available_len = packet_len - (payload - (char*)packets_get_packet());
            new_len = unescape(payload, available_len);
            if (new_len != mlen) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
          
            struct action_mem_args_t mem;
            mem.payload = (uint8_t*)payload;
            mem.maddr = maddr;
            mem.mlen = mlen;
          
            if (gdbserver_execute_on_main_thread(action_set_mem, &mem, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
            break;
        }
        case 'Z':
        {
            size_t type, addr, length;
            if (sscanf(payload, "%zx,%zx,%zx", &type, &addr, &length) != 3) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
          
            struct action_breakpoint_args_t b;
            b.maddr = addr;
          
            if (gdbserver_execute_on_main_thread(action_set_breakpoint, &b, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
        
            break;
        }
        case 'z':
        {
            size_t type, addr, length;
            if (sscanf(payload, "%zx,%zx,%zx", &type, &addr, &length) != 3) {
                packet_send_message((const uint8_t*)"E01", 3);
                break;
            }
          
            struct action_breakpoint_args_t b;
            b.maddr = addr;
          
            if (gdbserver_execute_on_main_thread(action_remove_breakpoint, &b, tmpbuf))
                packet_send_message((const uint8_t*)tmpbuf, strlen((const char*)tmpbuf));
        
            break;
        }
        case '?':
        {
            if (gdbserver_trapped)
            {
                gdbserver_send_stop_reply(gdbserver_last_trap_reason);
            }
            else
            {
                debugger_mode = DEBUGGER_MODE_HALTED;
            }
            break;
        }
        default:
        {
            packet_send_message((const uint8_t*)"", 0);
        }
    }
  
    return 0;
}


static int process_network(int socket)
{
    pthread_mutex_lock(&network_read_mutex);
    
    // Read raw bytes from socket
    int bytes_read = packets_read_socket(socket);
    if (bytes_read < 0)
    {
        pthread_mutex_unlock(&network_read_mutex);
        return -1;  // Error or EOF
    }
    if (bytes_read == 0)
    {
        pthread_mutex_unlock(&network_read_mutex);
        return 0;  // No data available
    }
    
    // Get pointer to incoming raw buffer
    const uint8_t *inbuf = packets_get_incoming_raw();
    size_t inbuf_size = packets_get_incoming_raw_len();
    
    // Feed all bytes to the deframer byte-by-byte
    for (size_t i = 0; i < inbuf_size; i++)
    {
        packets_feed_result_t result = packets_feed_byte(inbuf[i]);
        
        if (result == PACKETS_FEED_COMPLETE)
        {
            // Packet complete - ACK already sent by packets_feed_byte()
            // Process the packet
            process_packet();
            packets_reset();
            
            // Clear consumed bytes
            packets_clear_incoming_raw();
            pthread_mutex_unlock(&network_read_mutex);
            return 0;
        }
        else if (result == PACKETS_FEED_INTERRUPT)
        {
            // Interrupt character received in idle state
            // Deframer already reset itself, just trigger interrupt and continue processing
            debugger_mode = DEBUGGER_MODE_HALTED;
            // Continue processing remaining bytes in the buffer
        }
        // PACKETS_FEED_CONSUMED means byte consumed by deframer, continue
        // PACKETS_FEED_NOT_CONSUMED means not consumed (noise) - ignore
    }
    
    // All bytes consumed but no complete packet yet
    packets_clear_incoming_raw();
    pthread_mutex_unlock(&network_read_mutex);
  
    return 0;
}

static void* network_thread(void* arg)
{
    pthread_mutex_lock(&emulating_mutex);
    while (!gdbserver_emulating && !gdbserver_shutting_down &&
           gdbserver_debugging_enabled)
    {
        pthread_cond_wait(&emulating_cond, &emulating_mutex);
    }
    pthread_mutex_unlock(&emulating_mutex);

    while (gdbserver_debugging_enabled)
    {
        {
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 500000;

            pthread_mutex_lock(&network_read_mutex);
            
            if (gdbserver_socket < 0)
            {
                pthread_mutex_unlock(&network_read_mutex);
                break;
            }
            
            fd_set set;
            FD_ZERO(&set);
            FD_SET(gdbserver_socket, &set);

            if (select(gdbserver_socket + 1, &set, NULL, NULL, &timeout) <= 0)
            {
                pthread_mutex_unlock(&network_read_mutex);
                continue;
            }
            pthread_mutex_unlock(&network_read_mutex);
        }

        socklen_t socklen;
        struct sockaddr_in connected_addr;
        int sock = accept(gdbserver_socket, (struct sockaddr*)&connected_addr, &socklen);
        if (sock < 0)
        {
            continue;
        }
      
        int optval = 1;
        setsockopt (sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&optval, sizeof(optval));
#ifndef WIN32
        int interval = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(int));
        int maxpkt = 10;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(int));
#endif

        printf("Accepted new socket: %d\n", sock);

        gdbserver_client_socket = sock;
        // Reset packets subsystem for new connection
        packets_reset();
        packets_clear_incoming_raw();
    
        int ret;
        while ((ret = process_network(gdbserver_client_socket)) == 0) ;
        printf("Socket closed: %d\n", gdbserver_client_socket);
        
        /* Remove the client's breakpoints without racing the emulator's
           debugger_check() walk: free directly only while the emulator is
           parked in the trap loop; otherwise defer the free to the emulator
           thread, which performs it from debugger_check(). */
        if (gdbserver_trapped) {
            debugger_breakpoint_remove_all();
        } else {
            debugger_breakpoints_remove_pending = 1;
        }
        
        compat_socket_close(gdbserver_client_socket);
        gdbserver_client_socket = -1;

        gdbserver_detrap();
    }
    return NULL;
}

void gdbserver_init()
{
    pthread_cond_init(&trapped_cond, NULL);
    pthread_cond_init(&response_cond, NULL);
    pthread_cond_init(&emulating_cond, NULL);
    pthread_mutex_init(&trap_process_mutex, NULL);
    pthread_mutex_init(&network_read_mutex, NULL);
    pthread_mutex_init(&network_write_mutex, NULL);
    pthread_mutex_init(&emulating_mutex, NULL);
    
    // Initialize packets subsystem
    packets_init();
    gdbserver_reset_event =
      event_register(gdbserver_reset_event_fn, "GDB server reset");
    
    gdbserver_refresh_status();
}

int gdbserver_start( int port )
{
    utils_networking_init();

    gdbserver_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (gdbserver_socket == -1)
    {
#ifdef WIN32
        return WSAGetLastError();
#else
        return 1;
#endif
    }

    {
        int reuseval = 1;
        setsockopt(gdbserver_socket, SOL_SOCKET, SO_REUSEADDR, (void*)&reuseval, sizeof(int));
    }

    {
        struct linger sl;
        sl.l_onoff = 1;
        sl.l_linger = 0;
        setsockopt(gdbserver_socket, SOL_SOCKET, SO_LINGER, (void*)&sl, sizeof(sl));
    }

    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    // Try to bind to the requested port, or find the next available port
    int actual_port = port;
    int max_attempts = 5; // Limit to prevent infinite loop
    int attempts = 0;
    
    while (attempts < max_attempts)
    {
        servaddr.sin_port = htons(actual_port);
        
        if (bind(gdbserver_socket, (struct sockaddr*)&servaddr, sizeof(servaddr)) >= 0)
        {
            // Successfully bound to this port
            break;
        }
        
#ifdef WIN32
        int err = WSAGetLastError();
        if (err != WSAEADDRINUSE)
        {
            // Some other error, not just port in use
            utils_networking_end();
            return 2;
        }
#else
        if (errno != EADDRINUSE)
        {
            // Some other error, not just port in use
            utils_networking_end();
            return 2;
        }
#endif
        
        // Port is busy, try next port
        actual_port++;
        attempts++;
    }
    
    if (attempts >= max_attempts)
    {
        // Couldn't find an available port
        utils_networking_end();
        return 2;
    }
  
    if ((listen(gdbserver_socket, 5)) != 0)
    {
        utils_networking_end();
        return 3;
    }

    if (pthread_create(&network_thread_id, NULL, network_thread, NULL) != 0)
    {
        utils_networking_end();
        return 4;
    }

    gdbserver_port = actual_port;
    gdbserver_requested_port = port;
    gdbserver_debugging_enabled = 1;
    
    // Log if we used a different port than requested
    if (actual_port != port)
    {
        printf("GDB server: Port %d was busy, using port %d instead\n", port, actual_port);
    }
    
    return 0;
}

void gdbserver_stop()
{
    if (!gdbserver_debugging_enabled)
    {
        return;
    }

    /* Wake the main thread and any network-thread waiters before we touch
     * sockets. Otherwise pthread_join can deadlock: e.g. UI shows a modal
     * dialog so the CPU thread never services scheduled_action, while the
     * network thread blocks in gdbserver_execute_on_main_thread; or the
     * network thread holds network_read_mutex across recv(). */
    pthread_mutex_lock(&trap_process_mutex);
    gdbserver_shutting_down = 1;
    scheduled_action = NULL;
    pthread_cond_broadcast(&response_cond);
    pthread_cond_broadcast(&trapped_cond);
    pthread_mutex_unlock(&trap_process_mutex);

    pthread_mutex_lock(&emulating_mutex);
    pthread_cond_broadcast(&emulating_cond);
    pthread_mutex_unlock(&emulating_mutex);

#ifdef WIN32
    if (gdbserver_client_socket != -1)
        (void)shutdown(gdbserver_client_socket, SD_BOTH);
    if (gdbserver_socket != -1)
        (void)shutdown(gdbserver_socket, SD_BOTH);
#else
    if (gdbserver_client_socket != -1)
        (void)shutdown(gdbserver_client_socket, SHUT_RDWR);
    if (gdbserver_socket != -1)
        (void)shutdown(gdbserver_socket, SHUT_RDWR);
#endif

    gdbserver_debugging_enabled = 0;
    gdbserver_port = 0;
    gdbserver_requested_port = 0;

    pthread_join(network_thread_id, NULL);

    pthread_mutex_lock(&network_read_mutex);
    if (gdbserver_socket != -1)
    {
        compat_socket_close(gdbserver_socket);
        gdbserver_socket = -1;
    }
    if (gdbserver_client_socket != -1)
    {
        compat_socket_close(gdbserver_client_socket);
        gdbserver_client_socket = -1;
    }

    pthread_mutex_unlock(&network_read_mutex);

    gdbserver_shutting_down = 0;
    utils_networking_end();
}

void gdbserver_schedule_reset(void)
{
    if (gdbserver_reset_event >= 0)
        event_add(tstates + 1, gdbserver_reset_event);
}

void gdbserver_schedule_autoboot(void)
{
    spectranext_autoboot = autoboot_force_set_once;
    event_add(tstates + 1, gdbserver_reset_event);
}

void gdbserver_refresh_status()
{
    if (gdbserver_debugging_enabled ^ settings_current.gdbserver_enable)
    {
        if (settings_current.gdbserver_enable)
        {
            int code = gdbserver_start(settings_current.gdbserver_port);
            if (code)
            {
                ui_error(UI_ERROR_ERROR, "Cannot start gdbserver on port %d, code: %d", settings_current.gdbserver_port, code);
                settings_current.gdbserver_enable = 0;
                ui_menu_activate( UI_MENU_ITEM_MACHINE_DEBUGGER, 1 );
            }
            else
            {
                ui_menu_activate( UI_MENU_ITEM_MACHINE_DEBUGGER, 0 );
            }
        }
        else
        {
            gdbserver_stop();
            ui_menu_activate( UI_MENU_ITEM_MACHINE_DEBUGGER, 1 );
        }
    }
    else if (gdbserver_debugging_enabled &&
            (gdbserver_requested_port != settings_current.gdbserver_port))
    {
        gdbserver_stop();
        int code = gdbserver_start(settings_current.gdbserver_port);
        if (code)
        {
            ui_error(UI_ERROR_ERROR, "Cannot restart gdbserver on port %d, code: %d", settings_current.gdbserver_port, code);
            settings_current.gdbserver_enable = 0;
            ui_menu_activate( UI_MENU_ITEM_MACHINE_DEBUGGER, 1 );
        }
    }
}

static uint8_t gdbserver_detrap()
{
    uint8_t result = 0;
    pthread_mutex_lock(&trap_process_mutex);
    if (gdbserver_trapped)
    {
        gdbserver_trapped = 0;
        pthread_cond_signal(&trapped_cond);
        result = 1;
    }
    pthread_mutex_unlock(&trap_process_mutex);
    return result;
}

// schedule a simple job (call) on the main thread, while it's trapped
// data and response are supposed to be located on the stack of the caller (network) thread,
// as it's going to be stopped until the response is there
uint8_t gdbserver_execute_on_main_thread(trapped_action_t call, const void* data, void* response)
{
    pthread_mutex_lock(&trap_process_mutex);

    if (gdbserver_shutting_down)
    {
        pthread_mutex_unlock(&trap_process_mutex);
        return 0;
    }

    if (gdbserver_trapped != 1 && debugger_mode != DEBUGGER_MODE_HALTED)
    {
        pthread_mutex_unlock(&trap_process_mutex);
        return 0;
    }

    // prepare the action arguments
    scheduled_action = call;
    scheduled_action_data = data;
    scheduled_action_response = response;
    scheduled_action_response_delivered = 0;

    // execute
    pthread_cond_signal(&trapped_cond);
    pthread_mutex_unlock(&trap_process_mutex);

#if defined(__APPLE__)
    pthread_yield_np();
#elif defined(WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif

    pthread_mutex_lock(&trap_process_mutex);
    // wait for the response
    while (scheduled_action_response_delivered == 0 && !gdbserver_shutting_down)
    {
        pthread_cond_wait(&response_cond, &trap_process_mutex);
    }
    if (gdbserver_shutting_down && !scheduled_action_response_delivered)
    {
        scheduled_action = NULL;
        scheduled_action_data = NULL;
        scheduled_action_response = NULL;
        pthread_mutex_unlock(&trap_process_mutex);
        return 0;
    }
    pthread_mutex_unlock(&trap_process_mutex);
    return 1;
}

static uint8_t action_get_registers(const void* arg, void* response)
{
    int i;
    char* resp_buff = (char*)response;
  
    libspectrum_word regs[sizeof(registers) / (sizeof(libspectrum_word*))];
  
    for (i = 0; i < sizeof(registers) / (sizeof(libspectrum_word*)); i++)
    {
        get_register_value(i, &regs[i]);
    }
  
    mem2hex((const uint8_t *)regs, resp_buff, (sizeof(registers) / sizeof(libspectrum_word*)) * 2);
  
    return 0;
}

static uint8_t action_set_registers(const void* arg, void* response)
{
    int i;
    struct action_set_registers_args_t* regs = (struct action_set_registers_args_t*)arg;
    char* resp_buff = (char*)response;
  
    for (i = 0; i < sizeof(registers) / (sizeof(libspectrum_word*)); i++)
    {
        set_register_value(i, regs->regs_data[i]);
    }
  
    strcpy(resp_buff, "OK");
    return 0;
}

static uint8_t action_get_mem(const void* arg, void* response)
{
    int i;
    struct action_mem_args_t* mem = (struct action_mem_args_t*)arg;
    char* resp_buff = (char*)response;
  
    uint32_t t = tstates;
  
    libspectrum_word address = mem->maddr;
    for (i = 0; i < mem->mlen; i++, address++)
    {
        libspectrum_byte data = readbyte(address);
        mem2hex((void *)&data, resp_buff + i * 2, 1);
    }
  
    tstates = t;

    resp_buff[mem->mlen * 2] = '\0';
    return 0;
}

static uint8_t action_set_mem(const void* arg, void* response)
{
    uint32_t t = tstates;
  
    int i;
    struct action_mem_args_t* mem = (struct action_mem_args_t*)arg;
    char* resp_buff = (char*)response;
  
    libspectrum_byte data;
    libspectrum_word address = mem->maddr;
    for (i = 0; i < mem->mlen; i++, address++)
    {
        hex2mem((char*)mem->payload + i * 2, (uint8_t *)&data, 1);
        writebyte(address, data);
    }
  
    tstates = t;
  
    strcpy(resp_buff, "OK");
    return 0;
}

static uint8_t action_get_register(const void* arg, void* response)
{
    struct action_register_args_t* r = (struct action_register_args_t*)arg;
    char* resp_buff = (char*)response;

    libspectrum_word reg;
    if (get_register_value(r->reg, &reg))
    {
        strcpy(resp_buff, "E01");
        return 0;
    }

    mem2hex((const uint8_t *)&reg, resp_buff, SZ);
    resp_buff[SZ * 2] = '\0';
    return 0;
}

static uint8_t action_set_register(const void* arg, void* response)
{
    struct action_register_args_t* r = (struct action_register_args_t*)arg;
    char* resp_buff = (char*)response;
  
    if (set_register_value(r->reg, r->value))
    {
        strcpy(resp_buff, "E01");
        return 0;
    }

    strcpy(resp_buff, "OK");
    return 0;
}

static uint8_t action_set_breakpoint(const void* arg, void* response)
{
    struct action_breakpoint_args_t* b = (struct action_breakpoint_args_t*)arg;
    char* resp_buff = (char*)response;
  
    if (debugger_breakpoint_add_address(
        DEBUGGER_BREAKPOINT_TYPE_EXECUTE, memory_source_any, 0, b->maddr, 0,
        DEBUGGER_BREAKPOINT_LIFE_PERMANENT, NULL))
    {
        strcpy(resp_buff, "E01");
    }
    else
    {
        strcpy(resp_buff, "OK");
    }
  
    return 0;
}

static uint8_t action_remove_breakpoint(const void* arg, void* response)
{
    struct action_breakpoint_args_t* b = (struct action_breakpoint_args_t*)arg;
    char* resp_buff = (char*)response;
  
    libspectrum_word address = b->maddr;
    GSList* ptr;
    debugger_breakpoint* found = NULL;
    for(ptr = debugger_breakpoints; ptr; ptr = ptr->next)
    {
        debugger_breakpoint* p = (debugger_breakpoint*)ptr->data;
        if (p->type != DEBUGGER_BREAKPOINT_TYPE_EXECUTE)
            continue;
        if (p->value.address.source != memory_source_any)
            continue;
        if (p->value.address.offset != address)
            continue;
        found = p;
        break;
    }

    if (found)
    {
        debugger_breakpoint_remove(found->id);
        strcpy(resp_buff, "OK");
    }
    else
    {
        strcpy(resp_buff, "E01");
    }
  
    return 0;
}

void gdbserver_on_machine_reset(void)
{
#ifdef BUILD_SPECTRANET
    switch (spectranext_autoboot)
    {
      case autoboot_force_set_once:
      {
        spectranet_config_set_string(CONFIG_SECTION_AUTO_MOUNT, CONFIG_ITEM_MOUNT_RESOURCE, "xfs://ram/");
        spectranet_config_set_byte(CONFIG_SECTION_AUTO_MOUNT, CONFIG_ITEM_AUTO_BOOT, 1);
        
        uint8_t was_set = 0;

        spectranet_config_get_byte(
            CONFIG_SECTION_AUTO_MOUNT,
            CONFIG_ITEM_AUTO_BOOT,
            &was_set
        );
        
        if (was_set)
        {
          spectranext_autoboot = autoboot_nothing;
        }
        else
        {
          spectranext_autoboot = autoboot_force_reset_once;
        }
        
        break;
      }
      case autoboot_force_reset_once:
      {
        spectranet_config_set_byte(CONFIG_SECTION_AUTO_MOUNT, CONFIG_ITEM_AUTO_BOOT, 0);
        spectranext_autoboot = autoboot_nothing;
        break;
      }
      default:
      {
        break;
      }
    }
#endif
}

static uint8_t action_step_instruction(const void* arg, void* response)
{
    struct action_step_args_t* a = (struct action_step_args_t*)arg;

    if (a == NULL)
    {
        return 1;
    }
    else
    {
        if (a->addr != 0)
        {
            PC = a->addr;
        }
      
        debugger_breakpoint_add_address(
            DEBUGGER_BREAKPOINT_TYPE_EXECUTE, memory_source_any, 0, PC + a->len, 0,
            DEBUGGER_BREAKPOINT_LIFE_ONESHOT, NULL
        );
    }
  
    return 0;
}

int gdbserver_activate()
{
    // Default to signal received (T02) for backward compatibility
    return gdbserver_activate_with_reason(DEBUG_TRAP_REASON_SIGNAL_RECEIVED);
}

int gdbserver_activate_with_reason(int trap_reason)
{
    if (gdbserver_client_socket != -1 && gdbserver_debugging_enabled)
        gdbserver_send_stop_reply(trap_reason);

    pthread_mutex_lock(&trap_process_mutex);
    gdbserver_last_trap_reason = trap_reason;
    gdbserver_trapped = 1;
    pthread_cond_signal(&trapped_cond);
  
    uint8_t halt = 0;

    // a simple loop that waits for someone to unlock, or postone a simple
    // action (only one at a time)
    do
    {

        if (!gdbserver_trapped)
        {
            break;
        }

        pthread_cond_wait(&trapped_cond, &trap_process_mutex);

        if (gdbserver_shutting_down)
        {
            gdbserver_trapped = 0;
            break;
        }

        if (scheduled_action != NULL)
        {
            halt = scheduled_action(scheduled_action_data, scheduled_action_response);
            scheduled_action = NULL;
            scheduled_action_response_delivered = 1;
            
            // notify the waiter that we're done
            pthread_cond_signal(&response_cond);
        }
      
    } while (1);

    if (gdbserver_shutting_down)
    {
        debugger_mode = DEBUGGER_MODE_ACTIVE;
        gdbserver_trapped = 0;
    }
    else if (halt == 0)
    {
        debugger_mode = DEBUGGER_MODE_ACTIVE;
        // printf("Execution resumed.\n");
    }
    else
    {
        debugger_mode = DEBUGGER_MODE_HALTED;
        gdbserver_trapped = 0;
    }

    pthread_mutex_unlock(&trap_process_mutex);

    return 0;
}
