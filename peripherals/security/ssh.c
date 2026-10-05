#include "config.h"

#include "ssh.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#if defined(WIN32) || defined(_WIN32)
#include <winsock2.h>
#include <direct.h>
#else
#include <sys/select.h>
#endif

#include "mbedtls_config.h"
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <libssh2.h>

#include "compat.h"
#include "ui/ui.h"

#ifndef SSH_TERM_TYPE
#define SSH_TERM_TYPE "vt100"
#endif

#ifndef SSH_TERM_COLS
#define SSH_TERM_COLS 32
#endif

#ifndef SSH_TERM_ROWS
#define SSH_TERM_ROWS 24
#endif

#define SSH_LINE_MAX 128
#define SSH_PASSWORD_MAX 96
#define SSH_FINGERPRINT_MAX 96
#define SSH_IDENTITY_PRIVATE_FILE "identity_priv.pem"
#define SSH_IDENTITY_PUBLIC_FILE "identity_pub.txt"
#define SSH_KNOWN_HOSTS_FILE "known_hosts"
#define SSH_RSA_BITS 2048
#define SSH_RSA_EXPONENT 65537
#define SSH_CLEANUP_RETRIES 50
#define SSH_STORE_SUBDIR "ssh"

static bool g_ssh_initialized;
static bool g_ssh_store_initialized;
static char g_ssh_base_path[512];
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;

typedef enum ssh_control_state_t {
    SSH_STATE_NEW,
    SSH_STATE_EMIT_KEYGEN,
    SSH_STATE_EMIT_IDENTITY,
    SSH_STATE_CHECK_HOST,
    SSH_STATE_EMIT_TRUST,
    SSH_STATE_WAIT_TRUST,
    SSH_STATE_AUTH_START,
    SSH_STATE_EMIT_USER,
    SSH_STATE_WAIT_USER,
    SSH_STATE_AUTH_METHODS,
    SSH_STATE_EMIT_PASSWORD,
    SSH_STATE_WAIT_PASSWORD,
    SSH_STATE_AUTH_PASSWORD,
    SSH_STATE_OPEN_SHELL,
    SSH_STATE_EMIT_CONNECTED,
    SSH_STATE_RAW,
    SSH_STATE_ERROR
} ssh_control_state_t;

struct ssh_socket_t {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    compat_socket_t socket_fd;
    bool connected;
    bool raw_mode;
    char host[128];
    uint16_t port;
    char username[64];
    char password[SSH_PASSWORD_MAX];
    char last_error[64];
    char *identity_private_pem;
    char *identity_public_line;
    ssh_control_state_t state;
    bool identity_generated;
    bool host_known;
    bool error_emitted;
    char fingerprint[SSH_FINGERPRINT_MAX];

    pthread_mutex_t line_mutex;
    pthread_cond_t line_cond;
    pthread_mutex_t libssh2_lock;
    char line[SSH_LINE_MAX];
    size_t line_len;
    bool line_ready;
    bool prompt_pending;
};

static void ssh_set_error(ssh_socket_t *ssh, const char *reason)
{
    if (!ssh || !reason) {
        return;
    }
    strncpy(ssh->last_error, reason, sizeof(ssh->last_error) - 1);
    ssh->last_error[sizeof(ssh->last_error) - 1] = '\0';
}

static void ssh_dispose_identity(ssh_socket_t *ssh)
{
    if (!ssh) {
        return;
    }

    if (ssh->identity_private_pem) {
        memset(ssh->identity_private_pem, 0, strlen(ssh->identity_private_pem));
        free(ssh->identity_private_pem);
        ssh->identity_private_pem = NULL;
    }
    free(ssh->identity_public_line);
    ssh->identity_public_line = NULL;
}

static bool ssh_wait_socket(LIBSSH2_SESSION *session, int socket_fd)
{
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 100000,
    };
    fd_set readfds;
    fd_set writefds;
    fd_set *readfd = NULL;
    fd_set *writefd = NULL;
    int dir = libssh2_session_block_directions(session);

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);

    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) {
        FD_SET(socket_fd, &readfds);
        readfd = &readfds;
    }
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) {
        FD_SET(socket_fd, &writefds);
        writefd = &writefds;
    }

    if (!readfd && !writefd) {
        usleep(10000);
        return true;
    }

    int rc = select( (int)socket_fd + 1, readfd, writefd, NULL, &timeout );
    return rc >= 0 || errno == EINTR;
}

static int ssh_cleanup_wait(ssh_socket_t *ssh, int rc)
{
    if (rc != LIBSSH2_ERROR_EAGAIN) {
        return rc;
    }

    if (ssh->session && ssh->socket_fd != compat_socket_invalid) {
        ssh_wait_socket(ssh->session, (int)ssh->socket_fd);
    } else {
        usleep(10000);
    }
    return rc;
}

static int
ssh_store_mkdir( const char *path )
{
#if defined(WIN32) || defined(_WIN32)
    if( _mkdir( path ) == 0 || errno == EEXIST )
#else
    if( mkdir( path, 0700 ) == 0 || errno == EEXIST )
#endif
        return 0;
    return -1;
}

static int
ssh_store_init( void )
{
    const char *base = compat_get_config_path();

    if( g_ssh_store_initialized )
        return 0;

    if( !base )
        return -1;

    if( snprintf( g_ssh_base_path, sizeof(g_ssh_base_path), "%s" FUSE_DIR_SEP_STR "%s",
                  base, SSH_STORE_SUBDIR ) >= (int)sizeof(g_ssh_base_path) )
        return -1;

    if( ssh_store_mkdir( g_ssh_base_path ) != 0 ) {
        ui_error( UI_ERROR_WARNING, "ssh: failed to create ssh directory: %s\n",
                  strerror(errno) );
        return -1;
    }

    g_ssh_store_initialized = true;
    return 0;
}

