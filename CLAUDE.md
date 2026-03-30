# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Release build (RelWithDebInfo)
./test-gen-linux.sh
cd ./build-linux && make -j8

# Debug build
./test-gen-linux-debug.sh
cd ./build-linux-debug && make -j8

# Build with clang-tidy
./test-gen-linux-debug-with-clang.sh

# Run tests via ctest
cd build-linux && ctest --output-on-failure

# Run a single test executable directly
./build-linux/samples/TestBasic/TestBasic
```

## Architecture Overview

This is a C++ utility library providing foundational components for network applications.

### Core Modules (`src/fundamental/`)

- **algorithm** - range/hash operations, wyhash utilities
- **basic** - allocators, arg_parser, buffer, compress_utils, endian_utils, log, md5, mutex_utils, parallel execution, random_generator, string_utils, url_utils, uuid_utils
- **events** - event system using eventpp (signal/slot pattern)
- **delay_queue** - timer implementation
- **thread_pool** - thread pool for task execution
- **tracker** - memory_tracker, time_tracker for profiling
- **data_storage** - in-memory data storage interface
- **rttr_handler** - serialization/deserialization using RTTR reflection
- **read_write_queue** - lock-free queues (readerwriterqueue, readerwritercircularbuffer)

### Network Modules

- **network/rudp** - KCP-based reliable UDP implementation (`src/network/rudp/`)
- **rpc** - RPC framework with proxy support (SOCKS5, WebSocket forwarding)
- **http** - HTTP server implementation

### Database (`src/database/`)

- **sqlite3** - SQLite wrapper
- **rocksdb** - RocksDB support (disabled by default)

### Applications (`applications/`)

- **rudp_delay_test_server** - RUDP latency testing server
- **tcp_custom_proxy_server** - SOCKS5 proxy server

### Third-Party Dependencies

- **asio** - async I/O
- **eventpp** - heterogeneous event dispatcher
- **nlohmann/json** - JSON parsing
- **rttr** - runtime type reflection
- **spdlog** - logging
- **quickjs/quickjspp** - JavaScript scripting support

## CMake Options

Key configuration options (passed as `-DOPTION=ON/OFF`):

| Option | Default | Description |
|--------|---------|-------------|
| `FUNDAMENTAL_BUILD_NETWORK` | ON | Build network library |
| `FUNDAMENTAL_ENABLE_DATABASE_SUPPORT` | ON | Enable database modules |
| `FUNDAMENTAL_BUILD_RTTR` | ON | Enable RTTR serialization |
| `FUNDAMENTAL_BUILD_EVENTS` | ON | Enable events module |
| `FUNDAMENTAL_ENABLE_SCRIPT_SUPPORT` | ON (C++20) | Enable JS scripting |
| `FUNDAMENTAL_ENABLE_SQLITE3_SUPPORT` | ON | SQLite support |
| `FUNDAMENTAL_ENABLE_ROCKSDB_SUPPORT` | OFF | RocksDB support |
| `IMPORT_GTEST` | ON | Build with Google Test |
| `IMPORT_BENCHMARK` | ON | Build benchmarks |
| `ENABLE_DEBUG_MEMORY_TRACE` | ON | Memory tracking in debug |
| `DISABLE_DEBUG_SANITIZE_ADDRESS_CHECK` | OFF | Disable ASAN in debug |

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
