#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/toolchain.sh"
exec "$anof_node" "$@"