static int
ssh_store_path( char *out, size_t out_len, const char *filename )
{
    if( !out || out_len == 0 || !filename || ssh_store_init() != 0 )
        return -1;

    if( snprintf( out, out_len, "%s" FUSE_DIR_SEP_STR "%s",
                  g_ssh_base_path, filename ) >= (int)out_len )
        return -1;

    return 0;
}

static char *
ssh_read_file( const char *filename )
{
    char path[512];
    FILE *fp;
    long size;
    char *buf;

    if( ssh_store_path( path, sizeof(path), filename ) != 0 )
        return NULL;

    fp = fopen( path, "rb" );
    if( !fp )
        return NULL;

    if( fseek( fp, 0, SEEK_END ) != 0 ) {
        fclose( fp );
        return NULL;
    }

    size = ftell( fp );
    if( size <= 0 || size > 65536 ) {
        fclose( fp );
        return NULL;
    }

    rewind( fp );
    buf = malloc( (size_t)size + 1 );
    if( !buf ) {
        fclose( fp );
        return NULL;
    }

    if( fread( buf, 1, (size_t)size, fp ) != (size_t)size ) {
        free( buf );
        fclose( fp );
        return NULL;
    }

    buf[size] = '\0';
    fclose( fp );
    return buf;
}

static int
ssh_write_file( const char *filename, const char *data )
{
    char path[512];
    FILE *fp;

    if( !data || ssh_store_path( path, sizeof(path), filename ) != 0 )
        return -1;

    fp = fopen( path, "wb" );
    if( !fp )
        return -1;

    if( fputs( data, fp ) == EOF ) {
        fclose( fp );
        return -1;
    }

    fclose( fp );
    return 0;
}

static int ssh_zx_send_text(ssh_zx_send_cb_t cb, void *ctx, const char *text)
{
    return cb(ctx, (const uint8_t *)text, strlen(text));
}

static int ssh_zx_send_error(ssh_zx_send_cb_t cb, void *ctx, const char *reason)
{
    char out[96];
    snprintf(out, sizeof(out), "ERROR %s\r\n", reason ? reason : "failure");
    return ssh_zx_send_text(cb, ctx, out);
}

static uint8_t *ssh_blob_put_u32(uint8_t *p, uint32_t v)
{
    *p++ = (uint8_t)(v >> 24);
    *p++ = (uint8_t)(v >> 16);
    *p++ = (uint8_t)(v >> 8);
    *p++ = (uint8_t)v;
    return p;
}

static int ssh_blob_put_string(uint8_t **p, const uint8_t *end, const uint8_t *data, size_t len)
{
    if ((size_t)(end - *p) < len + 4) {
        return -1;
    }
    *p = ssh_blob_put_u32(*p, (uint32_t)len);
    memcpy(*p, data, len);
    *p += len;
    return 0;
}

static int ssh_blob_put_mpi(uint8_t **p, const uint8_t *end, const mbedtls_mpi *mpi)
{
    uint8_t *tmp = calloc(1, SSH_RSA_BITS / 8 + 1);
    size_t len = mbedtls_mpi_size(mpi);
    size_t offset = 0;
    int rc = -1;

    if (!tmp || len == 0 || len > SSH_RSA_BITS / 8) {
        return -1;
    }

    if (mbedtls_mpi_write_binary(mpi, tmp + 1, len) != 0) {
        goto out;
    }

    if ((tmp[1] & 0x80) == 0) {
        offset = 1;
    } else {
        len++;
    }

    rc = ssh_blob_put_string(p, end, tmp + offset, len);

out:
    free(tmp);
    return rc;
}

static char *ssh_public_line_from_rsa(mbedtls_rsa_context *rsa)
{
    static const uint8_t ssh_rsa[] = "ssh-rsa";
    mbedtls_mpi n;
    mbedtls_mpi e;
    uint8_t *blob = calloc(1, 384);
    uint8_t *b64 = calloc(1, 560);
    uint8_t *p = blob;
    const uint8_t *end = blob ? blob + 384 : NULL;
    size_t b64_len = 0;
    char *line = NULL;

    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);

    if (!blob || !b64 ||
        mbedtls_rsa_export(rsa, &n, NULL, NULL, NULL, &e) != 0 ||
        ssh_blob_put_string(&p, end, ssh_rsa, sizeof(ssh_rsa) - 1) < 0 ||
        ssh_blob_put_mpi(&p, end, &e) < 0 ||
        ssh_blob_put_mpi(&p, end, &n) < 0 ||
        mbedtls_base64_encode(b64, 560, &b64_len, blob, (size_t)(p - blob)) != 0) {
        goto out;
    }

    line = malloc(strlen("ssh-rsa ") + b64_len + strlen(" spectranext-fuse") + 1);
    if (!line) {
        goto out;
    }

    memcpy(line, "ssh-rsa ", strlen("ssh-rsa "));
    memcpy(line + strlen("ssh-rsa "), b64, b64_len);
    strcpy(line + strlen("ssh-rsa ") + b64_len, " spectranext-fuse");

out:
    free(blob);
    free(b64);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    return line;
}

static int ssh_generate_identity(char **private_pem, char **public_line)
{
    mbedtls_pk_context pk;
    unsigned char *private_buf = calloc(1, 4096);
    char *pub = NULL;
    char *priv = NULL;
    int rc = -1;

    if (!private_buf) {
        return -1;
    }

    mbedtls_pk_init(&pk);

    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) {
        goto out;
    }

    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), mbedtls_ctr_drbg_random, &g_ctr_drbg,
                            SSH_RSA_BITS, SSH_RSA_EXPONENT) != 0) {
        goto out;
    }

    if (mbedtls_pk_write_key_pem(&pk, private_buf, 4096) != 0) {
        goto out;
    }

    priv = strdup((const char *)private_buf);
    pub = ssh_public_line_from_rsa(mbedtls_pk_rsa(pk));
    if (!priv || !pub) {
        goto out;
    }

    *private_pem = priv;
    *public_line = pub;
    priv = NULL;
    pub = NULL;
    rc = 0;

