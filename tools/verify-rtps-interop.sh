#!/usr/bin/env bash
#
# RTPS 引擎回归 + 重复稳定性检查。跑在 Linux 测试机上：
#
#   tools/verify-rtps-interop.sh [build-dir]
#
# 环境变量：
#   NB_RTPS_BUILD_TYPE  CMake 构建类型（默认 Debug）
#
# 与外部 DDS 实现（Fast DDS / Cyclone DDS）的线缆互通演练不在这里：
# 见 tools/fastdds_interop/README.md，那里用独立进程的 Fast DDS 端点
# 与本仓库的 RTPS 引擎互为对端。

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build-linux}"
build_type="${NB_RTPS_BUILD_TYPE:-Debug}"

if [ ! -f "$build/CMakeCache.txt" ]; then
    echo "== configure ($build) =="
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE="$build_type" \
          -DNB_BUILD_TESTS=ON || exit 1
fi

echo "== build ($build, $build_type) =="
cmake --build "$build" --config "$build_type" -j"$(nproc)" || exit 1

run_ctest() {
    ( cd "$build" && ctest -C "$build_type" "$@" )
}

echo
echo "== unit + rtps =="
run_ctest -L "unit|rtps" --output-on-failure || exit 1

echo
echo "== e2e.rtps_participant, three repeated runs =="
run_ctest -R "e2e.rtps_participant" --repeat until-fail:3 \
          --output-on-failure || exit 1

echo
echo "All RTPS checks passed."
