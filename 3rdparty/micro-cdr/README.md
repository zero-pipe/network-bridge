# Micro-CDR (vendored)

Subset of [Micro-CDR 2.0.2](https://github.com/eProsima/Micro-CDR) used by the
RTPS codec. Only `include/`, `src/` and `CMakeLists.txt` are kept; upstream
tests, examples, CI, packaging and documentation were removed when the
dependency was vendored, so this tree is not a Git submodule.

The library is built as a static target `microcdr` and linked into `nb`.

Licensed under the Apache License 2.0, see `LICENSE`.