out:
    free(priv);
    free(pub);
    free(private_buf);
    mbedtls_pk_free(&pk);
    return rc;
}

static int ssh_load_or_create_identity(ssh_socket_t *ssh, ssh_zx_send_cb_t zx_send, void *zx_ctx)
{
    ssh->identity_private_pem = ssh_read_file( SSH_IDENTITY_PRIVATE_FILE );
    ssh->identity_public_line = ssh_read_file( SSH_IDENTITY_PUBLIC_FILE );
    if (ssh->identity_private_pem && ssh->identity_public_line) {
        return 0;
    }

    free(ssh->identity_private_pem);
    free(ssh->identity_public_line);
    ssh->identity_private_pem = NULL;
    ssh->identity_public_line = NULL;

    if (ssh_zx_send_text(zx_send, zx_ctx, "KEY-GENERATION\r\n") < 0) {
        ssh_set_error(ssh, "zx_stream_failed");
        return -1;
    }

    if (ssh_generate_identity(&ssh->identity_private_pem, &ssh->identity_public_line) != 0) {
        ssh_set_error(ssh, "identity_generate_failed");
        return -1;
    }

    if (ssh_write_file( SSH_IDENTITY_PRIVATE_FILE, ssh->identity_private_pem ) != 0 ||
        ssh_write_file( SSH_IDENTITY_PUBLIC_FILE, ssh->identity_public_line ) != 0) {
        ssh_set_error(ssh, "identity_store_failed");
        return -1;
    }

    return 0;
}

static int ssh_prompt_line(ssh_socket_t *ssh, ssh_zx_send_cb_t cb, void *ctx,
                           const char *prompt, char *out, size_t out_len)
{
    pthread_mutex_lock( &ssh->line_mutex );
    ssh->line_ready = false;
    ssh->line_len = 0;
    ssh->line[0] = '\0';
    ssh->prompt_pending = true;
    pthread_mutex_unlock( &ssh->line_mutex );

    if (ssh_zx_send_text(cb, ctx, prompt) < 0) {
        pthread_mutex_lock( &ssh->line_mutex );
        ssh->prompt_pending = false;
        pthread_mutex_unlock( &ssh->line_mutex );
        ssh_set_error(ssh, "zx_stream_failed");
        return -1;
    }

    while (ssh->socket_fd != compat_socket_invalid) {
        struct timespec deadline;
        int rc;

        clock_gettime( CLOCK_REALTIME, &deadline );
        deadline.tv_sec += 30;

        pthread_mutex_lock( &ssh->line_mutex );
        while (!ssh->line_ready && ssh->socket_fd != compat_socket_invalid) {
            rc = pthread_cond_timedwait( &ssh->line_cond, &ssh->line_mutex, &deadline );
            if (rc == ETIMEDOUT) {
                ssh->prompt_pending = false;
                pthread_mutex_unlock( &ssh->line_mutex );
                ssh_set_error(ssh, "prompt_timeout");
                return -1;
            }
        }

        if (!ssh->line_ready) {
            ssh->prompt_pending = false;
            pthread_mutex_unlock( &ssh->line_mutex );
            break;
        }

        strncpy(out, ssh->line, out_len - 1);
        out[out_len - 1] = '\0';
        ssh->line_ready = false;
        ssh->line_len = 0;
        ssh->line[0] = '\0';
        ssh->prompt_pending = false;
        pthread_mutex_unlock( &ssh->line_mutex );
        return 0;
    }

    ssh_set_error(ssh, "disconnected");
    return -1;
}

static bool ssh_line_is_accept(const char *line, const char *fingerprint)
{
    char lower[SSH_LINE_MAX];
    size_t i;

    if (strcmp(line, fingerprint) == 0) {
        return true;
    }

    for (i = 0; line[i] && i < sizeof(lower) - 1; i++) {
        lower[i] = (char)tolower((unsigned char)line[i]);
    }
    lower[i] = '\0';

    return strcmp(lower, "y") == 0 || strcmp(lower, "yes") == 0 ||
           strcmp(lower, "accept") == 0 || strcmp(lower, "trust") == 0;
}

