#include "app_config.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "util/platform.h"

static const char* TAG = "config";
static app_config_t s_cfg;

app_config_t* app_config(void) { return &s_cfg; }

void app_config_set_defaults(app_config_t* c) {
    memset(c, 0, sizeof(*c));
    c->version = 1;
    c->inference.provider = CFG_PROVIDER_OPENAI;
    strlcpy(c->inference.base_url, "https://api.openai.com/v1", sizeof(c->inference.base_url));
    strlcpy(c->inference.model, "gpt-4.1-mini", sizeof(c->inference.model));
    c->ui.dark = true;
    c->ui.font_size = 1;
    c->ui.default_mode = CFG_MODE_AI;
    c->ui.ssh_host = -1;
}

void app_config_mask_secret(const char* secret, char* out, int out_len) {
    size_t n = strlen(secret);
    if (n == 0) { out[0] = 0; return; }
    if (n <= 4) { snprintf(out, out_len, "••••"); return; }
    snprintf(out, out_len, "••••%s", secret + n - 4);
}
static bool is_masked(const char* s) { return s && strncmp(s, "••••", strlen("••••")) == 0; }

static void jget_str(cJSON* o, const char* key, char* dst, size_t n) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(dst, v->valuestring, n);
}
static void jget_secret(cJSON* o, const char* key, char* dst, size_t n, bool keep_masked) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || !v->valuestring) return;
    if (keep_masked && is_masked(v->valuestring)) return;
    strlcpy(dst, v->valuestring, n);
}
static void jget_bool(cJSON* o, const char* key, bool* dst) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}
static void jget_int(cJSON* o, const char* key, int* dst) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v)) *dst = v->valueint;
}

