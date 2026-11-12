#!/usr/bin/env bash
# Install StarDesk into /Applications (or ~/Applications) and ask macOS for the
# Screen Recording / Accessibility permissions again.
#
#   ./scripts/install-macos.sh          # keep existing permission grants
#   ./scripts/install-macos.sh --reset  # forget permissions (prompts again)
#
# The app is ad-hoc signed, so re-run this after rebuilding to keep the installed
# copy in sync with the build (macOS may ask for the permissions again because
# the ad-hoc signature changes).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/build/bin/stardesk.app"
DEST_DIR="/Applications"
if [[ ! -w "$DEST_DIR" ]]; then
    DEST_DIR="$HOME/Applications"
    mkdir -p "$DEST_DIR"
fi
DEST="$DEST_DIR/StarDesk.app"

if [[ ! -d "$APP" ]]; then
    echo "build first: cmake --build build --target stardesk" >&2
    exit 1
fi

echo "== installing to $DEST"
rm -rf "$DEST"
ditto "$APP" "$DEST"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
codesign --force --deep --sign - "$DEST" >/dev/null 2>&1 || true

if [[ "${1:-}" == "--reset" ]]; then
    echo "== resetting permission decisions for com.stardesk.app"
    tccutil reset Accessibility com.stardesk.app >/dev/null 2>&1 || true
    tccutil reset ScreenCapture com.stardesk.app >/dev/null 2>&1 || true
    tccutil reset ListenEvent com.stardesk.app >/dev/null 2>&1 || true
fi

echo "== launching $DEST"
open "$DEST"

cat <<MSG

StarDesk 已安装到: $DEST
启动时会弹出权限请求（辅助功能 / 屏幕录制）。如果没有弹：

  系统设置 > 隐私与安全性 > 辅助功能 / 屏幕录制
  → 点 "+" → 按 Cmd+Shift+G → 输入 $DEST_DIR 回车
  → 选择 StarDesk.app

之后每次重新编译 mac 端，再运行一次本脚本即可（安装的副本会更新，
签名变化时系统可能要求重新勾选权限）。
MSG