static void ssh_make_nvs_key(const char *host, uint16_t port, char *out, size_t out_len)
{
    unsigned char hash[32];
    char input[160];
    size_t used = 0;

    for (; host && *host && used < sizeof(input) - 8; host++) {
        input[used++] = (char)tolower((unsigned char)*host);
    }
    snprintf(input + used, sizeof(input) - used, ":%u", (unsigned)port);

    mbedtls_sha256((const unsigned char *)input, strlen(input), hash, 0);
    snprintf(out, out_len, "k%02x%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6]);
}

static bool
ssh_known_host_lookup( const char *key, char *stored, size_t stored_len )
{
    char path[512];
    FILE *fp;
    char line[256];
    char file_key[32];

    if( ssh_store_path( path, sizeof(path), SSH_KNOWN_HOSTS_FILE ) != 0 )
        return false;

    fp = fopen( path, "rb" );
    if( !fp )
        return false;

    while( fgets( line, sizeof(line), fp ) ) {
        if( sscanf( line, "%31s %95s", file_key, stored ) != 2 )
            continue;
        if( strcmp( file_key, key ) == 0 ) {
            fclose( fp );
            return true;
        }
    }

    fclose( fp );
    (void)stored_len;
    return false;
}

static bool ssh_known_host_matches(const char *host, uint16_t port, const char *fingerprint, bool *known)
{
    char key[16];
    char stored[SSH_FINGERPRINT_MAX];

    *known = false;
    ssh_make_nvs_key(host, port, key, sizeof(key));

    if( !ssh_known_host_lookup( key, stored, sizeof(stored) ) )
        return false;

    *known = true;
    return strcmp(stored, fingerprint) == 0;
}

static int ssh_store_known_host(const char *host, uint16_t port, const char *fingerprint)
{
    char key[16];
    char path[512];
    char temp_path[512];
    char line[192];
    char existing_line[256];
    char file_key[32];
    FILE *in;
    FILE *out;
    bool stored = false;

    ssh_make_nvs_key(host, port, key, sizeof(key));

    if( ssh_store_path( path, sizeof(path), SSH_KNOWN_HOSTS_FILE ) != 0 )
        return -1;

    if( snprintf( temp_path, sizeof(temp_path), "%s.tmp", path ) >= (int)sizeof(temp_path) )
        return -1;

    snprintf( line, sizeof(line), "%s %s\n", key, fingerprint );

    in = fopen( path, "rb" );
    out = fopen( temp_path, "wb" );
    if( !out ) {
        if( in ) fclose( in );
        return -1;
    }

    if( in ) {
        while( fgets( existing_line, sizeof(existing_line), in ) ) {
            if( sscanf( existing_line, "%31s", file_key ) == 1 &&
                strcmp( file_key, key ) == 0 ) {
                if( !stored && fputs( line, out ) == EOF ) {
                    fclose( in );
                    fclose( out );
                    remove( temp_path );
                    return -1;
                }
                stored = true;
                continue;
            }

            if( fputs( existing_line, out ) == EOF ) {
                fclose( in );
                fclose( out );
                remove( temp_path );
                return -1;
            }
        }
        fclose( in );
    }

    if( !stored && fputs( line, out ) == EOF ) {
        fclose( out );
        remove( temp_path );
        return -1;
    }

    if( fclose( out ) != 0 ) {
        remove( temp_path );
        return -1;
    }

    if( rename( temp_path, path ) != 0 ) {
        remove( temp_path );
        return -1;
    }

    return 0;
}

static int ssh_host_fingerprint(ssh_socket_t *ssh, char *out, size_t out_len)
{
    size_t hostkey_len = 0;
    int hostkey_type = 0;
    const char *hostkey = libssh2_session_hostkey(ssh->session, &hostkey_len, &hostkey_type);
    (void)hostkey_type;
    if (!hostkey || hostkey_len == 0) {
        ssh_set_error(ssh, "hostkey_unavailable");
        return -1;
    }

    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)hostkey, hostkey_len, hash, 0);

    size_t used = 0;
    int n = snprintf(out, out_len, "SHA256:");
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    used = (size_t)n;

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out + used, out_len - used, &olen, hash, sizeof(hash)) != 0) {
        ssh_set_error(ssh, "fingerprint_failed");
        return -1;
    }

    used += olen;
    while (used > 0 && out[used - 1] == '=') {
        out[--used] = '\0';
    }
    return 0;
}

int ssh_session_verify_or_trust_host(LIBSSH2_SESSION *session, const char *host, uint16_t port)
{
    ssh_socket_t temporary;
    char fingerprint[SSH_FINGERPRINT_MAX];
    bool known = false;

    if (!session || !host || !host[0]) return -1;
    memset(&temporary, 0, sizeof(temporary));
    temporary.session = session;
    if (ssh_host_fingerprint(&temporary, fingerprint, sizeof(fingerprint)) < 0) return -1;
    if (ssh_known_host_matches(host, port, fingerprint, &known)) return 0;
    if (known) return -1;
    return ssh_store_known_host(host, port, fingerprint);
}

static int ssh_verify_host(ssh_socket_t *ssh, ssh_zx_send_cb_t cb, void *ctx)
{
    char fingerprint[SSH_FINGERPRINT_MAX];
    bool known = false;
    char prompt[128];
    char answer[SSH_LINE_MAX];

    if (ssh_host_fingerprint(ssh, fingerprint, sizeof(fingerprint)) < 0) {
        return -1;
    }

    if (ssh_known_host_matches(ssh->host, ssh->port, fingerprint, &known)) {
        return 0;
    }

    snprintf(prompt, sizeof(prompt), "TRUST? %s\r\n", fingerprint);
    if (ssh_prompt_line(ssh, cb, ctx, prompt, answer, sizeof(answer)) < 0) {
        return -1;
    }

    if (!ssh_line_is_accept(answer, fingerprint)) {
        ssh_set_error(ssh, known ? "hostkey_changed" : "hostkey_rejected");
        return -1;
    }

    if (ssh_store_known_host(ssh->host, ssh->port, fingerprint) < 0) {
        ssh_set_error(ssh, "trust_store_failed");
        return -1;
    }
    return 0;
}

static bool ssh_auth_method_allowed(const char *methods, const char *method)
{
    if (!methods || !method) {
        return false;
    }
    const char *p = methods;
    size_t len = strlen(method);
    while (*p) {
        while (*p == ',') {
            p++;
        }
        if (strncmp(p, method, len) == 0 && (p[len] == ',' || p[len] == '\0')) {
            return true;
        }
        p = strchr(p, ',');
        if (!p) {
            break;
        }
    }
    return false;
}

static int ssh_auth_publickey(ssh_socket_t *ssh)
{
    if (!ssh->identity_private_pem || !ssh->identity_public_line) {
        return -1;
    }

    int rc;
    do {
        rc = libssh2_userauth_publickey_frommemory(ssh->session,
                                                   ssh->username,
                                                   strlen(ssh->username),
                                                   ssh->identity_public_line,
                                                   strlen(ssh->identity_public_line),
                                                   ssh->identity_private_pem,
                                                   strlen(ssh->identity_private_pem),
                                                   NULL);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, ssh->socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);
    return rc == 0 ? 0 : -1;
}

