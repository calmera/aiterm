#include "http_stream.h"
#include <errno.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "util/jsonbuf.h"
#include "util/platform.h"

static const char* TAG = "http";

typedef struct {
    const http_stream_req_t* req;
    http_stream_res_t*       res;
} ctx_t;

static esp_err_t event_handler(esp_http_client_event_t* evt) {
    ctx_t* c = (ctx_t*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && c) {
        if (evt->header_key && evt->header_value) {
            if (strcasecmp(evt->header_key, "Content-Type") == 0)
                strlcpy(c->res->content_type, evt->header_value, sizeof(c->res->content_type));
            if (c->req->on_header) c->req->on_header(evt->header_key, evt->header_value, c->req->user);
        }
    }
    return ESP_OK;
}

esp_err_t http_stream_run(const http_stream_req_t* req, http_stream_res_t* res) {
    memset(res, 0, sizeof(*res));
    ctx_t ctx = {.req = req, .res = res};
    bool is_post = req->method && strcasecmp(req->method, "POST") == 0;
    esp_http_client_config_t cfg = {
        .url                   = req->url,
        .method                = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms            = req->connect_timeout_ms > 0 ? req->connect_timeout_ms : 15000,
        .event_handler         = event_handler,
        .user_data             = &ctx,
        .buffer_size           = 4096,
        .buffer_size_tx        = 4096,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
        .keep_alive_enable     = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { snprintf(res->error, sizeof(res->error), "invalid URL"); return ESP_ERR_INVALID_ARG; }
    for (int i = 0; i < req->n_headers; i++) {
        if (req->headers[i].name && req->headers[i].value && req->headers[i].value[0])
            esp_http_client_set_header(client, req->headers[i].name, req->headers[i].value);
    }
    if (is_post && !req->body) esp_http_client_set_header(client, "Content-Length", "0");
    esp_err_t err = esp_http_client_open(client, is_post && req->body ? (int)req->body_len : 0);
    if (err != ESP_OK) {
        snprintf(res->error, sizeof(res->error), "connect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    if (is_post && req->body && req->body_len) {
        size_t off = 0;
        while (off < req->body_len) {
            int w = esp_http_client_write(client, req->body + off, (int)(req->body_len - off));
            if (w < 0) { snprintf(res->error, sizeof(res->error), "write failed"); esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_FAIL; }
            off += (size_t)w;
        }
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0 && content_length != -ESP_ERR_HTTP_EAGAIN) {
        snprintf(res->error, sizeof(res->error), "no response headers");
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    res->status = esp_http_client_get_status_code(client);
    int read_timeout = req->read_timeout_ms > 0 ? req->read_timeout_ms : 30000;
    esp_http_client_set_timeout_ms(client, read_timeout > 2000 ? 2000 : read_timeout);  // short reads, we poll abort
    int64_t start = esp_timer_get_time() / 1000;
    int64_t last_data = start;
    char* buf = (char*)plat_malloc(4096);
    if (!buf) { esp_http_client_close(client); esp_http_client_cleanup(client); return ESP_ERR_NO_MEM; }
    esp_err_t result = ESP_OK;
    while (1) {
        if (req->abort_flag && *req->abort_flag) { res->aborted = true; break; }
        if (req->total_timeout_ms > 0 && (esp_timer_get_time() / 1000 - start) > req->total_timeout_ms) {
            snprintf(res->error, sizeof(res->error), "timeout"); result = ESP_ERR_TIMEOUT; break;
        }
        errno = 0;
        int r = esp_http_client_read(client, buf, 4096);
        if (r > 0) {
            res->bytes += (size_t)r;
            last_data = esp_timer_get_time() / 1000;
            if (req->on_data) req->on_data(buf, (size_t)r, req->user);
            continue;
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                if ((esp_timer_get_time() / 1000 - last_data) > read_timeout) { snprintf(res->error, sizeof(res->error), "read timeout"); result = ESP_ERR_TIMEOUT; break; }
                continue;
            }
            if (esp_http_client_is_complete_data_received(client)) break;
            snprintf(res->error, sizeof(res->error), "read error (errno %d)", errno);
            result = res->bytes > 0 ? ESP_OK : ESP_FAIL;  // treat a torn connection after data as EOF
            break;
        }
        // r == 0
        if (esp_http_client_is_complete_data_received(client)) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
            if ((esp_timer_get_time() / 1000 - last_data) > read_timeout) { snprintf(res->error, sizeof(res->error), "read timeout"); result = ESP_ERR_TIMEOUT; break; }
            continue;
        }
        break;  // connection closed by the peer: EOF
    }
    plat_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (res->aborted) { snprintf(res->error, sizeof(res->error), "aborted"); return ESP_ERR_INVALID_STATE; }
    return result;
}

typedef struct { jsonbuf_t body; size_t max; void (*inner)(const char*, size_t, void*); void* inner_user; } collect_t;
static void collect_cb(const char* data, size_t len, void* user) {
    collect_t* c = (collect_t*)user;
    if (c->body.len + len <= c->max) jb_append(&c->body, data, len);
}

esp_err_t http_fetch(const http_stream_req_t* req, http_stream_res_t* res, char** out_body, size_t max_len) {
    collect_t c = {.max = max_len ? max_len : (1u << 20)};
    jb_init(&c.body);
    http_stream_req_t r = *req;
    r.on_data = collect_cb;
    r.user = &c;
    esp_err_t err = http_stream_run(&r, res);
    *out_body = jb_detach(&c.body);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s %s -> %s (%s)", r.method ? r.method : "GET", r.url, esp_err_to_name(err), res->error);
    return err;
}
