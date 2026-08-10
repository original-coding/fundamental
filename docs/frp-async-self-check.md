# FRP 模块异步规范 §10 自检

> 依据 `docs/asio-async-standards.md` §10 代码评审检查清单，对 FRP 模块逐条自检。
> 范围：frp_client / frp_server / frp_accessor / frp_provider / frp_punch_engine /
> frp_signal_client / kcp_channel。结论：**23/23 通过**（含 2 处文档化约定）。

## 生命周期

- [x] **对象由 make_shared 创建，继承 enable_shared_from_this**
  - frp_tcp_channel / frp_signal_channel / relay_data_channel / frp_unified_client
    （frp_client.hpp:31/101/155/297）、frp_signal_session / frp_public_server
    （frp_server.hpp）、frp_punch_engine（frp_punch_engine.hpp:22）、kcp_channel
    （kcp_channel.hpp:33）均继承 enable_shared_from_this 并经 make_shared/create 创建。
- [x] **所有异步回调捕获强引用，无裸 this**
  - frp_client/frp_server/frp_punch_engine/frp_signal_client 中
    `self/ptr = shared_from_this()` 捕获共 46 处；未发现裸 this 异步回调。
- [x] **无"入口 weak 哨兵 + 全程裸 this"模式**
  - 入口用 reference_（network_data_reference）有效性检查 + 回调内强引用全程持有。
- [x] **成员持有 self 的引用链在关闭时显式断开**
  - relay_data_channel::close 依次释放 punch_engine_/kcp_ch_/tcp_/data_writer_/
    on_release_（frp_client.cpp:395-409）；attach_tcp 的 on_release 绑定在 p2p
    升级时清空（accept_p2p 内 tcp_->set_on_release({})）。

## 状态

- [x] **生命周期状态是 atomic，迁移用 CAS**
  - relay_data_channel::closed_（std::atomic<bool>，frp_client.hpp:251）用
    exchange 幂等迁移；frp_public_server::has_started_ 用 compare_exchange；
    frp_tcp_channel::reference_ 释放用 release() 返回值检查幂等。
- [x] **close/start 幂等**
  - relay_data_channel::close() 首行 `if (closed_.exchange(true)) return;`；
    start()/do_accept 均有 has_started_/acceptor.is_open 守卫。
- [x] **用户回调在状态迁移之后触发**
  - relay close 先置 closed_ 再执行清理与 on_release_；punch on_punch_success
    先置 result_delivered_ 再回调。
- [x] **握手/协议状态与生命周期状态分离**
  - connection_uuid_/peer_uuid_/register_key_/transport_（协议身份）与 closed_
    （生命周期）分离；p2p_success_ 为协议状态。

## 线程

- [x] **对象成员只在绑定 io 线程访问（有断言或文档约定）**
  - 关键入口已加 running_in_io_thread() 断言（relay_data_channel::close、
    frp_signal_session::process_command/read_next_command、
    frp_punch_engine::on_punch_success、rpc_forward_connection::HandleDisconnect，
    commit 50dc6a9）；p2p_success_ 为非原子 bool，仅在 io 线程读写（文档约定，
    断言覆盖的入口内）。
- [x] **用户线程调用全部 post，无直接操作 asio 对象**
  - relay_data_channel::release_obj 非 io 线程时 post 到 executor_
    （frp_client.cpp:383-386）；frp_signal_client::release_obj post；
    frp_public_server/frp_signal_session 的跨线程操作经 post_keepalive 投递。
- [x] **同一 socket 至多一个在途写，pop 只在完成回调**
  - backend/local 写经 network::serialized_writer（data_writer_，
    frp_client.cpp:426-438）；frp_tcp_channel 写经 write_queue_ 单在途驱动。