static int ssh_authenticate(ssh_socket_t *ssh, ssh_zx_send_cb_t cb, void *ctx)
{
    char *methods = NULL;
    int rc;

    if (ssh->username[0] == '\0' &&
        ssh_prompt_line(ssh, cb, ctx, "USER?\r\n", ssh->username, sizeof(ssh->username)) < 0) {
        return -1;
    }

    do {
        methods = libssh2_userauth_list(ssh->session, ssh->username, (unsigned int)strlen(ssh->username));
        rc = methods ? 0 : libssh2_session_last_errno(ssh->session);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, ssh->socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    if (libssh2_userauth_authenticated(ssh->session)) {
        return 0;
    }

    if (methods && ssh_auth_method_allowed(methods, "publickey") && ssh_auth_publickey(ssh) == 0) {
        return 0;
    }

    if (!methods || !ssh_auth_method_allowed(methods, "password")) {
        ssh_set_error(ssh, "auth_method_unavailable");
        return -1;
    }

    if (ssh->password[0] == '\0' &&
        ssh_prompt_line(ssh, cb, ctx, "PASSWORD?\r\n", ssh->password, sizeof(ssh->password)) < 0) {
        return -1;
    }

    do {
        rc = libssh2_userauth_password_ex(ssh->session, ssh->username,
                                          (unsigned int)strlen(ssh->username),
                                          ssh->password,
                                          (unsigned int)strlen(ssh->password),
                                          NULL);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, ssh->socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    memset(ssh->password, 0, sizeof(ssh->password));
    if (rc != 0) {
        ssh_set_error(ssh, "auth_failed");
        return -1;
    }

    return 0;
}

static int ssh_open_shell(ssh_socket_t *ssh)
{
    int rc;

    do {
        ssh->channel = libssh2_channel_open_session(ssh->session);
        if (!ssh->channel) {
            rc = libssh2_session_last_errno(ssh->session);
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                ssh_wait_socket(ssh->session, ssh->socket_fd);
            }
        } else {
            rc = 0;
        }
    } while (!ssh->channel && rc == LIBSSH2_ERROR_EAGAIN);

    if (!ssh->channel) {
        ssh_set_error(ssh, "channel_open_failed");
        return -1;
    }

    do {
        rc = libssh2_channel_request_pty_ex(ssh->channel,
                                            SSH_TERM_TYPE, strlen(SSH_TERM_TYPE),
                                            NULL, 0,
                                            SSH_TERM_COLS, SSH_TERM_ROWS, 0, 0);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, ssh->socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    if (rc != 0) {
        ssh_set_error(ssh, "pty_failed");
        return -1;
    }

    do {
        rc = libssh2_channel_shell(ssh->channel);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, ssh->socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    if (rc != 0) {
        ssh_set_error(ssh, "shell_failed");
        return -1;
    }

    return 0;
}

static void ssh_extract_user(ssh_socket_t *ssh)
{
    char *at = strchr(ssh->host, '@');
    if (!at || at == ssh->host || at[1] == '\0') {
        return;
    }

    size_t user_len = at - ssh->host;
    if (user_len >= sizeof(ssh->username)) {
        user_len = sizeof(ssh->username) - 1;
    }
    memcpy(ssh->username, ssh->host, user_len);
    ssh->username[user_len] = '\0';
    memmove(ssh->host, at + 1, strlen(at + 1) + 1);
}

int ssh_socket_init(void)
{
    if (g_ssh_initialized) {
        return 0;
    }

    if (ssh_store_init() != 0) {
        return -1;
    }

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);

    const char *pers = "spectranext_ssh_identity";
    int rc = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char *)pers, strlen(pers));
    if (rc != 0) {
        mbedtls_ctr_drbg_free(&g_ctr_drbg);
        mbedtls_entropy_free(&g_entropy);
        return rc;
    }

    rc = libssh2_init(0);
    if (rc != 0) {
        mbedtls_ctr_drbg_free(&g_ctr_drbg);
        mbedtls_entropy_free(&g_entropy);
        return rc;
    }

    g_ssh_initialized = true;
    return 0;
}

ssh_socket_t *ssh_socket_allocate(const char *host, uint16_t port, const char *username)
{
    if (!host || host[0] == '\0') {
        return NULL;
    }
    if (ssh_socket_init() != 0) {
        return NULL;
    }

    ssh_socket_t *ssh = calloc(1, sizeof(*ssh));
    if (!ssh) {
        return NULL;
    }

    ssh->socket_fd = compat_socket_invalid;
    ssh->port = port;
    strncpy(ssh->host, host, sizeof(ssh->host) - 1);
    if (username && username[0] != '\0') {
        strncpy(ssh->username, username, sizeof(ssh->username) - 1);
        ssh->username[sizeof(ssh->username) - 1] = '\0';
    } else {
        ssh_extract_user(ssh);
    }
    pthread_mutex_init( &ssh->line_mutex, NULL );
    pthread_cond_init( &ssh->line_cond, NULL );
    pthread_mutex_init( &ssh->libssh2_lock, NULL );
    ssh_set_error(ssh, "failure");
    return ssh;
}

void ssh_socket_free(ssh_socket_t *ssh)
{
    if (!ssh) {
        return;
    }
    ssh_socket_close(ssh);
    pthread_mutex_destroy( &ssh->line_mutex );
    pthread_cond_destroy( &ssh->line_cond );
    pthread_mutex_destroy( &ssh->libssh2_lock );
    ssh_dispose_identity(ssh);
    free(ssh);
}

