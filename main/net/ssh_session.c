// SSH client session over wolfSSH (wolfCrypt), password auth, "xterm" PTY shell.
// A dedicated task owns the socket and the wolfSSH object: it runs the blocking handshake,
// then pumps bytes both ways with select() and non-blocking I/O callbacks.
#include "ssh_session.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "ssh";

#if CONFIG_AITERM_SSH
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wolfssh/error.h>
#include <wolfssh/ssh.h>

static ssh_callbacks_t      s_cb;
static volatile ssh_state_t s_state = SSH_ST_IDLE;
static volatile bool        s_stop = false;
static RingbufHandle_t      s_tx = NULL;    // terminal -> remote
static volatile int         s_cols = 80, s_rows = 24;
static volatile bool        s_resize = false;
static char                 s_host[128], s_user[64], s_pass[128];
static uint16_t             s_port = 22;
static TaskHandle_t         s_task = NULL;
static int                  s_sock = -1;
static volatile bool        s_sock_closed = false;

static void set_state(ssh_state_t st, const char* msg) {
    s_state = st;
    if (s_cb.on_state) s_cb.on_state(st, msg ? msg : "", s_cb.user);
}

// ---- wolfSSH I/O callbacks (socket is blocking during the handshake, non-blocking after) ----
static int io_recv(WOLFSSH* ssh, void* buf, word32 sz, void* ctx) {
    (void)ssh; (void)ctx;
    if (s_sock < 0) return WS_SOCKET_ERROR_E;
    int n = (int)recv(s_sock, buf, sz, 0);
    if (n > 0) return n;
    if (n == 0) { s_sock_closed = true; return WS_CBIO_ERR_CONN_CLOSE; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return WS_CBIO_ERR_WANT_READ;
    if (errno == EINTR) return WS_CBIO_ERR_WANT_READ;
    s_sock_closed = true;
    return WS_CBIO_ERR_GENERAL;
}
static int io_send(WOLFSSH* ssh, void* buf, word32 sz, void* ctx) {
    (void)ssh; (void)ctx;
    if (s_sock < 0) return WS_SOCKET_ERROR_E;
    int n = (int)send(s_sock, buf, sz, 0);
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return WS_CBIO_ERR_WANT_WRITE;
    s_sock_closed = true;
    return WS_CBIO_ERR_GENERAL;
}
static int user_auth(byte authType, WS_UserAuthData* authData, void* ctx) {
    (void)ctx;
    if (authType == WOLFSSH_USERAUTH_PASSWORD) {
        authData->sf.password.password = (byte*)s_pass;
        authData->sf.password.passwordSz = (word32)strlen(s_pass);
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_FAILURE;
}
static int pubkey_check(const byte* pubKey, word32 pubKeySz, void* ctx) {
    (void)ctx;
    // No known_hosts store on the badge: log a short fingerprint-ish digest and accept (DECISIONS.md #18).
    uint32_t h = 2166136261u;
    for (word32 i = 0; i < pubKeySz; i++) h = (h ^ pubKey[i]) * 16777619u;
    ESP_LOGI(TAG, "server host key: %u bytes, fnv1a %08x", (unsigned)pubKeySz, (unsigned)h);
    return WS_SUCCESS;
}

static int tcp_connect(const char* host, uint16_t port, char* err, size_t err_len) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo* res = NULL;
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) { snprintf(err, err_len, "cannot resolve %s", host); return -1; }
    int sock = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        struct timeval tv = {.tv_sec = 20, .tv_usec = 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(sock); sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) snprintf(err, err_len, "connection to %s:%u failed", host, port);
    return sock;
}

