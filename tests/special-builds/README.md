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

A test that documents a known, deliberately-unfixed defect (see the issue
tracker) is registered with `WILL_FAIL TRUE` and a comment in its source: it
keeps the CTest run green while the defect is present and turns red as soon as
the defect is fixed, signalling that it should be converted into a normal
regression test.

Run a special build locally, e.g.:

```sh
cmake -S tests/special-builds/locking-sysv -B @build-special \
      -DLIBMDBX_SOURCE_DIR="$PWD"
cmake --build @build-special
ctest --test-dir @build-special --output-on-failure
```