# fh-fundamental

fundamental kits for developer
# 功能简介
- 基于spdlog的日志封装
- 基于eventpp的事件封装，类qt的事件实现
- 线程池
- 定时器
- 基于rttr的反射封装，提供json序列化/反序列化，二进制序列化/反序列化
- rocksdb/sqlite3封装 使用示例
- 基于asio的rpc实现，ws协议代理管道，socks5代理，http,支持tls
- 命令行解析
- hash/base64
- 基于rttr实现的chaiscript绑定，可用于基于chai脚本动态测试
- 基于asio+kcp实现的rudp协议，支持rudp_server/rudp_client
# 目录结构
```
.
├── CMakeLists.txt
├── LICENSE.txt
├── README.md
├── applications 应用示例
├── assets 资源目录
├── build-linux release-with-debuginfo构建目录
├── build-linux-debug debug构建输出目录
├── cmake   cmake文件放置目录
├── cmake-gen-win.bat  windows vs工程生成脚本
├── samples  测试用例目录
├── scripts 常用脚本文件目录
├── src 项目库源码目录
├── test-gen-linux-debug-with-clang.sh
├── test-gen-linux-debug.sh
├── test-gen-linux-release-no-optimize.sh
├── test-gen-linux-release.sh
├── test-gen-linux.sh
├── test-install.sh
├── third-parties 三方库目录
├── vcpkg-help.md
└── vcpkg.json

applications/
├── CMakeLists.txt
├── rudp_delay_test_server  rudp延迟测试服务
└── tcp_custom_proxy_server socks5代理服务

cmake
├── TemplateLib.cmake.in 
├── clang-tidy-helper.cmake
├── config-target.cmake  通用项目配置
├── env-check.cmake  C++运行环境检查
├── import_benchmark_config.cmake benchmark导入配置
├── import_gtest_config.cmake gtest导入配置
├── lib-deploy.cmake 库/安装
├── patch-helper.cmake patch规则
├── platform.h.in
├── reflect-helper.cmake
└── source_import_config.cmake 当前项目cmake配置项

src/
├── CMakeLists.txt
├── database
│   ├── CMakeLists.txt
│   ├── dummy.cpp
│   ├── rocksdb
│   └── sqlite3
├── fundamental
│   ├── CMakeLists.txt
│   ├── algorithm
│   ├── application
│   ├── basic
│   ├── data_storage
│   ├── delay_queue
│   ├── events
│   ├── process
│   ├── read_write_queue
│   ├── rttr_handler
│   ├── thread_pool
│   └── tracker
├── http
│   ├── CMakeLists.txt
│   ├── http_connection.h
│   ├── http_definitions.cpp
│   ├── http_definitions.hpp
│   ├── http_request.cpp
│   ├── http_request.hpp
│   ├── http_response.cpp
│   ├── http_response.hpp
│   ├── http_router.cpp
│   ├── http_router.hpp
│   ├── http_server.cpp
│   └── http_server.hpp
├── network
│   ├── CMakeLists.txt
│   ├── io_context_pool.cpp
│   ├── io_context_pool.hpp
│   ├── network.hpp
│   ├── rudp
│   ├── upgrade_interface.hpp
│   └── use_asio.hpp
└── rpc
    ├── CMakeLists.txt
    ├── basic
    ├── connection.cpp
    ├── connection.h
    ├── proxy
    ├── rpc_client.hpp
    └── rpc_server.hpp

src/
├── CMakeLists.txt
├── database
│   ├── CMakeLists.txt
│   ├── dummy.cpp
│   ├── rocksdb
│   │   └── dummy.cpp
│   └── sqlite3   sqlite3数据库操作c++封装
│       ├── sqlite-common.hpp
│       ├── sqlite.hpp
│       └── sqliteext.hpp
├── fundamental
│   ├── CMakeLists.txt
│   ├── algorithm  range/hash操作
│   │   ├── common.hpp
│   │   ├── hash.hpp
│   │   ├── range_set.hpp
│   │   └── wyhash_utils.hpp
│   ├── application  应用资源管理
│   │   ├── application.cpp
│   │   └── application.hpp
│   ├── basic
│   │   ├── allocator.hpp  内存分配器
│   │   ├── arg_parser.hpp 命令行解析
│   │   ├── base64_utils.hpp
│   │   ├── buffer.hpp  buffer实现
│   │   ├── compress_utils.hpp 多线程压缩/zip格式封装
│   │   ├── cxx_config_include.hpp 
│   │   ├── endian_utils.hpp 大小端处理
│   │   ├── error_code.hpp  错误码定义
│   │   ├── filesystem_utils.hpp 文件读/写
│   │   ├── integer_codec.hpp 整数编码
│   │   ├── log.cpp
│   │   ├── log.h   日志宏
│   │   ├── md5_utils.hpp  md5
│   │   ├── mutext_utils.hpp 文件锁
│   │   ├── parallel.hpp 多线程并发执行
│   │   ├── random_generator.hpp 随机数生成
│   │   ├── string_utils.hpp 字符串处理
│   │   ├── url_utils.hpp url处理
│   │   ├── utils.cpp
│   │   ├── utils.hpp 常用工具实现
│   │   └── uuid_utils.hpp
│   ├── data_storage 数据读写
│   │   ├── data_storage.hpp
│   │   ├── data_storage_interface.hpp
│   │   ├── memory_data_storage.hpp
│   │   └── object_definitions.h
│   ├── delay_queue 定时器
│   │   ├── delay_queue.cpp
│   │   └── delay_queue.h
│   ├── events 事件/信号槽
│   │   ├── event.h
│   │   ├── event_process.cpp
│   │   ├── event_process.h
│   │   ├── event_system.cpp
│   │   └── event_system.h
│   ├── process 程序状态
│   │   ├── process_status.cc
│   │   └── process_status.h
│   ├── read_write_queue 线程安全无锁队列
│   │   ├── atomicops.h
│   │   ├── queue_with_locker.hpp
│   │   ├── readerwritercircularbuffer.h
│   │   └── readerwriterqueue.h
│   ├── rttr_handler 序列化/反序列化/反射
│   │   ├── binary_packer.cpp
│   │   ├── binary_packer.h
│   │   ├── deserializer.cpp
│   │   ├── deserializer.h
│   │   ├── meta_control.cpp
│   │   ├── meta_control.h
│   │   ├── script_support
│   │   │   └── chaiscript_visitor.h
│   │   ├── serializer.cpp
│   │   └── serializer.h
│   ├── thread_pool 线程池
│   │   ├── thread_pool.cpp
│   │   └── thread_pool.h
│   └── tracker
│       ├── memory_tracker.cpp
│       ├── memory_tracker.hpp
│       └── time_tracker.hpp
├── http http服务器实现
│   ├── CMakeLists.txt
│   ├── http_connection.h
│   ├── http_definitions.cpp
│   ├── http_definitions.hpp
│   ├── http_request.cpp
│   ├── http_request.hpp
│   ├── http_response.cpp
│   ├── http_response.hpp
│   ├── http_router.cpp
│   ├── http_router.hpp
│   ├── http_server.cpp
│   └── http_server.hpp
├── network
│   ├── CMakeLists.txt
│   ├── io_context_pool.cpp
│   ├── io_context_pool.hpp
│   ├── network.hpp
│   ├── rudp  rudp协议实现
│   │   ├── asio_rudp.cpp
│   │   ├── asio_rudp.hpp
│   │   ├── asio_rudp_definitions.hpp
│   │   ├── kcp_imp
│   │   │   ├── LICENSE
│   │   │   ├── ikcp.c
│   │   │   └── ikcp.h
│   │   └── readme.md
│   ├── upgrade_interface.hpp
│   └── use_asio.hpp
└── rpc 自定义rpc/socks5/自定义代理
    ├── CMakeLists.txt
    ├── basic
    │   ├── client_util.hpp
    │   ├── codec.h
    │   ├── const_vars.h
    │   ├── md5.hpp
    │   ├── meta_util.hpp
    │   └── router.hpp
    ├── connection.cpp
    ├── connection.h
    ├── proxy
    │   ├── protocal_pipe
    │   │   ├── forward_pipe_codec.hpp
    │   │   ├── pipe_connection_upgrade_session.hpp
    │   │   ├── protocal_pipe_connection.cpp
    │   │   └── protocal_pipe_connection.hpp
    │   ├── proxy_buffer.cpp
    │   ├── proxy_buffer.hpp
    │   ├── proxy_defines.h
    │   ├── proxy_manager.cpp
    │   ├── proxy_manager.hpp
    │   ├── rpc_forward_connection.cpp
    │   ├── rpc_forward_connection.hpp
    │   ├── socks5
    │   │   ├── common.cpp
    │   │   ├── common.h
    │   │   ├── socks5_proxy_session.hpp
    │   │   ├── socks5_session.cpp
    │   │   ├── socks5_session.h
    │   │   └── socks5_type.h
    │   ├── transparent_proxy_connection.hpp
    │   └── websocket
    │       ├── ws_common.cpp
    │       ├── ws_common.hpp
    │       ├── ws_forward_connection.cpp
    │       ├── ws_forward_connection.hpp
    │       └── ws_upgrade_session.hpp
    ├── rpc_client.hpp
    └── rpc_server.hpp

third-parties/
├── CMakeLists.txt
├── ChaiScript  脚本支持
├── asio-source 异步io
├── eventpp-source 事件
├── nlohmann-source json
├── rttr-source 反射
├── spdlog-source 日志
└── wingetopt 命令行解析windows支持
```