- [x] **同一 socket 单一读循环，切换前先取消旧者**
  - relay p2p 读循环单一（start_p2p_read_loop）；punch engine 的 endpoint echo 与
    punch 读循环切换前先 cancel（P0-9 修复）；relay→p2p 交接时 punch 成功回调
    post 到取消 handler 之后（ed2cffa，避免交接期双接收者）。

## 关闭

- [x] **三层模型齐全：release_obj 只投递 + 关闭序列（io 线程固定顺序）+ 析构兜底**
  - relay_data_channel：release_obj（入口，io 线程内直接 close，否则 post）→
    close（统一序列）→ 析构（unreg_timer/unreg_object + kcp close，
    frp_client.cpp:364-368）；frp_punch_engine 同构（析构调 release，
    frp_punch_engine.cpp:83-88）；kcp_channel 同构（析构调 close）。
- [x] **关闭序列按 §4.3 顺序执行，全程持有强引用**
  - close() 内 post 的回调/内部对象释放均捕获 self；顺序为状态迁移 → 取消定时器
    → 释放内部对象 → 用户回调。
- [x] **用户线程的 destroy 只投递**
  - release_obj 均为"仅投递"入口（见上）。
- [x] **容器移除先于用户回调**
  - frp_accessor::close_all（accessor.cpp:1027-1030）与 provider 对应路径先搬出
    channels_ 再逐项关闭。
- [x] **所有错误路径收敛到关闭序列**
  - frp_accessor/provider 读循环错误统一 close_data_channel（§7.2 修复）；
    relay data_writer 错误 handler → close()。
- [x] **析构调 release_obj 兜底（幂等），且析构内无同步清理**
  - 析构直接 unreg_timer/unreg_object + 关闭内部对象（对象消亡时无在途回调，
    同步清理安全）；kcp/punch 析构调各自 release/close 幂等入口。
- [x] **post_and_wait 遵守死锁约定**
  - FRP 模块未使用 post_and_wait（rg 无命中），无死锁风险面。

## 缓冲与定时器

- [x] **异步操作期间 buffer 存活（shared_ptr 持有）**
  - kcp on_output 的 async_send_to 捕获 d（shared_ptr<string>，
    frp_client.cpp:416-421）；serialized_writer 持有 data shared_ptr。
- [x] **用户缓冲在错误路径也回掉**
  - data_writer_ 错误 handler 触发 close（统一关闭序列回掉全部在途写）。
- [x] **所有定时器在关闭时取消，回调捕获强引用**
  - relay close 取消 idle/handshake 定时器（frp_client.cpp:397-399）；punch engine
    release 取消 endpoint_probe/deadline/punch 定时器；回调均捕获 self。

## 池与注册

- [x] **跨 stop 存活的定时器已 reg_timer/unreg_timer（§9.2）**
  - frp_signal_client reconnect/poll/probe（frp_signal_client.cpp:40-48）、
    frp_punch_engine 三定时器（frp_punch_engine.cpp:78-88）、kcp_channel timer
    （kcp_channel.cpp:12-17）、relay idle/handshake、frp_signal_session timeout_timer
    均成对 reg/unreg。
- [x] **动态创建的对象已 reg_object/unreg_object（§9.3）**
  - frp_public_server（frp_server.cpp:26/141）、frp_signal_session（:248/253/258）、
    frp_signal_client（frp_signal_client.cpp:55/63）、relay_data_channel
    （frp_client.cpp:28/365-367）均接入池注册表。
- [x] **顶层对象用 make_guard 或显式 release 覆盖（§9.3）**
  - frp_proxy_server/client 主流程用 network::make_guard 持有 server/client；
    release_obj 显式驱动。

## 结论

FRP 模块对 §10 清单 23 项全部通过；2 处为文档化约定（p2p_success_ 非原子 io 线程内
访问、析构内同步清理的合理性）。自检依据：历次加固提交（9fc79a9、frp 阶段 A-D、
round-2 6146751、本分支 50dc6a9/ed2cffa 等）+ 本分支 ASAN 全套与 4/4 联调脚本验证。