static int ssh_load_or_create_identity_deferred(ssh_socket_t *ssh)
{
    ssh->identity_private_pem = ssh_read_file( SSH_IDENTITY_PRIVATE_FILE );
    ssh->identity_public_line = ssh_read_file( SSH_IDENTITY_PUBLIC_FILE );
    if (ssh->identity_private_pem && ssh->identity_public_line) {
        ssh->identity_generated = false;
        return 0;
    }

    free(ssh->identity_private_pem);
    free(ssh->identity_public_line);
    ssh->identity_private_pem = NULL;
    ssh->identity_public_line = NULL;

    if (ssh_generate_identity(&ssh->identity_private_pem, &ssh->identity_public_line) != 0) {
        ssh_set_error(ssh, "identity_generate_failed");
        return -1;
    }

    if (ssh_write_file( SSH_IDENTITY_PRIVATE_FILE, ssh->identity_private_pem ) != 0 ||
        ssh_write_file( SSH_IDENTITY_PUBLIC_FILE, ssh->identity_public_line ) != 0) {
        ssh_set_error(ssh, "identity_store_failed");
        return -1;
    }

    ssh->identity_generated = true;
    return 0;
}

static ssize_t ssh_emit_line(ssh_socket_t *ssh, void *buf, size_t len, const char *text)
{
    size_t text_len = strlen(text);
    if (len < text_len) {
        ssh_set_error(ssh, "rx_buffer_too_small");
        ssh->state = SSH_STATE_ERROR;
        return -1;
    }
    memcpy(buf, text, text_len);
    return (ssize_t)text_len;
}

static int ssh_auth_password(ssh_socket_t *ssh)
{
    int rc;
    do {
        rc = libssh2_userauth_password_ex(ssh->session, ssh->username,
                                          (unsigned int)strlen(ssh->username),
                                          ssh->password,
                                          (unsigned int)strlen(ssh->password),
                                          NULL);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, ssh->socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    memset(ssh->password, 0, sizeof(ssh->password));
    if (rc != 0) {
        ssh_set_error(ssh, "auth_failed");
        return -1;
    }
    return 0;
}

static int ssh_advance_control(ssh_socket_t *ssh)
{
    while (ssh->state != SSH_STATE_ERROR && ssh->state != SSH_STATE_RAW) {
        switch (ssh->state) {
        case SSH_STATE_CHECK_HOST:
            if (ssh_host_fingerprint(ssh, ssh->fingerprint, sizeof(ssh->fingerprint)) < 0) {
                ssh->state = SSH_STATE_ERROR;
                return -1;
            }
            if (ssh_known_host_matches(ssh->host, ssh->port, ssh->fingerprint, &ssh->host_known)) {
                ssh->state = SSH_STATE_AUTH_START;
            } else {
                ssh->state = SSH_STATE_EMIT_TRUST;
                return 0;
            }
            break;
        case SSH_STATE_AUTH_START:
            if (ssh->username[0] == '\0') {
                ssh->state = SSH_STATE_EMIT_USER;
                return 0;
            }
            ssh->state = SSH_STATE_AUTH_METHODS;
            break;
        case SSH_STATE_AUTH_METHODS: {
            char *methods = NULL;
            int rc;
            do {
                methods = libssh2_userauth_list(ssh->session, ssh->username,
                                                (unsigned int)strlen(ssh->username));
                rc = methods ? 0 : libssh2_session_last_errno(ssh->session);
                if (rc == LIBSSH2_ERROR_EAGAIN) {
                    ssh_wait_socket(ssh->session, ssh->socket_fd);
                }
            } while (rc == LIBSSH2_ERROR_EAGAIN);

            if (libssh2_userauth_authenticated(ssh->session)) {
                ssh->state = SSH_STATE_OPEN_SHELL;
                break;
            }

            if (methods && ssh_auth_method_allowed(methods, "publickey") &&
                ssh_auth_publickey(ssh) == 0) {
                ssh->state = SSH_STATE_OPEN_SHELL;
                break;
            }

            if (!methods || !ssh_auth_method_allowed(methods, "password")) {
                ssh_set_error(ssh, "auth_method_unavailable");
                ssh->state = SSH_STATE_ERROR;
                return -1;
            }

            if (ssh->password[0] == '\0') {
                ssh->state = SSH_STATE_EMIT_PASSWORD;
                return 0;
            }
            ssh->state = SSH_STATE_AUTH_PASSWORD;
            break;
        }
        case SSH_STATE_AUTH_PASSWORD:
            if (ssh_auth_password(ssh) < 0) {
                ssh->state = SSH_STATE_ERROR;
                return -1;
            }
            ssh->state = SSH_STATE_OPEN_SHELL;
            break;
        case SSH_STATE_OPEN_SHELL:
            if (ssh_open_shell(ssh) < 0) {
                ssh->state = SSH_STATE_ERROR;
                return -1;
            }
            ssh_dispose_identity(ssh);
            ssh->connected = true;
            ssh->state = SSH_STATE_EMIT_CONNECTED;
            return 0;
        case SSH_STATE_EMIT_KEYGEN:
        case SSH_STATE_EMIT_IDENTITY:
        case SSH_STATE_EMIT_TRUST:
        case SSH_STATE_EMIT_USER:
        case SSH_STATE_EMIT_PASSWORD:
        case SSH_STATE_EMIT_CONNECTED:
        case SSH_STATE_WAIT_TRUST:
        case SSH_STATE_WAIT_USER:
        case SSH_STATE_WAIT_PASSWORD:
            return 0;
        default:
            ssh->state = SSH_STATE_ERROR;
            ssh_set_error(ssh, "invalid_state");
            return -1;
        }
    }
    return 0;
}

int ssh_socket_connect(ssh_socket_t *ssh, compat_socket_t socket_fd)
{
    int rc;
    if (!ssh || socket_fd == compat_socket_invalid) {
        return -1;
    }

    ssh->socket_fd = socket_fd;
    ssh->session = libssh2_session_init();
    if (!ssh->session) {
        ssh_set_error(ssh, "session_alloc_failed");
        return -1;
    }

    libssh2_session_set_blocking(ssh->session, 0);

    if (ssh_load_or_create_identity_deferred(ssh) < 0) {
        ssh_dispose_identity(ssh);
        ssh_socket_close(ssh);
        return -1;
    }

    do {
        rc = libssh2_session_handshake(ssh->session, socket_fd);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_wait_socket(ssh->session, socket_fd);
        }
    } while (rc == LIBSSH2_ERROR_EAGAIN);

    if (rc != 0) {
        ssh_set_error(ssh, "handshake_failed");
        ssh_dispose_identity(ssh);
        ssh_socket_close(ssh);
        return -1;
    }

    ssh->state = ssh->identity_generated ? SSH_STATE_EMIT_KEYGEN : SSH_STATE_EMIT_IDENTITY;
    return 0;
}

