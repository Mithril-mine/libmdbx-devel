# Tests with special libmdbx build options

Some bugs only manifest under specific libmdbx build configurations (for
example a non-default locking implementation). The default CI builds the
library once with the auto-detected options, so such defects need a separate
build of the library with the required options.

This directory holds one subdirectory per required option set:

```
tests/special-builds/
  locking-sysv/        # libmdbx built with MDBX_LOCKING=5 (System V IPC)
```

Each subdirectory is a self-contained CMake project:

- `CMakeLists.txt` builds the libmdbx SDK from the repository root with the
  required options and registers the tests via CTest, and
- the test sources live next to it.

Run a special build locally, e.g.:

```sh
cmake -S tests/special-builds/locking-sysv -B @build-special \
      -DLIBMDBX_SOURCE_DIR="$PWD"
cmake --build @build-special
ctest --test-dir @build-special --output-on-failure
```

The `locking-sysv` tests need the probe-bus at run time (`MDBX_PROBES=1`),
which `CMakeLists.txt` sets on the affected CTest tests.