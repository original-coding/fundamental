# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Release build (RelWithDebInfo)
./test-gen-linux.sh
cd ./build-linux && make -j$(nproc)

# Debug build (with Address Sanitizer)
./test-gen-linux-debug.sh
cd ./build-linux-debug && make -j$(nproc)

# Release build (no optimization, easier debugging)
./test-gen-linux-release-no-optimize.sh && cd ./build-linux-release-no-optimize && make -j$(nproc)

# Release build with precompiled headers
./test-gen-linux-release-precompile.sh && cd ./build-linux-release-precompile && make -j$(nproc)

# Build with clang-tidy
./test-gen-linux-debug-with-clang.sh

# Install built libraries (after make)
cmake --install .

# Run tests via ctest
cd build-linux && ctest --output-on-failure

# Run a single test executable directly
./build-linux/samples/TestBasic/TestBasic
```

## Architecture Overview

This is a C++ utility library providing foundational components for network applications.

### Core Modules (`src/fundamental/`)

- **algorithm** - range/hash operations, wyhash/BLAKE3 utilities
- **application** - application lifecycle management (singleton event loop)
- **basic** - allocators, arg_parser, base64, buffer, compress_utils (zlib + parallel deflate), endian_utils, error_code, file_utils, integer_codec, log (spdlog), md5, mutex_utils, parallel execution, random_generator, string_utils, url_utils, uuid_utils
- **data_storage** - RTTR-based in-memory key-value store
- **delay_queue** - timer/delayed task scheduler
- **events** - event system using eventpp (signal/slot pattern)
- **io** - CSV file read/write
- **process** - process status monitoring (CPU/memory usage)
- **read_write_queue** - lock-free queues (readerwriterqueue, readerwritercircularbuffer) and step task executor
- **rttr_handler** - serialization/deserialization using RTTR reflection (JSON, binary packing)
- **thread_pool** - parallel task thread pool
- **tracker** - memory_tracker, time_tracker for profiling

### Network Modules

- **network/io_context_pool** - multi-threaded asio io_context pool
- **network/rudp** - KCP-based reliable UDP with connection state management (SYN/SYN_ACK/FIN/PING control protocol)
- **rpc** - RPC framework with proxy support (SOCKS5, WebSocket forwarding, FRP NAT traversal)
- **http** - lightweight asio-based HTTP/1.1 server

### Database (`src/database/`)

- **sqlite3** - SQLite wrapper
- **rocksdb** - RocksDB support (disabled by default)

### Applications (`applications/`)

- **frp_proxy_server** - FRP public server (signaling coordination, relay forwarding)
- **frp_proxy_client** - FRP unified client (one device, one signal channel; can simultaneously register services as provider and subscribe as accessor)
- **frp_echo_test** - TCP echo server/client for FRP integration testing
- **rudp_delay_test_server** - RUDP latency testing server
- **tcp_custom_proxy_server** - SOCKS5 proxy server

### Third-Party Dependencies

- **asio** - async I/O (standalone mode)
- **eventpp** - heterogeneous event dispatcher
- **nlohmann/json** - JSON parsing and serialization
- **rttr** - runtime type reflection
- **spdlog** - logging framework
- **quickjs/quickjspp** - JavaScript scripting support (optional)
- **OpenSSL** - TLS/SSL, cryptographic primitives
- **zlib** - compression
- **Google Test** - unit testing
- **Google Benchmark** - performance benchmarking

## CMake Options

Key configuration options (passed as `-DOPTION=ON/OFF`):

| Option | Default | Description |
|--------|---------|-------------|
| `FUNDAMENTAL_BUILD_NETWORK` | ON | Build network library |
| `FUNDAMENTAL_BUILD_APPLICATIONS` | ON | Build application executables |
| `FUNDAMENTAL_ENABLE_DATABASE_SUPPORT` | ON | Enable database modules |
| `FUNDAMENTAL_BUILD_RTTR` | ON | Enable RTTR serialization |
| `FUNDAMENTAL_BUILD_EVENTS` | ON | Enable events module |
| `FUNDAMENTAL_ENABLE_INSTALL` | ON | Enable install targets |
| `FUNDAMENTAL_ENABLE_SCRIPT_SUPPORT` | ON (C++20) | Enable JS scripting |
| `FUNDAMENTAL_ENABLE_SQLITE3_SUPPORT` | ON | SQLite support |
| `FUNDAMENTAL_ENABLE_SQLITE3_LOADABLE_EXT_SUPPORT` | OFF | SQLite loadable extensions |
| `FUNDAMENTAL_ENABLE_ROCKSDB_SUPPORT` | OFF | RocksDB support |
| `FUNDAMENTAL_LOG_PRINT_FILE_NAME` | ON | Include file name in log output |
| `IMPORT_GTEST` | ON | Build with Google Test |
| `IMPORT_BENCHMARK` | ON | Build benchmarks |
| `ENABLE_DEBUG_MEMORY_TRACE` | ON | Memory tracking in debug |
| `DISABLE_DEBUG_SANITIZE_ADDRESS_CHECK` | OFF | Disable ASAN in debug |
| `ENABLE_JEMALLOC_MEMORY_PROFILING` | OFF | Enable jemalloc heap profiling |

## Memory Leak Detection

```bash
cmake ... -DDISABLE_DEBUG_SANITIZE_ADDRESS_CHECK=ON -DENABLE_JEMALLOC_MEMORY_PROFILING=ON
export MALLOC_CONF="prof:true,prof_active:true,lg_prof_sample:0,prof_leak:true,prof_accum:true"
# Run program to generate .heap files
jeprof --text --show_bytes --lines --base=1.out program 2.out
```

## Key Build Targets

- `fundamental` (or `fh::fundamental`) - static library (`build-linux/src/fundamental/libfundamental.a`)
- `network` - static library for network components
- `rpc` - static library for RPC framework
- `http` - static library for HTTP server
- Test executables: `build-linux/samples/TestBasic/TestBasic`
- Benchmark executables: `build-linux/samples/RpcBenchmark/RpcBenchmark`
