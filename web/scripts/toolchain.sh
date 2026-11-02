#!/usr/bin/env bash
# 由node/npm入口source；只选择和验证工具，不下载安装。
web_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
node_version="$(cat "$web_root/.node-version")"
private_node="$web_root/.tools/node-v${node_version}-linux-x64/bin/node"
valid_node() {
  local candidate
  candidate="$(readlink -f -- "$1" 2>/dev/null)" || return 1
  [[ -x "$candidate" && "$candidate" != /mnt/* && "$candidate" != *.exe ]] &&
    [[ "$("$candidate" -p 'process.platform + ":" + process.arch + ":" + process.versions.node' 2>/dev/null)" == "linux:x64:${node_version}" ]]
}
if [[ -n "${ANOF_NODE:-}" ]]; then
  anof_node="$(command -v -- "$ANOF_NODE" || true)"
  valid_node "$anof_node" || { printf 'ANOF_NODE must select Linux x64 Node %s\n' "$node_version" >&2; exit 1; }
else
  anof_node="$(command -v node || true)"
  if ! valid_node "$anof_node"; then anof_node="$private_node"; fi
  valid_node "$anof_node" || { printf 'Install Linux x64 Node %s or set ANOF_NODE; no download was attempted.\n' "$node_version" >&2; exit 1; }
fi
anof_node="$(readlink -f -- "$anof_node")"
export PATH="$(dirname -- "$anof_node"):$PATH"
export PLAYWRIGHT_BROWSERS_PATH="${PLAYWRIGHT_BROWSERS_PATH:-$web_root/.tools/browsers}"
export npm_config_cache="${npm_config_cache:-$web_root/.tools/npm-cache}"
cd -- "$web_root"
