// Configuration model (§4) persisted as one JSON document in NVS namespace "aiterm", key "config".
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MAX_MCP      8
#define CFG_MAX_SSH      8
#define CFG_STR          128
#define CFG_URL          256
#define CFG_SECRET       512
#define CFG_PROMPT       2048

typedef enum { CFG_PROVIDER_OPENAI = 0, CFG_PROVIDER_ANTHROPIC = 1 } cfg_provider_t;
typedef enum { CFG_MODE_AI = 0, CFG_MODE_SSH = 1 } cfg_mode_t;

typedef struct {
    char ssid[33];
    char password[65];
} cfg_wifi_t;

typedef struct {
    cfg_provider_t provider;
    char           base_url[CFG_URL];
    char           api_key[CFG_SECRET];
    char           model[CFG_STR];
    char           system_prompt[CFG_PROMPT];
} cfg_inference_t;

typedef struct {
    char name[CFG_STR];
    char url[CFG_URL];
    char auth_header[CFG_STR];
    char auth_value[CFG_SECRET];
    bool enabled;
} cfg_mcp_server_t;

typedef struct {
    char     name[CFG_STR];
    char     host[CFG_URL];
    uint16_t port;
    char     user[CFG_STR];
    char     password[CFG_SECRET];
} cfg_ssh_host_t;

typedef struct {
    bool       dark;        // theme
    uint8_t    font_size;   // 0 small, 1 medium, 2 large
    cfg_mode_t default_mode;
    int8_t     ssh_host;    // index into ssh_hosts, -1 none
} cfg_ui_t;

typedef struct {
    int              version;
    cfg_wifi_t       wifi;
    cfg_inference_t  inference;
    cfg_mcp_server_t mcp[CFG_MAX_MCP];
    int              mcp_count;
    cfg_ssh_host_t   ssh[CFG_MAX_SSH];
    int              ssh_count;
    cfg_ui_t         ui;
} app_config_t;

// The live configuration (owned by app_config; copy fields out while holding no lock —
// writers only run on the UI task or during provisioning HTTP handlers).
app_config_t* app_config(void);

void      app_config_set_defaults(app_config_t* c);
esp_err_t app_config_load(void);     // NVS -> app_config()  (defaults when missing)
esp_err_t app_config_save(void);     // app_config() -> NVS
esp_err_t app_config_reset(void);    // erase + defaults

// JSON import/export (§4 format). Export: caller frees. mask_secrets replaces secrets by "••••abcd".
char*     app_config_export_json(const app_config_t* c, bool mask_secrets);
// Merge a JSON document into c. Keys that are absent keep their value; masked secrets
// ("••••xxxx" or empty strings for secrets when keep_masked is true) are left unchanged.
esp_err_t app_config_import_json(app_config_t* c, const char* json, bool keep_masked);

bool      app_config_has_wifi(void);
bool      app_config_has_inference(void);
void      app_config_mask_secret(const char* secret, char* out, int out_len);

#ifdef __cplusplus
}
#endif
