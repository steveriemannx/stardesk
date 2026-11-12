#!/usr/bin/env bash
# Build StarDesk locally and (optionally) deploy + rebuild the Linux host.
#
#   ./scripts/build-deploy.sh              # local build only
#   ./scripts/build-deploy.sh --host       # local build + sync sources to the
#                                          # Linux host, rebuild it and restart
#                                          # the stardesk.service
#   ./scripts/build-deploy.sh --host --no-local
#
# Environment (host deploy):
#   STARDESK_HOST=shxu@192.168.137.148     ssh target
#   STARDESK_HOST_DIR=~/projects-main/stardesk
#   STARDESK_HOST_BUILD=build-wayland
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

HOST="${STARDESK_HOST:-shxu@192.168.137.148}"
HOST_DIR="${STARDESK_HOST_DIR:-projects-main/stardesk}"
HOST_BUILD="${STARDESK_HOST_BUILD:-build-wayland}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
LOCAL=1
DEPLOY=0

for arg in "$@"; do
    case "$arg" in
    --host) DEPLOY=1 ;;
    --no-local) LOCAL=0 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$LOCAL" == 1 ]]; then
    echo "== local build ($JOBS jobs)"
    cmake --build build --target stardesk -j "$JOBS"
fi

if [[ "$DEPLOY" == 1 ]]; then
    echo "== sync sources to $HOST"
    rsync -az --delete -e "ssh -T -o ConnectTimeout=20" src/ "$HOST:$HOST_DIR/src/"
    rsync -az -e "ssh -T -o ConnectTimeout=20" CMakeLists.txt "$HOST:$HOST_DIR/CMakeLists.txt"
    echo "== rebuild host"
    ssh -T -o ConnectTimeout=20 "$HOST" "cd ~/$HOST_DIR && cmake --build $HOST_BUILD --target stardesk -j 12"
    echo "== restart stardesk.service"
    ssh -T -o ConnectTimeout=20 "$HOST" "systemctl --user restart stardesk.service && sleep 1 && systemctl --user is-active stardesk.service"
fi

echo "done"
