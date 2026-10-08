# Serialization & Type Punning Reference

## std::bit_cast (C++20) — Safe Bit-Level Conversion

```cpp
#include <bit>

// Safe: works at compile time, no UB
float f = 3.14f;
auto bits = std::bit_cast<std::uint32_t>(f);  // View float's bit representation

// Reverse
auto f2 = std::bit_cast<float>(bits);  // bits → float

// Requirement: source and target types must have same size and be trivially copyable
static_assert(sizeof(float) == sizeof(std::uint32_t));
```

### What It Replaces

```cpp
// Old method 1: reinterpret_cast — violates strict aliasing (UB)
float f = 3.14f;
auto bits = *reinterpret_cast<uint32_t*>(&f);  // UB

// Old method 2: memcpy — safe but verbose, not constexpr
float f = 3.14f;
uint32_t bits;
std::memcpy(&bits, &f, sizeof(bits));  // Safe but runtime only

// New method: bit_cast — safe + constexpr
constexpr auto bits = std::bit_cast<uint32_t>(3.14f);  // Compile time!
```

## Network Byte Order

```cpp
#include <bit>

// Compile-time endianness detection
if constexpr (std::endian::native == std::endian::little) {
    // x86/ARM (most platforms)
} else if constexpr (std::endian::native == std::endian::big) {
    // Network byte order / some embedded
}

// C++23: std::byteswap
#include <bit>
std::uint32_t host_val = 0x12345678;
std::uint32_t net_val = std::byteswap(host_val);  // Byte reversal

// Network order conversion (replaces htonl / ntohl)
constexpr auto to_network(std::uint32_t val) -> std::uint32_t {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(val);
    } else {
        return val;
    }
}
```

## Binary Serialization Patterns

### Simple POD Serialization

```cpp
// Only for trivially copyable types; not cross-platform
struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t payload_size;
};
static_assert(std::is_trivially_copyable_v<Header>);

// Write
void write(std::ostream& os, const Header& h) {
    os.write(reinterpret_cast<const char*>(&h), sizeof(h));
}

// Read
Header read_header(std::istream& is) {
    Header h;
    is.read(reinterpret_cast<char*>(&h), sizeof(h));
    return h;
}
```

### Cross-Platform Safe Serialization

```cpp
// Manually control byte order and alignment
void serialize(std::span<std::byte> buf, const Header& h) {
    auto write_u32 = [&](std::size_t offset, std::uint32_t val) {
        val = to_network(val);
        std::memcpy(buf.data() + offset, &val, sizeof(val));
    };
    auto write_u64 = [&](std::size_t offset, std::uint64_t val) {
        // Write in network byte order
        for (int i = 7; i >= 0; --i) {
            buf[offset + (7 - i)] = static_cast<std::byte>(val >> (i * 8));
        }
    };

    write_u32(0, h.magic);
    write_u32(4, h.version);
    write_u64(8, h.payload_size);
}
```

## std::start_lifetime_as (C++23)

```cpp
// Create object from raw bytes without placement new
// Only for implicit-lifetime types
auto* header = std::start_lifetime_as<Header>(buffer.data());
```

## Selection Guide

| Scenario | Approach |
|----------|----------|
| View float bit representation | `std::bit_cast` |
| Structured data serialization | Protocol Buffers / FlatBuffers / Cap'n Proto |
| Simple binary protocol | Manual serialization + explicit byte order |
| Local IPC (same architecture) | `memcpy` / `bit_cast` (mind alignment) |
| Network transport | Big-endian byte order (`std::byteswap` helper) |
| Type punning | `std::bit_cast` (never `reinterpret_cast`) |