# 构建

## 构建系统要求
```
ubuntu 22.04及以上
windows msvc 2022及以上+(vcpkg)
cmake 3.22及以上
g++9及以上
c++17
```
## 编译
```
##release
./test-gen-linux.sh
cd ./build-linux && make -j8

##debug
./test-gen-linux-debug.sh
cd ./build-linux-debug && make -j8

```

# 其它
## 内存泄漏排查
```
cmake配置参数增加 -DDISABLE_DEBUG_SANITIZE_ADDRESS_CHECK=ON -DENABLE_JEMALLOC_MEMORY_PROFILING=ON
运行时增加环境变量 export MALLOC_CONF="prof:true,prof_active:true,lg_prof_sample:0,prof_leak:true,prof_accum:true"

执行程序后生成 heap文件，这里使用TestBasic生成的两个文件来比较，比较命令如下
jeprof --text --show_bytes --lines --base=1.out samples/TestBasic/TestBasic 2.out
示例输出:
Total: 448 B
     448 100.0% 100.0%      448 100.0% main /home/lightning/work/fh-fundamental/samples/TestBasic/src/TestBasic.cpp:112 (discriminator 4)
       0   0.0% 100.0%      448 100.0% __libc_start_call_main ./csu/../sysdeps/nptl/libc_start_call_main.h:58
       0   0.0% 100.0%      448 100.0% __libc_start_main_impl ./csu/../csu/libc-start.c:392
       0   0.0% 100.0%      448 100.0% _start ??:?
第112行
```
