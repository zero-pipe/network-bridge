# Cyclone DDS (vendored source tree)

Source-only copy of Cyclone DDS, compiled into the `cyclone.dds` transport
driver. No prebuilt SDK is used: the `ddsc` static library is built from these
sources by the top-level `CMakeLists.txt`.

- Version: Cyclone DDS **11.0.1** (upstream commit `3a552631`,
  `git describe` = `11.0.1-291-g3a552631`)
- License: Eclipse Public License 2.0 / Eclipse Distribution License 1.0
  (see `LICENSE`, `NOTICE.md` and the SPDX headers in every file)
- Upstream: <https://github.com/eclipse-cyclonedds/cyclonedds>

## Layout

```
CMakeLists.txt        trimmed copy of the upstream top-level build file
cmake/                build modules (export headers, hashes, coverage, ...)
compat/               getopt shim used by the config generator on Windows
etc/                  checked-in cyclonedds.rnc / .xsd configuration schema
src/
  CMakeLists.txt
  core/
    cdr/              CDR / XCDRv1 / XCDRv2 serialization
    ddsi/             RTPS protocol stack, discovery, state machine
    ddsc/             public C API and the DDS engine core
  ddsrt/              platform abstraction (threads, sockets, sync)
  psmx_iox/           iceoryx / iceoryx2 shared-memory PSMX plugin
  security/           OMG DDS Security (not compiled by default)
  tools/
    _confgen/         generates defconfig.c from the config table
    dyntypelib/       dynamic type library used by the tools/API
    xmltype/          type inspection tool
```

Upstream has no separate `dds/` engine directory: `src/core/ddsc` carries both
the public API (`include/dds/ddsc/dds_public_*.h`, `src/dds_*.c`) and the
engine (QoS matching, reader/writer history, waitsets).

## Build wiring

`CMakeLists.txt` in the repository root does:

```cmake
add_subdirectory(3rdparty/cyclonedds)
add_library(nb_cyclonedds INTERFACE)
target_link_libraries(nb_cyclonedds INTERFACE ddsc)
target_compile_definitions(nb_cyclonedds INTERFACE NB_ENABLE_DDS=1)
```

`ddsc` propagates its include directories, so consumers just link
`nb_cyclonedds` and `#include <dds/dds.h>`. Build artefacts stay under
`<build>/3rdparty/cyclonedds/lib` and therefore do not mix with the project's
own outputs.

The vendored defaults are plain (non-cache) variables, set before the
subdirectory is added, so they never leak into the parent cache:

| Variable | Value | Why |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | static `ddsc.lib` linked into `nb` |
| `BUILD_TESTING` | `OFF` | the upstream test suites are not vendored |
| `BUILD_IDLC`, `BUILD_DDSPERF`, `BUILD_EXAMPLES` | `OFF` | tools only, not needed |
| `ENABLE_SECURITY`, `ENABLE_SSL`, `ENABLE_TCP_TLS` | `OFF` | keeps OpenSSL out of the build |
| `ENABLE_ICEORYX`, `ENABLE_ICEORYX2` | `OFF` | `psmx_iox` sources kept, plugin not built |
| `ENABLE_LTO` | `OFF` | keeps MSVC/GCC builds fast and portable |

The security and `psmx_iox` sources are vendored on purpose: set
`ENABLE_SECURITY` / `ENABLE_SSL` (OpenSSL required) or `ENABLE_ICEORYX`
(iceoryx installed) before `add_subdirectory(3rdparty/cyclonedds)` to build
them.

## Differences from upstream

Nothing under `src/` was edited apart from one guard, so the tree can be
refreshed from upstream by copying the directories back:

1. `CMakeLists.txt` is a trimmed derivative of the upstream build file.
   Documentation, examples, fuzzing, CI, packaging (CPack, `PackageConfig`/
   `PkgConfig` templates, install/export rules) and the `check_generated_*`
   helper documentation targets that depend on `docs/` are gone. The
   `check_generated_sources` target is kept and regenerates `defconfig.c` into
   the build tree.
2. `src/core/CMakeLists.txt`: `add_subdirectory(xtests)` is guarded with
   `if(BUILD_TESTING AND EXISTS ...)`, because the test tree is not vendored.

Removed compared to upstream: `docs/`, `examples/`, `fuzz/`, `scripts/`,
`hooks/`, `ports/`, `WiX/`, `.github/`, `.azure/`, `src/ucunit/`, every
`tests/` and `xtests/` directory, `src/tools/idlc/` and `src/tools/ddsperf/`
(the pre-generated XTypes sources in `src/core/ddsi` are used instead), and
the per-platform prebuilt SDK trees (`<os>_<arch>/`) that used to hold
`ddsc.lib`/`ddsc.dll`/`libddsc.so`.

## Refreshing from upstream

```bash
git clone --depth 1 --branch 11.0.1 https://github.com/eclipse-cyclonedds/cyclonedds
# copy src/core, src/ddsrt, src/psmx_iox, src/security, src/tools/{_confgen,dyntypelib,xmltype},
# compat, etc, cmake over 3rdparty/cyclonedds, drop tests/ and xtests/ again,
# then re-apply the xtests guard above.
```

## IDL payload type

The transport carries already-encoded framework frames as opaque octet
samples. The generated C type lives with the driver at
`src/transport/dds/cy_dds_message.h` / `.c`; regenerate it with the matching
Cyclone DDS `idlc` and do not hand-edit the generated files.
