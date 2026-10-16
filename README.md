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
| `out/flash.sh` | `esptool.py --chip esp32p4 -p ${PORT:-/dev/ttyACM0} -b 921600 write_flash 0x0 ai-terminal-merged.bin` |
| `out/flasher_args.json` | the offsets the image was built from |

Built image: 2,313,616 bytes (app 2.18 MB of the 6 MB factory partition; SSH on). SHA-256 in `WORKLOG.md`.

```sh
pip install esptool            # any recent esptool (>= 4.8) knows the ESP32-P4
PORT=/dev/ttyACM0 ./out/flash.sh
```

**Download mode.** The P4's USB-Serial/JTAG port lets esptool reset the chip into the
bootloader by itself, so normally no key is needed. If esptool reports "Failed to connect to
ESP32-P4", put the badge into download mode manually with the usual ESP32 sequence: **hold the
badge's BOOT/download key, press and release RESET, release BOOT**, then run the flash script
within a few seconds. The badge wiki / launcher documentation names the exact key for your
hardware revision (this build host could not reach the wiki to quote it — see DECISIONS.md).
If the badge still runs the stock launcher, its own "flash app" / badgelink path also works for
getting an image onto the device.

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

### Endpoints that work
* OpenAI: `https://api.openai.com/v1`, model e.g. `gpt-4.1-mini`
* Ollama: `http://<host>:11434/v1` (empty API key)
* vLLM / LiteLLM / any OpenAI-compatible gateway: `http(s)://host/v1`
* Anthropic: `https://api.anthropic.com/v1`, model e.g. `claude-sonnet-4-5`

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

Known limitations (untested on hardware — this firmware was built without a badge at hand):
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
