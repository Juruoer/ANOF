#!/usr/bin/env bash
set -euo pipefail

web_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
browser_libs="$web_root/.tools/browser-libs"
if [[ -d "$browser_libs" ]]; then
  # 仅作用于本次浏览器工具进程及其子进程，不修改主机动态链接配置。
  export LD_LIBRARY_PATH="$browser_libs/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export ALSA_CONFIG_PATH="$browser_libs/usr/share/alsa/alsa.conf"
fi
exec "$web_root/scripts/node.sh" "$@"
