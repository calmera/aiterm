# ai-terminal — a hardware AI client for the WHY2025 badge

`ai-terminal` turns the WHY2025 badge (ESP32-P4 + ESP32-C6 radio, 720×720 display, QWERTY
keyboard) into a chat/coding terminal for any OpenAI-compatible or Anthropic inference endpoint,
with remote **MCP** tool servers, seven on-device **render tools** the model can call
(markdown, code, diff, table, choice, image, chart), and a plain **SSH** terminal mode.

Everything heavy runs on the backend; the badge renders, takes input and holds the connections.
No vendor protocol in the firmware — only the OpenAI/Anthropic HTTP APIs and MCP Streamable HTTP.

## Flashing

Artifacts are in `out/`:

| file | purpose |
|---|---|
| `out/ai-terminal-merged.bin` | complete 16 MB-flash image for offset `0x0` (bootloader @0x2000, partition table @0x8000, app @0x20000) |
| `out/parts/` | bootloader, partition table, otadata and app as separate files |
| `out/flash.sh` | writes `out/parts/` at their offsets (saved settings in NVS are kept); `ERASE_SETTINGS=1` writes the merged image instead; port auto-detected on macOS, 460800 baud, `--no-stub` |
| `out/flasher_args.json` | the offsets the image was built from |

Built image: 2,313,616 bytes (app 2.18 MB of the 6 MB factory partition; SSH on). SHA-256 in `WORKLOG.md`.

```sh
pipx install esptool            # any recent esptool (>= 4.8) knows the ESP32-P4
./out/flash.sh                  # macOS: auto-detects /dev/cu.wchusbserial*; Linux: PORT=/dev/ttyUSB0 ./out/flash.sh
```

**Which socket.** The badge has two USB-C sockets. Flash through the **side** socket: it is a
CH340 USB-UART wired to the P4's UART (the bottom socket is charging / the P4's native USB and
is not used for flashing). The CH340 lets esptool reset the chip into the bootloader by itself.
If esptool reports "Failed to connect to ESP32-P4", use the usual ESP32 sequence: **hold BOOT,
press and release RESET, release BOOT**, then run the flash script within a few seconds.

**macOS driver.** The badge's CH340 reports USB ID `1a86:7522`, which macOS's built-in WCH driver
does not match (it only knows `1a86:7523` and `1a86:55d4`), so no `/dev/cu.*` device appears
until the vendor driver is installed. Follow the badge wiki
(https://wiki.why2025.org/Badge/MacOS_Drivers): install **CH34xVCPDriver** from
https://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html (drag the app to Applications, run it,
click Install), then allow it in *System Settings → General → Login Items & Extensions → Driver
Extensions* (`cn.wch.CH34xVCPDriver`). The port then shows up as `/dev/cu.wchusbserialNN`.
Do not use the Homebrew cask; the wiki reports it does not work. Linux needs no driver
(`/dev/ttyUSB0`).

The stock launcher's web flasher (https://why-prog.heplaphon.com/, Chrome/Edge) uses the same
side socket and can flash `out/ai-terminal-merged.bin` at offset `0x0` as well.

**Console.** The firmware logs on UART0 at 115200 baud (`CONFIG_ESP_CONSOLE_UART_DEFAULT`), i.e.
on the same side-socket CH340 port used for flashing (`/dev/cu.wchusbserialNN`, Linux
`/dev/ttyUSB0`). The **bottom** USB-C socket is the **ESP32-C6 radio's** USB-Serial/JTAG
(`303a:1001`, `/dev/cu.usbmodem*`): useful to watch the ESP-Hosted slave, but the P4's own
USB-Serial/JTAG is not reachable from either socket.

Verified on hardware (2026-10-17): flashed from macOS through the side socket with esptool 5.6.0,
460800 baud, `--no-stub`, 2,313,616 bytes in 98 s, hash verified.

Flashing replaces the launcher. The ESP32-C6 radio firmware (ESP-Hosted slave) is **never**
touched by this image.

