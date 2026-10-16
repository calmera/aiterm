// render_image: fetch (url) or decode (base64) PNG/JPEG via LVGL's lodepng/tjpgd decoders,
// scaled to fit and centred. The fetch runs on the agent task before the view is shown.
#include <stdio.h>
#include <string.h>
#include "mbedtls/base64.h"
#include "net/http_stream.h"
#include "render_tools.h"
#include "util/platform.h"
#include "views.h"

static ui_view_t     s_view;
static lv_image_dsc_t s_dsc;
static uint8_t*      s_data = NULL;
static lv_obj_t*     s_img = NULL;
static char          s_result[160];

#define IMG_MAX_BYTES (6u << 20)

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) { render_tool_finish(s_result); ui_pop(); }
}
static void on_close(ui_view_t* v) {
    (void)v;
    render_tool_finish(s_result);
    if (s_img) { lv_image_cache_drop(&s_dsc); s_img = NULL; }
    plat_free(s_data); s_data = NULL;
}

static bool looks_png(const uint8_t* d, size_t n) { return n > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G'; }
static bool looks_jpeg(const uint8_t* d, size_t n) { return n > 3 && d[0] == 0xFF && d[1] == 0xD8; }

void view_image_show(const char* url, const char* base64, const char* format, const char* title) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "image"; s_view.on_key = on_key; s_view.on_close = on_close;
    snprintf(s_result, sizeof(s_result), "{\"status\":\"shown\"}");
    plat_free(s_data); s_data = NULL;
    size_t len = 0;
    char err[128] = "";
    if (base64 && *base64) {
        const char* b = base64;
        const char* comma = strstr(b, "base64,");
        if (comma) b = comma + 7;
        size_t blen = strlen(b), olen = 0;
        s_data = (uint8_t*)plat_malloc(blen * 3 / 4 + 4);
        if (!s_data || mbedtls_base64_decode(s_data, blen * 3 / 4 + 4, &olen, (const unsigned char*)b, blen) != 0) { snprintf(err, sizeof(err), "invalid base64 data"); plat_free(s_data); s_data = NULL; }
        else len = olen;
    } else if (url && *url) {
        http_stream_req_t req = {.url = url, .method = "GET", .connect_timeout_ms = 15000, .read_timeout_ms = 20000, .total_timeout_ms = 60000};
        http_stream_res_t res; char* body = NULL;
        esp_err_t e = http_fetch(&req, &res, &body, IMG_MAX_BYTES);
        if (e != ESP_OK || res.status >= 400) { snprintf(err, sizeof(err), "download failed: %s", res.error[0] ? res.error : "HTTP error"); plat_free(body); }
        else { s_data = (uint8_t*)body; len = res.bytes > IMG_MAX_BYTES ? IMG_MAX_BYTES : res.bytes; }
    }
    char status[160];
    snprintf(status, sizeof(status), "%s · Esc close", title ? title : "Image");
    lv_obj_t* c = ui_view_create_root(&s_view, status);
    const ui_theme_t* th = ui_theme();
    if (!err[0] && s_data) {
        bool png = looks_png(s_data, len), jpg = looks_jpeg(s_data, len);
        (void)format;
        if (!png && !jpg) snprintf(err, sizeof(err), "unsupported image format (need PNG or JPEG)");
        else {
            memset(&s_dsc, 0, sizeof(s_dsc));
            s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_dsc.header.cf = LV_COLOR_FORMAT_RAW;
            s_dsc.data = s_data;
            s_dsc.data_size = (uint32_t)len;
            lv_image_header_t hdr;
            if (lv_image_decoder_get_info(&s_dsc, &hdr) != LV_RESULT_OK || hdr.w == 0 || hdr.h == 0) snprintf(err, sizeof(err), "could not decode image header");
            else if (hdr.w > 2048 || hdr.h > 2048) snprintf(err, sizeof(err), "image too large (%ux%u, max 2048x2048)", (unsigned)hdr.w, (unsigned)hdr.h);
            else {
                s_dsc.header.w = hdr.w; s_dsc.header.h = hdr.h;
                s_img = lv_image_create(c);
                lv_image_set_src(s_img, &s_dsc);
                int aw = lv_obj_get_width(c) - 8, ah = lv_obj_get_height(c) - 8;
                uint32_t zx = (uint32_t)aw * 256 / hdr.w, zy = (uint32_t)ah * 256 / hdr.h;
                uint32_t z = zx < zy ? zx : zy;
                if (z > 256 * 4) z = 256 * 4;  // don't blow up tiny images more than 4x
                lv_image_set_scale(s_img, z);
                lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
                lv_obj_set_size(s_img, aw, ah);
                lv_obj_center(s_img);
                lv_obj_set_style_bg_color(s_img, th->bg, 0);
            }
        }
    }
    if (err[0]) {
        lv_obj_t* l = ui_mono_label(c, ui_font(), th->error);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, ui_screen_w() - 40);
        lv_label_set_text_fmt(l, "[error] %s", err);
        lv_obj_set_pos(l, 20, 20);
        snprintf(s_result, sizeof(s_result), "{\"error\":\"%s\"}", err);
    }
    ui_push(&s_view);
}
