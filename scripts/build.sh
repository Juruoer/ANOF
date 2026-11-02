#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
usage() {
  cat <<'HELP'
Usage: scripts/build.sh [options] [-- -DNAME=value ...]
  --preset NAME          debug, release (default), debug-tests, release-tests
  --build-dir DIR        Override the preset build directory
  --jobs N               Build jobs (default 2)
  --sqlite-prefix DIR    Search this SQLite prefix, replacing cached SQLite paths
  --target NAME          Build one actual CMake target (default all)
  --web-dist DIR         Install explicitly prebuilt Web assets
  --configure-only      Check dependencies and configure; do not build
  --test                Build all targets then run CTest serially (tests preset required)
  --help                Show this help
No dependency download, instance initialization or system installation is performed.
HELP
}
fail() { printf '%s\n' "$*" >&2; exit 2; }
need_value() { [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || fail "Missing value: $1"; }
preset=release
jobs=2
build_dir=''
target=''
configure_only=false
run_tests=false
cmake_args=()
while (($#)); do
  case "$1" in
    --help|-h) usage; exit 0;;
    --preset) need_value "$@"; preset="$2"; shift 2;;
    --build-dir) need_value "$@"; build_dir="$(realpath -m -- "$2")"; shift 2;;
    --jobs) need_value "$@"; jobs="$2"; shift 2;;
    --target) need_value "$@"; target="$2"; shift 2;;
    --sqlite-prefix)
      need_value "$@"; [[ -d "$2" ]] || fail 'SQLite prefix does not exist'
      cmake_args+=('-USQLite3_INCLUDE_DIR' '-USQLite3_LIBRARY' "-DSQLite3_ROOT=$(realpath -- "$2")"); shift 2;;
    --web-dist) need_value "$@"; [[ -d "$2" ]] || fail 'Web dist does not exist'; cmake_args+=("-DANOF_WEB_DIST=$(realpath -- "$2")"); shift 2;;
    --configure-only) configure_only=true; shift;;
    --test) run_tests=true; shift;;
    --) shift; for arg in "$@"; do [[ "$arg" == -D* || "$arg" == -U* ]] || fail 'Extra arguments must be -D or -U CMake options'; cmake_args+=("$arg"); done; break;;
    *) fail "Unknown option: $1";;
  esac
done
case "$preset" in debug|release|debug-tests|release-tests) ;; *) fail 'Unsupported preset';; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail 'Jobs must be a positive integer'
[[ "$target" != -* ]] || fail 'Invalid build target'
if $run_tests; then
  [[ "$preset" == *-tests && -z "$target" ]] || fail '--test requires a tests preset and building all targets'
  $configure_only && fail '--test cannot be combined with --configure-only'
fi
[[ -n "$build_dir" ]] || build_dir="$repo_root/cmake-build-${preset%-tests}"
[[ "$build_dir" != "$repo_root" ]] || fail 'In-source builds are not supported'
cd -- "$repo_root"
cmake --preset "$preset" -B "$build_dir" "${cmake_args[@]}"
$configure_only && exit 0
build_args=(--build "$build_dir" --parallel "$jobs")
[[ -z "$target" ]] || build_args+=(--target "$target")
cmake "${build_args[@]}"
if $run_tests; then
  # 用户额外cache选项不能把一个空测试集伪装成成功验收。
  ctest --test-dir "$build_dir" --output-on-failure --no-tests=error -j 1
fi
printf 'Build completed: %s\n' "$build_dir"
