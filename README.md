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
├── .clang-format
├── .gitignore
├── .gitmodules
├── CMakeLists.txt
├── LICENSE.txt
├── README.md
├── build-linux     release 构建目录
├── build-linux-debug debug构建目录
├── cmake    cmake文件目录
│   ├── TemplateLib.cmake.in  库导出模板
│   ├── config-target.cmake   配置target的相关函数声明
│   ├── import_gtest_config.cmake  gtest导入相关配置
│   ├── lib-deploy.cmake    自定义库安装相关配置
│   ├── platform.h.in   c++编译precompile头，可参考此文件的方式添加自己的预编译头
│   └── reflect-helper.cmake 反射注册辅助函数
├── samples   测试用例
│   ├── CMakeLists.txt
│   ├── GtestSample   gtest库导入测试
│   ├── TestApplication application流程测试
│   ├── TestAsio   asio库导入测试
│   ├── TestBasic  c++环境测试
│   ├── TestDelayQueue    定时器测试
│   ├── TestEvents   事件测试
│   ├── TestLog   日志测试
│   ├── TestMemoryTracker  内存跟踪测试
│   ├── TestParallel  并行单元测试
│   ├── TestProxyServer   转发服务及客户端
│   ├── TestRttr   rttr反射测试
│   └── TestTrafficProxy  流量代理客户端
├── test-gen-linux-debug.sh  debug版本构建脚本
├── test-gen-linux.sh    release版本构建脚本
└── third-parties 三方库源码目录
    ├── CMakeLists.txt
    ├── asio-source   
    ├── eventpp-source   
    ├── nlohmann-source
    ├── rttr-source
    └── spdlog-source
./src/
├── CMakeLists.txt
├── fundamental   基础组件
│   ├── CMakeLists.txt
│   ├── application   应用程序启动流程标准化定义
│   │   ├── application.cpp
│   │   └── application.hpp
│   ├── basic  
│   │   ├── buffer.hpp  自定义buffer
│   │   ├── log.cpp 
│   │   ├── log.h   spdlog封装logger
│   │   ├── parallel.cpp
│   │   ├── parallel.hpp  并行封装,基于thread_pol
│   │   ├── utils.cpp
│   │   └── utils.hpp  常用的utils
│   ├── delay_queue  定时器事件实现
│   │   ├── delay_queue.cpp
│   │   └── delay_queue.h
│   ├── events   基于eventpp异步事件/同步信号定义
│   │   ├── event.h
│   │   ├── event_process.cpp
│   │   ├── event_process.h 应用层事件/信号定义
│   │   ├── event_system.cpp
│   │   └── event_system.h  事件系统封装
│   ├── rttr_handler  基于rttr及nlohmann-json的序列化实现
│   │   ├── deserializer.cpp
│   │   ├── deserializer.h   序列化
│   │   ├── meta_control.cpp
│   │   ├── meta_control.h  序列化反序列化流程控制
│   │   ├── serializer.cpp
│   │   └── serializer.h  反序列化
│   ├── thread_pool  可取消等待中任务/提供future的线程池实现
│   │   ├── thread_pool.cpp
│   │   └── thread_pool.h
│   └── tracker  
│       └── memory_tracker.hpp  内存分配跟踪器基类
└── network  基于asio的网络模块实现
    ├── CMakeLists.txt
    ├── server  
    │   ├── basic_server.hpp  通用tcp服务器声明
    │   ├── io_context_pool.cpp 
    │   └── io_context_pool.hpp  asio-io-context-pool实现，基于thread_pool
    └── services 基于basic_server的echo实例
        ├── echo
        └── proxy_server 多服务共用端口的server实现
            ├── agent_service  数据存储/查询服务实现
            ├── proxy_connection.cpp
            ├── proxy_connection.hpp
            ├── proxy_defines.h proxy数据格式定义
            ├── proxy_encode.h  prxoy数据编码
            ├── proxy_request_handler.cpp
            ├── proxy_request_handler.hpp proxy数据处理
            ├── readMe.md
            └── traffic_proxy_service   流量代理服务实现
```

# 构建

## 构建系统要求
```
ubuntu 22.04及以上
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
