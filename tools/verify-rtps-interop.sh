#!/usr/bin/env bash
#
# Verify the self-developed RTPS stack, including Cyclone DDS interop.
# Run this by hand on the Linux test machine:
#
#   tools/verify-rtps-interop.sh [build-dir]
#
# Environment:
#   NB_RTPS_ADVERTISE_HOST  routable IPv4 address of this machine. Defaults to
#                           the source address of the default route.
#   NB_RTPS_BUILD_TYPE      CMake build type (default: Debug).
#
# The advertised address must be a real interface address, not 127.0.0.1:
# Cyclone binds its sockets to an interface and cannot send to loopback, so
# the data plane fails even though discovery succeeds.
#
# The Cyclone test runs twice: with normal multicast discovery, and in
# unicast-only mode (fixed ports plus explicit discovery peers), which does
# not depend on multicast routing.

set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build-linux}"
build_type="${NB_RTPS_BUILD_TYPE:-Debug}"
# Cyclone DDS 从 3rdparty/cyclonedds 源码编译并静态链接，无需设置 LD_LIBRARY_PATH。

if [ ! -f "$build/CMakeCache.txt" ]; then
    echo "== configure ($build) =="
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE="$build_type" \
          -DNB_BUILD_TESTS=ON || exit 1
fi

advertise="${NB_RTPS_ADVERTISE_HOST:-}"
if [ -z "$advertise" ]; then
    advertise="$(ip -4 route get 1.1.1.1 2>/dev/null |
        awk '{ for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit } }')"
fi
if [ -z "$advertise" ]; then
    advertise="$(hostname -I 2>/dev/null | awk '{ print $1 }')"
fi
if [ -z "$advertise" ]; then
    echo "error: set NB_RTPS_ADVERTISE_HOST to a routable IPv4 address" >&2
    exit 2
fi

echo "== build ($build, $build_type) =="
cmake --build "$build" --config "$build_type" || exit 1

run_ctest() {
    ( cd "$build" && ctest -C "$build_type" "$@" )
}

echo
echo "== unit and RTPS tests =="
run_ctest -L "unit|rtps" --output-on-failure || exit 1

echo
echo "== e2e.rtps_participant, three repeated runs =="
run_ctest -R "e2e.rtps_participant" --repeat until-fail:3 \
          --output-on-failure || exit 1

echo
echo "== Cyclone interop, multicast discovery (advertise $advertise) =="
( cd "$build" &&
  NB_TEST_RTPS_CYCLONE=1 NB_TEST_RTPS_ADVERTISE_HOST="$advertise" \
      ctest -C "$build_type" -R e2e.rtps_cyclone --output-on-failure ) || exit 1

echo
echo "== Cyclone interop, unicast-only peers (advertise $advertise) =="
( cd "$build" &&
  NB_TEST_RTPS_CYCLONE=1 NB_TEST_RTPS_UNICAST_ONLY=1 \
  NB_TEST_RTPS_ADVERTISE_HOST="$advertise" \
      ctest -C "$build_type" -R e2e.rtps_cyclone --output-on-failure ) || exit 1

echo
echo "All RTPS interop checks passed on advertise host $advertise."
