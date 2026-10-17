#include "github_auth.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "http_stream.h"
#include "util/jsonbuf.h"
#include "util/platform.h"

static const char* TAG = "github";
#define GH_OAUTH "https://github.com"

static void form_add(jsonbuf_t* b, const char* k, const char* v) {
    if (b->len) jb_append_char(b, '&');
    jb_append_str(b, k); jb_append_char(b, '=');
    for (const char* p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') jb_append_char(b, (char)c);
        else jb_appendf(b, "%%%02X", c);
    }
}
// POST a form, return the body (plat_malloc'd) or NULL with err.
static char* post_form(const char* url, const jsonbuf_t* form, char* err, size_t err_len) {
    http_header_t hdrs[] = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}, {"User-Agent", "ai-terminal"}};
    http_stream_req_t req = {.url = url, .method = "POST", .headers = hdrs, .n_headers = 3, .body = jb_cstr(form), .body_len = form->len,
                             .connect_timeout_ms = 15000, .read_timeout_ms = 15000, .total_timeout_ms = 20000};
    http_stream_res_t res; char* body = NULL;
    esp_err_t e = http_fetch(&req, &res, &body, 64 * 1024);
    if (e != ESP_OK) { snprintf(err, err_len, "%s", res.error[0] ? res.error : esp_err_to_name(e)); plat_free(body); return NULL; }
    if (res.status != 200) { snprintf(err, err_len, "GitHub answered HTTP %d", res.status); ESP_LOGW(TAG, "%s: %d %.120s", url, res.status, body ? body : ""); plat_free(body); return NULL; }
    if (!body) { snprintf(err, err_len, "empty reply"); return NULL; }
    return body;
}

esp_err_t github_device_start(const char* client_id, gh_device_t* out, char* err, size_t err_len) {
    err[0] = 0;
    jsonbuf_t f; jb_init(&f);
    form_add(&f, "client_id", client_id);
    char* body = post_form(GH_OAUTH "/login/device/code", &f, err, err_len);
    jb_free(&f);
    if (!body) return ESP_FAIL;
    bool ok = gh_parse_device_code(body, out);
    if (!ok) snprintf(err, err_len, "device flow refused (is the client id right, and the flow enabled on the App?)");
    plat_free(body);
    return ok ? ESP_OK : ESP_FAIL;
}

int github_device_poll(const char* client_id, const char* device_code, char* access, size_t an, char* refresh, size_t rn, int* expires_in, char* err, size_t err_len) {
    err[0] = 0;
    jsonbuf_t f; jb_init(&f);
    form_add(&f, "client_id", client_id);
    form_add(&f, "device_code", device_code);
    form_add(&f, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    char* body = post_form(GH_OAUTH "/login/oauth/access_token", &f, err, err_len);
    jb_free(&f);
    if (!body) return -1;
    int r = gh_parse_token(body, access, an, refresh, rn, expires_in, err, err_len);
    plat_free(body);
    return r;
}

int github_refresh(const char* client_id, const char* refresh_token, char* access, size_t an, char* refresh, size_t rn, int* expires_in, char* err, size_t err_len) {
    err[0] = 0;
    jsonbuf_t f; jb_init(&f);
    form_add(&f, "client_id", client_id);
    form_add(&f, "refresh_token", refresh_token);
    form_add(&f, "grant_type", "refresh_token");
    char* body = post_form(GH_OAUTH "/login/oauth/access_token", &f, err, err_len);
    jb_free(&f);
    if (!body) return -1;
    int r = gh_parse_token(body, access, an, refresh, rn, expires_in, err, err_len);
    plat_free(body);
    if (r == 0) { snprintf(err, err_len, "refresh pending?"); r = -1; }
    return r;
}
