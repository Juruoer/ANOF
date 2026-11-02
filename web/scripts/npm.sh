#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain.sh"
anof_npm="${ANOF_NPM:-$(command -v npm || true)}"
[[ -f "$anof_npm" ]] || { printf 'npm CLI not found; install npm or set ANOF_NPM.\n' >&2; exit 1; }
anof_npm="$(readlink -f -- "$anof_npm")"
case "$anof_npm" in /mnt/*|*.exe|*.cmd) printf 'Windows npm is not supported.\n' >&2; exit 1;; esac
npm_version="$("$anof_node" -p 'require("./package.json").engines.npm')"
[[ "$("$anof_node" "$anof_npm" --version)" == "$npm_version" ]] || { printf 'npm %s is required.\n' "$npm_version" >&2; exit 1; }
exec "$anof_node" "$anof_npm" "$@"