## First boot

1. The badge boots straight into the terminal. With no Wi-Fi configured it starts the setup
   hotspot `aiterm-XXXX` (open network) and shows two QR codes.
2. Scan the first QR (or join `aiterm-XXXX`) with a phone or laptop, then scan the second QR or
   open `http://192.168.4.1/`.
3. On the page: pick your Wi-Fi (Scan) and enter the password → **Connect**. The status line
   shows the IP once connected; the same page is then also reachable at `http://<ip>/`.
4. Fill in **Inference** (provider, base URL, API key, model) → **Test**. Add **MCP servers**
   (→ **Connect** lists the discovered tools) and **SSH hosts**. **Save**.
5. Back on the badge, type a message and press Enter.

The whole configuration can be exported/imported as JSON on the page (secrets masked on export).

**System bar (top).** Clock (local time via SNTP once Wi-Fi is up; set *Device → Timezone* as a
POSIX TZ string, default `CET-1CEST,M3.5.0,M10.5.0/3`), Wi-Fi signal bars with the SSID (or
`setup aiterm-XXXX` / `no wifi`), and a battery gauge **if** the BSP reports one. The WHY2025
badge has no fuel gauge or charger the P4 can read (the BSP's power functions are stubs for this
target and BadgeVMS has no battery driver either), so the battery slot stays hidden there.

**Status line (bottom).** Vim-style segments: ` AI ` (blue; ` SSH ` purple) · model · context
gauge `████░░░░ 42%` coloured green/yellow/red by usage of the model's **context window**
(*Inference → Context window*, default 128000 tokens; `~` = estimated because the server did not
report usage) · session cost from the **price per 1M tokens** fields, or the token total when no
prices are set · tool count. `Meta+N` starts a new session and resets the counters.

### Endpoints that work
* OpenAI: `https://api.openai.com/v1`, model e.g. `gpt-4.1-mini`
* Ollama: `http://<host>:11434/v1` (empty API key)
* vLLM / LiteLLM / any OpenAI-compatible gateway: `http(s)://host/v1`
* Anthropic: `https://api.anthropic.com/v1`, model e.g. `claude-sonnet-4-5`
* Scaleway Generative APIs: `https://api.scaleway.ai/<project-id>/v1`, model e.g. `llama-3.3-70b-instruct` (verified)

## Keys

| key | action |
|---|---|
| `Enter` | send message (AI) / open the SSH terminal (SSH mode) |
| `Esc` | close the current full-screen view; in the chat: cancel the running request |
| `F1` or `Meta+,` | Settings |
| `Meta+M` | toggle AI / SSH mode |
| `Meta+N` | new AI session (clears the conversation) |
| `PgUp/PgDn` (`Fn+↑/↓`), `Shift+↑/↓` | scroll the chat |
| `↑/↓` (empty input) | input history |
| `Home/End` (`Fn+←/→`), `Ctrl+U/A/E/K/W` | line editing |
| in views: `↑↓ PgUp PgDn Home End` scroll, `←→` pan wide content | |
| diff view: `A` accept, `R`/`Esc` reject | |
| choice view: `↑↓` move, `Space` toggle (multi), `Enter` confirm, `Esc` cancel | |
| SSH terminal: keys go to the remote; `Meta+Q` (or `Meta+Esc`) leaves | |

`Meta` is the badge's Super key.

## What is implemented

| phase | status |
|---|---|
| 0 Skeleton (IDF 6.0.2, BSP, LVGL 9.4 TUI, keyboard, merge/flash scripts) | done |
| 1 Wi-Fi via ESP-Hosted/C6, NVS config, SoftAP provisioning, setup page + REST API, QR codes, settings | done |
| 2 AI mode: streaming SSE, OpenAI + Anthropic, agent loop, abort, history budget | done |
| 3 Render tools: markdown, choice, code, table, diff, image, chart | done |
| 4 MCP Streamable HTTP client, tool merge, Connect test, per-server toggle | done |
| 5 Polish: status line, live theme/font, light theme, `[error] … — hint` lines, PSRAM watchdog, About | done |
| 6 SSH mode (wolfSSH 1.4.20 + wolfCrypt 5.8.2, password auth, xterm PTY, VT100/ANSI screen) | done, behind `CONFIG_AITERM_SSH` (default **on**) |
| 7 Voice | **not implemented** — `audio/voice.h` stubs + `CONFIG_AITERM_VOICE` (off). STT/TTS are meant as backend services (`/audio/transcriptions`, `/audio/speech`); the C6 is BLE-only (no A2DP headsets) |