ssize_t ssh_socket_send(ssh_socket_t *ssh, const void *buf, size_t len)
{
    if (!ssh || !buf || len == 0) {
        return -1;
    }

    if (!ssh->raw_mode) {
        ssize_t consumed = ssh_socket_feed_control(ssh, buf, len);
        if (consumed <= 0 || !ssh->line_ready) {
            return consumed;
        }

        switch (ssh->state) {
        case SSH_STATE_WAIT_TRUST:
            if (!ssh_line_is_accept(ssh->line, ssh->fingerprint)) {
                ssh_set_error(ssh, ssh->host_known ? "hostkey_changed" : "hostkey_rejected");
                ssh->state = SSH_STATE_ERROR;
                break;
            }
            if (ssh_store_known_host(ssh->host, ssh->port, ssh->fingerprint) < 0) {
                ssh_set_error(ssh, "trust_store_failed");
                ssh->state = SSH_STATE_ERROR;
                break;
            }
            ssh->state = SSH_STATE_AUTH_START;
            break;
        case SSH_STATE_WAIT_USER:
            strncpy(ssh->username, ssh->line, sizeof(ssh->username) - 1);
            ssh->username[sizeof(ssh->username) - 1] = '\0';
            ssh->state = SSH_STATE_AUTH_METHODS;
            break;
        case SSH_STATE_WAIT_PASSWORD:
            strncpy(ssh->password, ssh->line, sizeof(ssh->password) - 1);
            ssh->password[sizeof(ssh->password) - 1] = '\0';
            ssh->state = SSH_STATE_AUTH_PASSWORD;
            break;
        default:
            break;
        }

        ssh->line_ready = false;
        ssh->line_len = 0;
        ssh->line[0] = '\0';
        ssh_advance_control(ssh);
        return consumed;
    }

    size_t total = 0;
    pthread_mutex_lock( &ssh->libssh2_lock );
    while (total < len) {
        ssize_t rc = libssh2_channel_write(ssh->channel, (const char *)buf + total, len - total);
        if (rc > 0) {
            total += (size_t)rc;
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            pthread_mutex_unlock( &ssh->libssh2_lock );
            ssh_wait_socket(ssh->session, (int)ssh->socket_fd);
            pthread_mutex_lock( &ssh->libssh2_lock );
            continue;
        }
        ssh_set_error(ssh, "write_failed");
        pthread_mutex_unlock( &ssh->libssh2_lock );
        return total ? (ssize_t)total : -1;
    }
    pthread_mutex_unlock( &ssh->libssh2_lock );
    return (ssize_t)total;
}

ssize_t ssh_socket_recv(ssh_socket_t *ssh, void *buf, size_t len)
{
    if (!ssh || !buf || len == 0) {
        return -1;
    }

    if (!ssh->raw_mode) {
        char out[700];

        if (ssh_advance_control(ssh) < 0) {
            snprintf(out, sizeof(out), "ERROR %s\r\n", ssh->last_error);
            ssh->state = SSH_STATE_ERROR;
            return ssh_emit_line(ssh, buf, len, out);
        }

        switch (ssh->state) {
        case SSH_STATE_EMIT_KEYGEN:
            ssh->state = SSH_STATE_EMIT_IDENTITY;
            return ssh_emit_line(ssh, buf, len, "KEY-GENERATION\r\n");
        case SSH_STATE_EMIT_IDENTITY:
            snprintf(out, sizeof(out), "IDENTITY %s\r\n", ssh->identity_public_line);
            ssh->state = SSH_STATE_CHECK_HOST;
            return ssh_emit_line(ssh, buf, len, out);
        case SSH_STATE_EMIT_TRUST:
            snprintf(out, sizeof(out), "TRUST? %s\r\n", ssh->fingerprint);
            ssh->prompt_pending = true;
            ssh->state = SSH_STATE_WAIT_TRUST;
            return ssh_emit_line(ssh, buf, len, out);
        case SSH_STATE_EMIT_USER:
            ssh->prompt_pending = true;
            ssh->state = SSH_STATE_WAIT_USER;
            return ssh_emit_line(ssh, buf, len, "USER?\r\n");
        case SSH_STATE_EMIT_PASSWORD:
            ssh->prompt_pending = true;
            ssh->state = SSH_STATE_WAIT_PASSWORD;
            return ssh_emit_line(ssh, buf, len, "PASSWORD?\r\n");
        case SSH_STATE_EMIT_CONNECTED:
            ssh->raw_mode = true;
            ssh->state = SSH_STATE_RAW;
            return ssh_emit_line(ssh, buf, len, "CONNECTED\r\n");
        case SSH_STATE_ERROR:
            if (ssh->error_emitted) {
                return 0;
            }
            snprintf(out, sizeof(out), "ERROR %s\r\n", ssh->last_error);
            ssh->error_emitted = true;
            ssh_socket_close(ssh);
            return ssh_emit_line(ssh, buf, len, out);
        default:
            return -2;
        }
    }

    pthread_mutex_lock( &ssh->libssh2_lock );
    ssize_t rc = libssh2_channel_read(ssh->channel, (char *)buf, len);
    if (rc == LIBSSH2_ERROR_EAGAIN) {
        pthread_mutex_unlock( &ssh->libssh2_lock );
        ssh_wait_socket(ssh->session, (int)ssh->socket_fd);
        return -2;
    }
    if (rc == 0 && libssh2_channel_eof(ssh->channel)) {
        pthread_mutex_unlock( &ssh->libssh2_lock );
        return 0;
    }
    if (rc < 0) {
        ssh_set_error(ssh, "read_failed");
    }
    pthread_mutex_unlock( &ssh->libssh2_lock );
    return rc;
}

