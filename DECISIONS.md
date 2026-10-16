# DECISIONS — choices this build made where the spec left room

Each entry: what was decided, and one line of reasoning.

1. **ESP-IDF v6.0.2** — the version pinned by `tanmatsu-template`'s Makefile and required (`idf: ">=6.0.2"`) by badge-bsp/tanmatsu-wifi. Toolchain `riscv32-esp-elf` esp-15.2.0_20251204.
2. **All components vendored into `components/`, component manager disabled** — the Espressif component registry (components.espressif.com / api.components.espressif.com / components-file.espressif.com) and dl.espressif.com are blocked from this build host; GitHub git clones and release assets work. Sources and commits are listed in `components/VENDORED.md`. Requirements the component manager would have injected were added to the vendored CMakeLists by hand.
3. **badge-bsp v1.6.1** (not the template's 1.1.0) — the version the reference launcher ships with and the one with the storage API the spec mentions (>=1.6); tanmatsu-wifi 1.3.1 accepts >=1.1.0.
4. **esp-hosted fork `Nicolai-Electronics/esp-hosted-mcu` tag v2.12.3-1** — this is `nicolaielectronics/esp-hosted-tanmatsu 2.12.3~1`, the exact version tanmatsu-wifi 1.3.1 pins; it carries the "reset slave using callback" patch the badge needs. The C6 firmware is never touched.
5. **`CONFIG_BSP_TARGET_WHY2025` + the WHY2025 SDIO pin map** from `tanmatsu-template/sdkconfigs/why2025` (CMD 33, CLK 32, D0 28, D1 31, D2 30, D3 29) — the WHY2025 badge routes the C6 differently from the Tanmatsu.
6. **Console on USB-Serial/JTAG** — the spec states USB-C goes to the P4 USB-Serial/JTAG. (The template's why2025 sdkconfig picks UART0; logs over USB are more useful for first-run debugging.)
7. **`storage` partition is SPIFFS, not LittleFS** — ESP-IDF has SPIFFS built in; LittleFS is a registry component (joltwallet) that would have needed another vendored tree. It only holds the MCP tool-list cache.
8. **Wi-Fi credentials live in the app's own NVS JSON config (`aiterm/config`), with `wifi_init_config.nvs_enable = false`** — the ESP-Hosted slave would otherwise persist credentials in the C6's NVS; the launcher/wifi-manager do the same.
9. **LVGL 9.4.0, RGB888 (24 bpp), partial render into two PSRAM draw buffers, software rotation 90° via `lv_display_rotate_area` + `lv_draw_sw_rotate`, flushed with `bsp_display_blit`** — follows the launcher's pipeline (BSP RGB888 DPI framebuffer, rotation 90 == pax CCW); LVGL's ROTATION_90 pixel mapping was verified to equal pax's CCW mapping.
10. **Font: DejaVu Sans Mono converted with lv_font_conv** at 13/15/18 px (small/medium/large; medium gives exactly 80 columns at 720 px) plus bold variants for headings — DejaVu Sans Mono is available on the build host and covers Latin-1 + box drawing.
11. **Meta key = the badge's SUPER (⌘-like) key** — the WHY2025 BSP reports it as `BSP_INPUT_MODIFIER_SUPER_L`, so `Meta+M`, `Meta+N`, `Meta+,` are Super combinations.
12. **PgUp/PgDn/Home/End = Fn+Up/Down/Left/Right** — the keyboard has no dedicated keys; the BSP exposes an Fn modifier.