Verified on a production WHY2025 badge on 2026-10-17: display (RGB565), keyboard, Wi-Fi via the
C6, the setup page, the AI chat (Scaleway OpenAI-compatible endpoint, Llama/Mistral/Gemma) and the
markdown/diff/code views. See `WORKLOG.md` for the bring-up trail; the differences from the
upstream badge.team BSP are documented in `DECISIONS.md`.

Known quirks:
* Every cold boot shows a second ROM banner `rst:0x7 (HP_SYS_HP_WDT_RESET)` about 0.6 s after
  power-on, before the app starts; the second boot is the real one. Cause not identified.
* The C6 ships with ESP-Hosted slave 2.0.17 while the host component is 2.12.x; the link works but
  the host warns "Version mismatch ... Upgrade co-proc". BadgeVMS reflashes the C6 itself.
* The setup page's inference **Test** sends an 8-token request; Gemma 4 answers it with empty text
  (it spends the budget before answering) although chat with it works. Llama/Mistral reply "pong".
* Display rotation is set to 90° like the launcher; if the image is rotated wrongly change
  `AITERM_DISPLAY_ROTATION` in `idf.py menuconfig` → ai-terminal.
* The SSH view ignores background colours except for inverse video; bold is rendered brighter.
* SSH host keys are not verified (fingerprint is logged on the console).
* Enterprise (802.1X) Wi-Fi is not supported on the P4/C6 combination.
* Wi-Fi credentials are stored in the app's own NVS blob (not in the radio's NVS).

## Rebuilding

```sh
# one-time: ESP-IDF v6.0.2 at /root/esp/esp-idf-v6.0.2 (or set IDF_PATH), tools installed
source env.sh                       # exports IDF, disables the component manager (all deps are vendored)
idf.py set-target esp32p4
idf.py build
./tools/make_merged_bin.sh          # -> out/ai-terminal-merged.bin
```

All dependencies live in `components/` (see `components/VENDORED.md` for sources and
commits); the ESP component registry is not needed. Project options: `idf.py menuconfig` →
"ai-terminal" (rotation, draw buffer size, SSH, voice, scrollback size, version string).

## Host tests

```sh
cd host_tests && make run
```

Compiles the protocol/parser sources with the host compiler and runs: SSE parsing, OpenAI and
Anthropic delta accumulation (incl. fragmented tool-call arguments), MCP JSON-RPC/SSE framing
and pagination, markdown → blocks, unified-diff parsing and old/new line diff, VT100 screen
emulation, history budget trimming.

## Layout

```
main/            app (ui/, net/, agent/, provisioning/, util/, audio/, fonts/)
components/      vendored: badge-bsp, tanmatsu-wifi, esp-hosted (Tanmatsu fork), esp_wifi_remote,
                 LVGL 9.4, cJSON, qrcodegen, wolfSSL/wolfSSH, panel drivers, cmake_utilities, …
host_tests/      native unit tests
tools/           make_merged_bin.sh, flash.sh
out/             final artifacts
DECISIONS.md     every choice the spec left open
WORKLOG.md       timeline incl. every build failure and its fix
```

## License

Application code: MIT. Vendored components keep their own licenses (see each directory); note that wolfSSL and wolfSSH are GPLv3 (or commercial), which makes a binary that includes SSH mode GPLv3 as a whole — build with `CONFIG_AITERM_SSH=n` if that matters.