static void ssh_task(void* arg) {
    (void)arg;
    char err[160] = "";
    WOLFSSH_CTX* ctx = NULL;
    WOLFSSH* ssh = NULL;
    s_sock_closed = false;
    set_state(SSH_ST_CONNECTING, "connecting");
    if (wolfSSH_Init() != WS_SUCCESS) { set_state(SSH_ST_ERROR, "wolfSSH init failed"); goto out; }
    s_sock = tcp_connect(s_host, s_port, err, sizeof(err));
    if (s_sock < 0) { set_state(SSH_ST_ERROR, err); goto out; }
    ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (!ctx) { set_state(SSH_ST_ERROR, "ssh context failed"); goto out; }
    wolfSSH_SetUserAuth(ctx, user_auth);
    wolfSSH_CTX_SetPublicKeyCheck(ctx, pubkey_check);
    wolfSSH_SetIORecv(ctx, io_recv);
    wolfSSH_SetIOSend(ctx, io_send);
    ssh = wolfSSH_new(ctx);
    if (!ssh) { set_state(SSH_ST_ERROR, "ssh session failed"); goto out; }
    wolfSSH_SetUserAuthCtx(ssh, NULL);
    wolfSSH_SetPublicKeyCheckCtx(ssh, NULL);
    wolfSSH_SetIOReadCtx(ssh, NULL);
    wolfSSH_SetIOWriteCtx(ssh, NULL);
    wolfSSH_set_fd(ssh, s_sock);
    if (wolfSSH_SetUsername(ssh, s_user) != WS_SUCCESS) { set_state(SSH_ST_ERROR, "bad user name"); goto out; }
    if (wolfSSH_SetChannelType(ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0) != WS_SUCCESS) { set_state(SSH_ST_ERROR, "channel type failed"); goto out; }
    set_state(SSH_ST_AUTH, "authenticating");
    int ret = wolfSSH_connect(ssh);  // blocking: handshake + auth + channel + pty + shell
    if (ret != WS_SUCCESS) {
        int code = wolfSSH_get_error(ssh);
        const char* name = wolfSSH_get_error_name(ssh);
        if (code == WS_USER_AUTH_E) snprintf(err, sizeof(err), "authentication failed (wrong password?)");
        else snprintf(err, sizeof(err), "handshake failed: %s (%d)", name ? name : "?", code);
        set_state(SSH_ST_ERROR, err); goto out;
    }
    wolfSSH_ChangeTerminalSize(ssh, (word32)s_cols, (word32)s_rows, 0, 0);
    // switch to non-blocking I/O for the pump
    int fl = fcntl(s_sock, F_GETFL, 0);
    fcntl(s_sock, F_SETFL, fl | O_NONBLOCK);
    set_state(SSH_ST_OPEN, "connected");
    uint8_t buf[1024];
    while (!s_stop && !s_sock_closed) {
        bool activity = false;
        // remote -> terminal (drain what is available)
        for (int k = 0; k < 8; k++) {
            int n = wolfSSH_stream_read(ssh, buf, sizeof(buf));
            if (n > 0) { if (s_cb.on_data) s_cb.on_data(buf, (size_t)n, s_cb.user); activity = true; continue; }
            int e = wolfSSH_get_error(ssh);
            if (n == WS_EOF || e == WS_EOF || e == WS_CHANNEL_CLOSED || e == WS_SOCKET_ERROR_E) { s_sock_closed = true; }
            break;  // WS_WANT_READ or error
        }
        if (s_sock_closed) break;
        // terminal -> remote
        size_t len = 0;
        uint8_t* item = (uint8_t*)xRingbufferReceiveUpTo(s_tx, &len, 0, 512);
        if (item) {
            size_t off = 0; int tries = 0;
            while (off < len && !s_stop && tries < 200) {
                int w = wolfSSH_stream_send(ssh, item + off, (word32)(len - off));
                if (w > 0) { off += (size_t)w; continue; }
                int e = wolfSSH_get_error(ssh);
                if (e == WS_WANT_WRITE || e == WS_WANT_READ) { tries++; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                s_sock_closed = true; break;
            }
            vRingbufferReturnItem(s_tx, item);
            activity = true;
        }
        if (s_resize) { s_resize = false; wolfSSH_ChangeTerminalSize(ssh, (word32)s_cols, (word32)s_rows, 0, 0); }
        if (!activity) {
            fd_set rfd; FD_ZERO(&rfd); FD_SET(s_sock, &rfd);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
            select(s_sock + 1, &rfd, NULL, NULL, &tv);
        }
    }
    set_state(SSH_ST_CLOSED, s_stop ? "disconnected" : "connection closed");
out:
    if (ssh) { wolfSSH_shutdown(ssh); wolfSSH_free(ssh); }
    if (ctx) wolfSSH_CTX_free(ctx);
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    wolfSSH_Cleanup();
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ssh_session_start(const char* host, uint16_t port, const char* user, const char* password, int cols, int rows, const ssh_callbacks_t* cb) {
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_cb = *cb;
    strlcpy(s_host, host, sizeof(s_host)); strlcpy(s_user, user, sizeof(s_user)); strlcpy(s_pass, password ? password : "", sizeof(s_pass));
    s_port = port ? port : 22; s_cols = cols; s_rows = rows; s_stop = false; s_resize = false;
    if (!s_tx) s_tx = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
    // wolfCrypt key exchange is stack hungry (SMALL_STACK helps, but DH/RSA still need room).
    BaseType_t r = xTaskCreatePinnedToCore(ssh_task, "ssh", 24576, NULL, 5, &s_task, 1);
    return r == pdPASS ? ESP_OK : ESP_FAIL;
}
void ssh_session_send(const uint8_t* data, size_t len) { if (s_tx && s_state == SSH_ST_OPEN) xRingbufferSend(s_tx, data, len, pdMS_TO_TICKS(100)); }
void ssh_session_resize(int cols, int rows) { s_cols = cols; s_rows = rows; s_resize = true; }
void ssh_session_close(void) { s_stop = true; }
ssh_state_t ssh_session_state(void) { return s_state; }
bool ssh_session_available(void) { return true; }

#else  // !CONFIG_AITERM_SSH

esp_err_t ssh_session_start(const char* host, uint16_t port, const char* user, const char* password, int cols, int rows, const ssh_callbacks_t* cb) {
    (void)host; (void)port; (void)user; (void)password; (void)cols; (void)rows;
    if (cb && cb->on_state) cb->on_state(SSH_ST_ERROR, "SSH is not built into this firmware (CONFIG_AITERM_SSH)", cb->user);
    ESP_LOGW(TAG, "SSH disabled at build time");
    return ESP_ERR_NOT_SUPPORTED;
}
void ssh_session_send(const uint8_t* data, size_t len) { (void)data; (void)len; }
void ssh_session_resize(int cols, int rows) { (void)cols; (void)rows; }
void ssh_session_close(void) {}
ssh_state_t ssh_session_state(void) { return SSH_ST_IDLE; }
bool ssh_session_available(void) { return false; }
#endif