esp_err_t app_config_import_json(app_config_t* c, const char* json, bool keep_masked) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;
    cJSON* o;
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "wifi")) && cJSON_IsObject(o)) {
        jget_str(o, "ssid", c->wifi.ssid, sizeof(c->wifi.ssid));
        jget_secret(o, "password", c->wifi.password, sizeof(c->wifi.password), keep_masked);
    }
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "inference")) && cJSON_IsObject(o)) {
        char prov[32] = "";
        jget_str(o, "provider", prov, sizeof(prov));
        if (prov[0]) c->inference.provider = (strcmp(prov, "anthropic") == 0) ? CFG_PROVIDER_ANTHROPIC : CFG_PROVIDER_OPENAI;
        jget_str(o, "base_url", c->inference.base_url, sizeof(c->inference.base_url));
        jget_secret(o, "api_key", c->inference.api_key, sizeof(c->inference.api_key), keep_masked);
        jget_str(o, "model", c->inference.model, sizeof(c->inference.model));
        jget_str(o, "system_prompt", c->inference.system_prompt, sizeof(c->inference.system_prompt));
        // strip trailing slash from base_url
        size_t n = strlen(c->inference.base_url);
        while (n > 0 && c->inference.base_url[n - 1] == '/') c->inference.base_url[--n] = 0;
    }
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "mcp_servers")) && cJSON_IsArray(o)) {
        // Replace the list; preserve the auth value of an entry with the same name+url when masked.
        cfg_mcp_server_t old[CFG_MAX_MCP];
        int old_n = c->mcp_count;
        memcpy(old, c->mcp, sizeof(old));
        c->mcp_count = 0;
        cJSON* e;
        cJSON_ArrayForEach(e, o) {
            if (!cJSON_IsObject(e) || c->mcp_count >= CFG_MAX_MCP) continue;
            cfg_mcp_server_t* m = &c->mcp[c->mcp_count];
            memset(m, 0, sizeof(*m));
            m->enabled = true;
            strlcpy(m->auth_header, "Authorization", sizeof(m->auth_header));
            jget_str(e, "name", m->name, sizeof(m->name));
            jget_str(e, "url", m->url, sizeof(m->url));
            jget_str(e, "auth_header", m->auth_header, sizeof(m->auth_header));
            jget_bool(e, "enabled", &m->enabled);
            cJSON* av = cJSON_GetObjectItemCaseSensitive(e, "auth_value");
            if (cJSON_IsString(av) && av->valuestring) {
                if (keep_masked && is_masked(av->valuestring)) {
                    for (int i = 0; i < old_n; i++)
                        if (strcmp(old[i].url, m->url) == 0) { strlcpy(m->auth_value, old[i].auth_value, sizeof(m->auth_value)); break; }
                } else {
                    strlcpy(m->auth_value, av->valuestring, sizeof(m->auth_value));
                }
            }
            if (m->url[0]) c->mcp_count++;
        }
    }
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "ssh_hosts")) && cJSON_IsArray(o)) {
        cfg_ssh_host_t old[CFG_MAX_SSH];
        int old_n = c->ssh_count;
        memcpy(old, c->ssh, sizeof(old));
        c->ssh_count = 0;
        cJSON* e;
        cJSON_ArrayForEach(e, o) {
            if (!cJSON_IsObject(e) || c->ssh_count >= CFG_MAX_SSH) continue;
            cfg_ssh_host_t* h = &c->ssh[c->ssh_count];
            memset(h, 0, sizeof(*h));
            h->port = 22;
            jget_str(e, "name", h->name, sizeof(h->name));
            jget_str(e, "host", h->host, sizeof(h->host));
            int port = 22; jget_int(e, "port", &port); h->port = (uint16_t)(port > 0 && port < 65536 ? port : 22);
            jget_str(e, "user", h->user, sizeof(h->user));
            cJSON* pw = cJSON_GetObjectItemCaseSensitive(e, "password");
            if (cJSON_IsString(pw) && pw->valuestring) {
                if (keep_masked && is_masked(pw->valuestring)) {
                    for (int i = 0; i < old_n; i++)
                        if (strcmp(old[i].host, h->host) == 0 && strcmp(old[i].user, h->user) == 0) { strlcpy(h->password, old[i].password, sizeof(h->password)); break; }
                } else {
                    strlcpy(h->password, pw->valuestring, sizeof(h->password));
                }
            }
            if (h->host[0]) c->ssh_count++;
        }
        if (c->ui.ssh_host >= c->ssh_count) c->ui.ssh_host = c->ssh_count ? 0 : -1;
    }
    if ((o = cJSON_GetObjectItemCaseSensitive(root, "ui")) && cJSON_IsObject(o)) {
        char s[32] = "";
        jget_str(o, "theme", s, sizeof(s));
        if (s[0]) c->ui.dark = strcmp(s, "light") != 0;
        s[0] = 0; jget_str(o, "font_size", s, sizeof(s));
        if (s[0]) c->ui.font_size = strcmp(s, "small") == 0 ? 0 : strcmp(s, "large") == 0 ? 2 : 1;
        s[0] = 0; jget_str(o, "default_mode", s, sizeof(s));
        if (s[0]) c->ui.default_mode = strcmp(s, "ssh") == 0 ? CFG_MODE_SSH : CFG_MODE_AI;
        int sh = c->ui.ssh_host; jget_int(o, "ssh_host", &sh);
        c->ui.ssh_host = (int8_t)(sh >= -1 && sh < c->ssh_count ? sh : (c->ssh_count ? 0 : -1));
    }
    cJSON_Delete(root);
    return ESP_OK;
}