int ssh_socket_close(ssh_socket_t *ssh)
{
    if (!ssh) {
        return -1;
    }

    pthread_mutex_lock( &ssh->line_mutex );
    ssh->line_ready = true;
    ssh->prompt_pending = false;
    pthread_cond_broadcast( &ssh->line_cond );
    pthread_mutex_unlock( &ssh->line_mutex );

    pthread_mutex_lock( &ssh->libssh2_lock );
    if (ssh->session) {
        libssh2_session_set_blocking(ssh->session, 0);
    }

    if (ssh->channel) {
        int rc = LIBSSH2_ERROR_EAGAIN;
        {
          for (int i = 0; i < SSH_CLEANUP_RETRIES && rc == LIBSSH2_ERROR_EAGAIN; i++) {
              rc = ssh_cleanup_wait(ssh, libssh2_channel_close(ssh->channel));
          }
        }

        rc = LIBSSH2_ERROR_EAGAIN;
        {
          for (int i = 0; i < SSH_CLEANUP_RETRIES && rc == LIBSSH2_ERROR_EAGAIN; i++) {
              rc = ssh_cleanup_wait(ssh, libssh2_channel_free(ssh->channel));
          }
        }

        if (rc != LIBSSH2_ERROR_EAGAIN) {
            ssh->channel = NULL;
        }
    }

    if (ssh->session) {
        int rc = LIBSSH2_ERROR_EAGAIN;
        {
          for (int i = 0; i < SSH_CLEANUP_RETRIES && rc == LIBSSH2_ERROR_EAGAIN; i++) {
              rc = ssh_cleanup_wait(ssh, libssh2_session_disconnect(ssh->session, "closed"));
          }
        }

        rc = LIBSSH2_ERROR_EAGAIN;
        {
          for (int i = 0; i < SSH_CLEANUP_RETRIES && rc == LIBSSH2_ERROR_EAGAIN; i++) {
              rc = ssh_cleanup_wait(ssh, libssh2_session_free(ssh->session));
          }
        }

        if (rc != LIBSSH2_ERROR_EAGAIN) {
            ssh->session = NULL;
        }
    }
    pthread_mutex_unlock( &ssh->libssh2_lock );
    ssh->socket_fd = compat_socket_invalid;
    ssh->connected = false;
    ssh->raw_mode = false;
    return 0;
}

bool ssh_socket_connected(const ssh_socket_t *ssh)
{
    return ssh && ssh->connected;
}

bool ssh_socket_has_pending_control(const ssh_socket_t *ssh)
{
    if (!ssh || ssh->raw_mode) {
        return false;
    }
    if (ssh->state == SSH_STATE_ERROR && ssh->error_emitted) {
        return false;
    }

    switch (ssh->state) {
    case SSH_STATE_EMIT_KEYGEN:
    case SSH_STATE_EMIT_IDENTITY:
    case SSH_STATE_CHECK_HOST:
    case SSH_STATE_EMIT_TRUST:
    case SSH_STATE_AUTH_START:
    case SSH_STATE_AUTH_METHODS:
    case SSH_STATE_AUTH_PASSWORD:
    case SSH_STATE_OPEN_SHELL:
    case SSH_STATE_EMIT_USER:
    case SSH_STATE_EMIT_PASSWORD:
    case SSH_STATE_EMIT_CONNECTED:
    case SSH_STATE_ERROR:
        return true;
    default:
        return false;
    }
}

ssize_t ssh_socket_feed_control(ssh_socket_t *ssh, const uint8_t *buf, size_t len)
{
    if (!ssh || !buf || ssh->raw_mode) {
        return -1;
    }

    pthread_mutex_lock( &ssh->line_mutex );
    if (!ssh->prompt_pending) {
        pthread_mutex_unlock( &ssh->line_mutex );
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == '\r' || c == '\n') {
            size_t consumed = i + 1;
            if (c == '\r' && consumed < len && buf[consumed] == '\n') {
                consumed++;
            }
            ssh->line[ssh->line_len] = '\0';
            ssh->line_ready = true;
            ssh->prompt_pending = false;
            pthread_cond_signal( &ssh->line_cond );
            pthread_mutex_unlock( &ssh->line_mutex );
            return (ssize_t)consumed;
        }
        if (ssh->line_len < sizeof(ssh->line) - 1) {
            ssh->line[ssh->line_len++] = (char)c;
        }
    }
    pthread_mutex_unlock( &ssh->line_mutex );
    return (ssize_t)len;
}

const char *ssh_socket_last_error(const ssh_socket_t *ssh)
{
    return ssh ? ssh->last_error : "failure";
}
