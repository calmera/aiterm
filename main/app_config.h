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
#define CFG_SENTINEL     2048
#define CFG_TOKEN        160

typedef enum { CFG_PROVIDER_OPENAI = 0, CFG_PROVIDER_ANTHROPIC = 1 } cfg_provider_t;
typedef enum { CFG_MODE_AI = 0, CFG_MODE_SSH = 1, CFG_MODE_ROOMS = 2 } cfg_mode_t;

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
    int            context_window;  // tokens the model accepts (status-bar gauge), 0 = default 128k
    float          price_in;        // USD per 1M input tokens (0 = unknown -> status bar shows tokens)
    float          price_out;       // USD per 1M output tokens
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

// chronicle-agents rooms over the hosted chronicle (GitHub sign-in through the identity bridge).
typedef struct {
    bool    enabled;                       // rooms mode offered (set once a sign-in completed)
    char    site[CFG_URL];                 // https://chronicle.impire.dev (install profile at /.well-known/chronicle/profile.json)
    char    nats_url[CFG_URL];             // tls://connect.chronicle.impire.dev:4222 (from the profile)
    char    github_client_id[64];          // the install's GitHub App (from the profile)
    char    sentinel[CFG_SENTINEL];        // the bridge's sentinel credential (from the profile; public)
    char    access_token[CFG_TOKEN];       // GitHub user token (secret)
    char    refresh_token[CFG_TOKEN];      // GitHub refresh token (secret)
    int64_t token_expiry;                  // unix seconds the access token lapses, 0 = unknown
    char    login[CFG_STR];                // GitHub login of the signed-in person
    char    account[CFG_STR];              // chronicle account the badge lands in
    char    store[CFG_STR];                // rooms store, default "rooms"
} cfg_chronicle_t;

typedef struct {
    bool       dark;        // theme
    uint8_t    font_size;   // 0 small, 1 medium, 2 large
    cfg_mode_t default_mode;
    int8_t     ssh_host;    // index into ssh_hosts, -1 none
    char       timezone[64];  // POSIX TZ string for the clock (SNTP), e.g. CET-1CEST,M3.5.0,M10.5.0/3
} cfg_ui_t;

typedef struct {
    int              version;
    cfg_wifi_t       wifi;
    cfg_inference_t  inference;
    cfg_mcp_server_t mcp[CFG_MAX_MCP];
    int              mcp_count;
    cfg_ssh_host_t   ssh[CFG_MAX_SSH];
    int              ssh_count;
    cfg_chronicle_t  chronicle;
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
bool      app_config_has_chronicle(void);   // signed in: tokens, account and sentinel present
const char* app_config_mode_name(cfg_mode_t m);  // "ai" / "ssh" / "rooms"
void      app_config_mask_secret(const char* secret, char* out, int out_len);

#ifdef __cplusplus
}
#endif
