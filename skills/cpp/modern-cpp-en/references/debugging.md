# Debugging & Profiling Reference

## GDB / LLDB Practical Tips

### Common Command Comparison

| Operation | GDB | LLDB |
|-----------|-----|------|
| Run | `run` / `r` | `run` / `r` |
| Breakpoint | `break func` / `b file:line` | `b func` / `b file:line` |
| Conditional breakpoint | `b func if x > 10` | `b func -c 'x > 10'` |
| Print | `print expr` / `p expr` | `p expr` |
| Pretty-print containers | `p vec` (needs pretty-printer) | `p vec` |
| Watch variable changes | `watch var` | `w set var var` |
| Call stack | `bt` / `backtrace` | `bt` |
| Navigate frames | `up` / `down` | `up` / `down` |
| Thread list | `info threads` | `thread list` |
| Switch thread | `thread N` | `thread select N` |
| Memory inspection | `x/16xb ptr` | `memory read ptr -c 16` |

### STL Container Inspection

```bash
# GDB — install pretty-printer (most distros include it)
python import sys; sys.path.insert(0, '/usr/share/gcc/python')

# Direct print
(gdb) p my_vector
# $1 = std::vector of length 3, capacity 4 = {1, 2, 3}

(gdb) p my_map
# $2 = std::map with 2 elements = {["key1"] = "val1", ["key2"] = "val2"}
```

### Core Debugging Patterns

```bash
# Compile with debug info (don't strip)
g++ -g -O0 -fsanitize=address,undefined main.cpp -o debug_build

# Core dump analysis
ulimit -c unlimited
./crash_program
gdb ./crash_program core

# Attach to running process
gdb -p <pid>
```

## Reading Sanitizer Output

### ASan (AddressSanitizer)

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x6020000000d4
READ of size 4 at 0x6020000000d4 thread T0
    #0 0x4e5d82 in main /src/main.cpp:10:15      ← Error location
    #1 0x7f... in __libc_start_main

0x6020000000d4 is located 4 bytes after 20-byte region [0x6020000000c0,0x6020000000d4)
allocated by thread T0 here:                        ← Allocation location
    #0 0x... in operator new[](unsigned long)
    #1 0x4e5d20 in main /src/main.cpp:8:18
```

Key: Look at **`#0` or `#1` file names and line numbers**; compare allocation location vs error location.

### TSan (ThreadSanitizer)

```
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 4 at 0x... by thread T1:          ← Thread 1 writes
    #0 worker() /src/main.cpp:15

  Previous read of size 4 at 0x... by thread T2:   ← Thread 2 reads
    #0 reader() /src/main.cpp:22

  Location is global 'counter' of size 4           ← Contested variable
```

Key: Find the **two threads accessing the same location**, add a lock or switch to atomic.

### UBSan (UndefinedBehaviorSanitizer)

```
/src/main.cpp:10:5: runtime error: signed integer overflow:
2147483647 + 1 cannot be represented in type 'int'
```

Directly tells you the line number and specific UB type.

## Performance Profiling

### perf (Linux)

```bash
# Sample CPU hotspots
perf record -g ./myapp
perf report

# Count cache misses, branch misses, etc.
perf stat -e cache-misses,cache-references,branch-misses ./myapp

# Flame graph
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Valgrind

```bash
# Memory leak detection (20-50x slower than ASan, but more detailed)
valgrind --leak-check=full --show-reachable=yes ./myapp

# Cache analysis
valgrind --tool=cachegrind ./myapp
cg_annotate cachegrind.out.*

# Call graph analysis
valgrind --tool=callgrind ./myapp
kcachegrind callgrind.out.*  # GUI viewer
```

### Benchmarking

```cpp
// Core benchmarking principles (framework-agnostic):
// 1. Isolate the code under test
// 2. Prevent compiler from optimizing away results
// 3. Run multiple iterations, collect statistics

// Key technique: prevent optimization
void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

void clobber() {
    asm volatile("" : : : "memory");
}

// Simple timing
auto start = std::chrono::high_resolution_clock::now();
do_work();
auto end = std::chrono::high_resolution_clock::now();
auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
```

## Debugging Strategy

| Problem Type | Primary Tool |
|-------------|-------------|
| Crash / segfault | ASan + GDB core dump |
| Memory leak | ASan (`detect_leaks=1`) or Valgrind |
| Data race | TSan |
| Undefined behavior | UBSan |
| Performance bottleneck | perf → flame graph |
| Cache efficiency | perf stat / Cachegrind |
| Logic error | Conditional breakpoints + watch |
