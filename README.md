# StarDesk Remote Desktop

**English** | [简体中文](README.zh-CN.md)

A LAN remote-desktop tool built on [dui](https://github.com/steveriemannx/dui) in
pure-code mode, similar to ToDesk / AnyDesk. Every window and asset is drawn by
code: no XML layouts, no image resources, no CEF/Web.

## Features

- **Screen sharing**: mirror mode (primary display) or extend mode (multiple
  monitors stitched into one canvas)
- **Video transport**: 64×64 tile diffing + PNG compression (dui's bundled
  libwebp has no encoder, so PNG is used in this phase); original / 720p / 1080p
  resolution and 24 / 30 / 60 fps
- **Keyboard & mouse control**: macOS `CGEventPost` (requires Accessibility
  permission) / Windows `SendInput` / Linux · FreeBSD XTest
- **View-only mode**: watch the screen without input injection (enforced by the
  host)
- **File transfer**: separate TCP channel with a session token, drag-and-drop or
  file dialog, bidirectional transfer with progress
- **Connection authentication**: SHA-256 challenge/response (the password is
  never sent in clear text); auto-accept or manual confirmation dialog per
  connection.
  **Manual confirmation needs no password** — once the host user clicks
  "Allow", the connection is authorized; auto-accept mode still verifies the
  password.
- **LAN discovery**: UDP broadcast beacons, one-click fill from the device list;
  devices outside the subnet can be added manually (name/IP/port, persisted)
- **Light/dark theme + 8 languages** (Simplified Chinese, Traditional Chinese,
  English, Japanese, Korean, German, French, Russian)
- **Remote window fullscreen** (button / ESC to exit), fit-to-window /
  original size, frame-rate and latency readout
- **Reverse remote assistance**: on the machine that wants to be controlled,
  select a known device and click "Request help"; the peer gets a confirmation
  dialog and then connects back with a one-time token (no password needed)

## Building

StarDesk is a standalone project (this directory). A dui source tree is required
at build time; point CMake variable `DUI_SRC_ROOT_DIR` at it — it defaults to the
sibling `../dui` (current dui main: native backends + UTF-8 string API, SDL
backend removed).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DDUI_SRC_ROOT_DIR=/path/to/dui
cmake --build build --target stardesk -j 8
open build/bin/stardesk.app
```

The dui tree is compiled as a sub-project **inside this project's build
directory** (library only — no dui examples / tests / CEF; Skia is used as a
prebuilt static library). The build target is therefore `stardesk`, but the dui
library (`libdui.a`, ~340 objects) lives in the same build tree: the first build
is full, afterwards **only changed files are recompiled**. Measured on macOS /
8 cores:

| Scenario | Time |
|----------|------|
| Nothing changed (no-op) | ~1.8 s |
| One stardesk source file changed | ~3–4 s |
| One dui source file changed | ~3 s |
| A dui public header changed (included everywhere) | many files, tens of seconds |

The local build directory is configured with **ccache**
(`CMAKE_CXX_COMPILER_LAUNCHER`), so clean or large rebuilds are much faster.

### macOS installation (permissions)

The macOS Privacy & Security panes only let you add an `.app` bundle, so install
the build output into `/Applications` and grant permissions there:

```bash
./scripts/setup-signing.sh   # first time only: create a local self-signed code-signing identity
./scripts/install-macos.sh   # install to /Applications/StarDesk.app and launch
```

**Why `setup-signing.sh`?** An ad-hoc signature changes with every build, and
macOS then invalidates the Screen Recording / Accessibility grants (you would
have to re-tick them after each rebuild). With a fixed local signing identity
the signature is stable and the grants persist (still authorized once the first
time).

Then enable StarDesk in **System Settings → Privacy & Security → Accessibility /
Screen Recording** (if it is not listed yet: click "+" → Cmd+Shift+G → type
`/Applications` → select StarDesk.app).

### Build / deploy scripts

```bash
./scripts/build-deploy.sh            # build the local macOS app only
./scripts/build-deploy.sh --host     # build + rsync to the Linux host, rebuild and restart it
```

`--host` uses rsync so only changed sources are transferred (mtimes preserved;
the remote build recompiles only what changed). A "change one file → sync →
host build → restart" cycle takes about **10–13 s**, versus 60–90 s with the old
"touch every source file" approach. Override the host with `STARDESK_HOST` /
`STARDESK_HOST_DIR`.

To build this directory inside the dui tree instead, copy it to
`<dui>/examples/stardesk` and omit `DUI_SRC_ROOT_DIR` (it then automatically
detects `../..` as the dui source tree).

### Platform requirements

| Platform | Requirements |
|----------|--------------|
| macOS | **14.0+** (ScreenCaptureKit); Screen Recording permission (video), Accessibility permission (remote input). Currently the validated platform |
| Windows | 7 / 10 / 11 (native Win32); sources kept, build not validated yet |
| Linux / FreeBSD | Native X11/Wayland backends; sources kept, build not validated yet |

macOS permissions: System Settings → Privacy & Security → Screen Recording /
Accessibility, tick StarDesk (or the terminal that launched it). Without Screen
Recording permission the host automatically falls back to the built-in "test
pattern" so the link can still be verified, and says so in the status line.

## Usage

1. Start StarDesk on both machines (same LAN / hotspot).
2. **Host side**: the left panel shows this machine's password; switch between
   "auto accept / manual confirm" and "mirror / extend" sharing mode.
3. **Client side**: the "Remote control" tab on the right — enter the peer IP and
   password (or click a device in the discovery list), choose control/view,
   resolution and frame rate, then click "Connect".
4. After connecting, the remote window opens: fit to window / original size /
   fullscreen / send file / disconnect.
5. "Files" tab: a "Send file" button opens the transfer window (drag-and-drop or
   browse); received files go to `~/Downloads/StarDesk/` by default (configurable).

## Configuration

Config file `~/.stardesk.conf` (UTF-8, `key=value`):

```
password=12345678      # connection password (the UI can generate a random one)
port=7456              # main port (the file port is the main port + 1)
manual_accept=0        # 1 = ask for manual confirmation on every connection
extend_screen=0        # 1 = extend mode (multi-monitor canvas)
receive_dir=           # file receive directory (empty = ~/Downloads/StarDesk)
dark_theme=1
language=0             # 0-7, one of the 8 built-in languages
want_fps=30            # default frame rate on the client side
want_res=0             # 0=original 1=720p 2=1080p
device_name=           # device name (defaults to the host name)
headless=0             # 1 = run the host without a local window, auto-accept connections
```

### Headless host

A Linux/FreeBSD machine without a monitor can run the host in headless mode:

```bash
STARDESK_HEADLESS=1 ./bin/stardesk
```

You can also set `headless=1` in `~/.stardesk.conf` and start it from systemd,
rc.d or another init system. This mode creates no local control window but still
listens on the port and accepts connections automatically; the client can use
`auto_host`, `auto_port` and `auto_password` to connect right after startup.

"Headless" means no physical monitor, not no desktop session. The remote side
must still have a real desktop or a virtual display session running (for
example an Xvfb/Xorg virtual output, or a logged-in Wayland/Portal session) —
GUI applications run as usual and StarDesk captures the real screen from that
session. Without a usable display session there is no real desktop to show; the
built-in test pattern only proves the network path.

## Protocol overview

Binary frame: `[magic "SDK1"][type u8][len u32][payload]` (little endian).
Authentication:
`Hello → Challenge{nonce} → Auth{SHA256(nonce‖password)} → AuthResult{ok, filePort, token}`.
Video stream: `ScreenInit{width,height}` + `ScreenTile{x,y,w,h,png}` + `ScreenEnd`
(end-of-frame marker; the client renders only after it, so tiles from two frames
never mix during fast scrolling) + `CursorPos{normalized coordinates}`.
Input: `InputEvent{kind, normalized coordinates / key code / modifiers}`.
File channel: separate TCP port (main port + 1), first frame `FileAuth{token}`,
then `FileStart/FileAccept/FileChunk/FileEnd/FileDone/FileAbort`.
Heartbeat is Ping/Pong every 3 s; the connection drops after 15 s without a
response. The video stream is unencrypted in this phase (LAN use; documented
here).

## Known limitations (phase one)

- The video stream is unencrypted; Linux is X11-only; macOS needs 14+; one
  control session at a time
- One-time tokens for reverse assistance expire after 120 s and can be used once
- PNG encoding is bandwidth-hungry on colourful screens (tile diffing mitigates
  most of it); a full libwebp/JPEG encoder can be plugged in later
- Capture: SCStream continuous stream (60 fps, mirror mode); multi-monitor extend
  mode still captures single frames (~5 fps)
- Large full-screen changes (e.g. video) are limited by PNG encoding + bandwidth
  (about 30 fps at 1080p on a LAN)
- Multi-monitor "extend" mode on macOS is a union canvas (secondary-display
  layout differences are not compensated)

## Moving to a standalone repository

This directory is its own git repository (nested under dui's `examples/`), and it
does not depend on any resource file outside dui's build. To move it:

```bash
cd examples/stardesk
git remote add origin git@github.com:<your>/stardesk.git
git push -u origin main
```

A standalone build needs dui and Skia libraries and headers (just point
`DUI_SRC_ROOT_DIR` at the dui repository root).