char* app_config_export_json(const app_config_t* c, bool mask) {
    char m[CFG_SECRET];
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON* w = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(w, "ssid", c->wifi.ssid);
    if (mask) { app_config_mask_secret(c->wifi.password, m, sizeof(m)); cJSON_AddStringToObject(w, "password", m); }
    else cJSON_AddStringToObject(w, "password", c->wifi.password);
    cJSON* inf = cJSON_AddObjectToObject(root, "inference");
    cJSON_AddStringToObject(inf, "provider", c->inference.provider == CFG_PROVIDER_ANTHROPIC ? "anthropic" : "openai");
    cJSON_AddStringToObject(inf, "base_url", c->inference.base_url);
    if (mask) { app_config_mask_secret(c->inference.api_key, m, sizeof(m)); cJSON_AddStringToObject(inf, "api_key", m); }
    else cJSON_AddStringToObject(inf, "api_key", c->inference.api_key);
    cJSON_AddStringToObject(inf, "model", c->inference.model);
    cJSON_AddStringToObject(inf, "system_prompt", c->inference.system_prompt);
    cJSON* arr = cJSON_AddArrayToObject(root, "mcp_servers");
    for (int i = 0; i < c->mcp_count; i++) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", c->mcp[i].name);
        cJSON_AddStringToObject(e, "url", c->mcp[i].url);
        cJSON_AddStringToObject(e, "auth_header", c->mcp[i].auth_header);
        if (mask) { app_config_mask_secret(c->mcp[i].auth_value, m, sizeof(m)); cJSON_AddStringToObject(e, "auth_value", m); }
        else cJSON_AddStringToObject(e, "auth_value", c->mcp[i].auth_value);
        cJSON_AddBoolToObject(e, "enabled", c->mcp[i].enabled);
        cJSON_AddItemToArray(arr, e);
    }
    arr = cJSON_AddArrayToObject(root, "ssh_hosts");
    for (int i = 0; i < c->ssh_count; i++) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", c->ssh[i].name);
        cJSON_AddStringToObject(e, "host", c->ssh[i].host);
        cJSON_AddNumberToObject(e, "port", c->ssh[i].port);
        cJSON_AddStringToObject(e, "user", c->ssh[i].user);
        if (mask) { app_config_mask_secret(c->ssh[i].password, m, sizeof(m)); cJSON_AddStringToObject(e, "password", m); }
        else cJSON_AddStringToObject(e, "password", c->ssh[i].password);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON* ui = cJSON_AddObjectToObject(root, "ui");
    cJSON_AddStringToObject(ui, "theme", c->ui.dark ? "dark" : "light");
    cJSON_AddStringToObject(ui, "font_size", c->ui.font_size == 0 ? "small" : c->ui.font_size == 2 ? "large" : "medium");
    cJSON_AddStringToObject(ui, "default_mode", c->ui.default_mode == CFG_MODE_SSH ? "ssh" : "ai");
    cJSON_AddNumberToObject(ui, "ssh_host", c->ui.ssh_host);
    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;  // cJSON uses malloc; caller frees with free()
}

esp_err_t app_config_load(void) {
    app_config_set_defaults(&s_cfg);
    nvs_handle_t h;
    esp_err_t res = nvs_open("aiterm", NVS_READONLY, &h);
    if (res != ESP_OK) { ESP_LOGI(TAG, "no stored config (%s)", esp_err_to_name(res)); return ESP_OK; }
    size_t len = 0;
    res = nvs_get_blob(h, "config", NULL, &len);
    if (res == ESP_OK && len > 0) {
        char* buf = (char*)plat_malloc(len + 1);
        if (buf && nvs_get_blob(h, "config", buf, &len) == ESP_OK) {
            buf[len] = 0;
            if (app_config_import_json(&s_cfg, buf, false) != ESP_OK) ESP_LOGW(TAG, "stored config is not valid JSON");
        }
        plat_free(buf);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: wifi=%s provider=%d model=%s mcp=%d ssh=%d", s_cfg.wifi.ssid[0] ? s_cfg.wifi.ssid : "(none)",
             s_cfg.inference.provider, s_cfg.inference.model, s_cfg.mcp_count, s_cfg.ssh_count);
    return ESP_OK;
}

esp_err_t app_config_save(void) {
    char* json = app_config_export_json(&s_cfg, false);
    if (!json) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    esp_err_t res = nvs_open("aiterm", NVS_READWRITE, &h);
    if (res == ESP_OK) {
        res = nvs_set_blob(h, "config", json, strlen(json));
        if (res == ESP_OK) res = nvs_commit(h);
        nvs_close(h);
    }
    free(json);
    if (res != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(res));
    return res;
}

esp_err_t app_config_reset(void) {
    nvs_handle_t h;
    esp_err_t res = nvs_open("aiterm", NVS_READWRITE, &h);
    if (res == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    app_config_set_defaults(&s_cfg);
    return res;
}

bool app_config_has_wifi(void) { return s_cfg.wifi.ssid[0] != 0; }
bool app_config_has_inference(void) { return s_cfg.inference.base_url[0] && s_cfg.inference.model[0]; }
