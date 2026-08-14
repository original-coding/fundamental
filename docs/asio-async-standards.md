# asio 异步开发规范化文档

> 版本：v1.0（2026-08-03）
> 适用范围：fh-fundamental 所有基于 asio 的网络代码（网络层、RPC、HTTP、代理、应用层）
> 强制程度：**必须（MUST）/ 应该（SHOULD）/ 建议（MAY）**，与 RFC 2119 语义一致
> 配套文档：`docs/refactoring-plan.md`（改造计划）、`src/rpc/proxy/frp/FRP_PROTOCOL.md`（FRP 协议现状）

---

## 0. 三条根本原则

所有规范都由这三条原则推导而来：

1. **所有权决定生死**：异步回调执行时，其引用的对象必须还活着。办法只有一个——回调持有对象的 `shared_ptr`（强引用）。
2. **单线程决定安全**：一个对象的所有状态只能在一个线程（其绑定的 io 线程）上修改。其他线程想碰它，只能投递（post）。
3. **状态决定行为**：对象的可操作阶段（启动中/运行中/关闭中/已关闭）必须是显式的状态机，任何时刻查询结果确定，任何迁移幂等。

---

## 1. 生命周期规范（所有权）

### 1.1 对象创建：MUST 使用 `make_shared` + `enable_shared_from_this`

异步对象（连接、会话、通道）的生命周期由引用计数决定，必须堆上创建、由 shared_ptr 管理。

**错误示例：**

```cpp
// 错误：裸指针创建，无法安全取得自身所有权
class Session {
public:
    void start() {
        asio::async_read(socket_, buf_, [this](auto ec, auto n) { ... });
    }
};
Session* s = new Session();          // 之后没有任何安全途径获得 shared_ptr

// 错误：栈上创建，shared_from_this() 直接 UB
Session s;
s.start();                           // 内部一旦调用 shared_from_this() → 崩溃
```

**正确示例：**

```cpp
class Session : public std::enable_shared_from_this<Session> {
public:
    static std::shared_ptr<Session> create() { return std::make_shared<Session>(); }
    void start() { ... }
};
auto s = Session::create();
```

### 1.2 异步回调：MUST 捕获强引用，禁止捕获裸 `this`

**错误示例：**

```cpp
void Session::start() {
    asio::async_write(socket_, buf_,
        [this](std::error_code ec, std::size_t n) {   // 危险：裸 this
            this->on_written(ec, n);                  // 对象销毁后执行 → use-after-free
        });
}
```

**正确示例：**

```cpp
void Session::start() {
    auto self = shared_from_this();                   // 先持有所有权
    asio::async_write(socket_, buf_,
        [self, this](std::error_code ec, std::size_t n) {  // self 保活整个回调
            this->on_written(ec, n);
        });
}
```

### 1.3 禁止"入口弱引用哨兵 + 全程裸 this"

在回调入口用 `weak_from_this().lock()` 检查一次、之后全程使用裸 `this`，是**错误模式**：哨兵只保护进入时刻。回调执行中途，其他回调（例如用户回调）可能销毁对象，此时本回调的裸 `this` 立即悬垂。

**错误示例：**

```cpp
timer_.async_wait([this, weak = weak_from_this()](auto ec) {
    auto strong = weak.lock();
    if (!strong) return;              // 只保护了这一行
    this->check_health();             // 中途可能被别的回调销毁 → 悬垂
    this->send_ping();
    this->do_something_long();        // 用户回调可能在 do_something_long 里销毁对象
});
```

**正确示例（二选一）：**

```cpp
// 方式 A：全程强引用（推荐——对象在回调期间必然存活）
timer_.async_wait([self = shared_from_this()](auto ec) {
    self->check_health();
});

// 方式 B：每次使用前重新提升
timer_.async_wait([weak = weak_from_this()](auto ec) {
    if (auto strong = weak.lock()) strong->check_health();
    if (auto strong = weak.lock()) strong->send_ping();   // 每次重新 lock
});
```

### 1.4 循环引用：MUST 显式断开

对象持有成员定时器/资源，定时器回调捕获对象强引用 → 循环引用，引用计数永不归零。

**错误示例：**

```cpp
class Session : public std::enable_shared_from_this<Session> {
    asio::steady_timer timer_;                        // Session → timer_
    void start() {
        timer_.async_wait([self = shared_from_this()](auto) { ... });  // timer_ → Session
        // 成环：两者互相持有，永不释放（内存泄漏）
    }
};
```

**正确示例：**

```cpp
class Session : public std::enable_shared_from_this<Session> {
    asio::steady_timer timer_;
    void stop() {
        timer_.cancel();                              // 关闭时取消定时器，断开引用链
        user_data_.reset();                           // 断开可能持有 self 的成员
    }
};
```

规则：**凡对象成员可能持有指向自身的 shared_ptr，stop/close 时必须主动断开，不要指望析构。**

### 1.5 回调触发的用户代码可能销毁对象：MUST 保证销毁发生在安全点

用户回调（如断开通知）内部可能释放最后一个 handle。此时**发起该回调的内部操作必须还持有强引用**，否则回调返回后继续执行的下一条语句就是 UAF。

**错误示例：**

```cpp
void close() {
    // 错误：先调用户回调，之后继续用 this
    if (closed_cb_) closed_cb_();     // 用户回调里销毁了对象
    timer_.cancel();                  // ← 已释放内存上的操作
    socket_.close();
}
```

**正确示例：**

```cpp
void close() {
    auto self = shared_from_this();   // 整个关闭过程持有所有权
    timer_.cancel();
    socket_.close();
    if (closed_cb_) closed_cb_();     // 回调在最后；即使回调里销毁对象，self 保活到函数结束
}
```

---

## 2. 状态机规范

### 2.1 生命周期状态：MUST 用 `std::atomic` + CAS 迁移

对象状态（started/stopping/closed 等）会被多个线程查询（用户线程）与修改（io 线程），普通成员跨线程读写是未定义行为。

**错误示例：**

```cpp
enum class State { Closed, Starting, Started, Stopping };
State state_ = State::Closed;              // 错误：普通成员

void set_state(State s) {
    if (state_ == s) return;               // 检查+赋值非原子，多线程下行为未定义
    state_ = s;
}
bool is_closed() const { return state_ == State::Closed; }  // 用户线程读，io 线程写 → 竞争
```

**正确示例：**

```cpp
std::atomic<State> state_{State::Closed};

// CAS 迁移：只有期望值匹配才迁移；失败则返回 false（幂等）
bool try_transition(State expected, State desired) {
    return state_.compare_exchange_strong(expected, desired);
}

bool close() {
    // 任何线程调用；无论 started 还是 starting 都迁移到 stopping；已关闭则 no-op
    if (try_transition(State::Started, State::Stopping)) return true;
    if (try_transition(State::Starting, State::Stopping)) return true;
    return false;                          // 已经在关闭/已关闭 → 幂等返回
}
```

### 2.2 状态迁移：MUST 幂等

重复的 start/stop/close 调用必须无副作用。CAS 失败即返回天然实现幂等，禁止"直接赋值后执行清理"。

### 2.3 用户回调：MUST 在状态迁移完成之后触发

**错误示例：**

```cpp
void on_connected_ready() {
    if (connected_cb_) connected_cb_();   // 错误：先回调
    state_ = State::Started;              // 用户在回调里查询 is_started() → 得到旧值
}
```

**正确示例：**

```cpp
void on_connected_ready() {
    state_ = State::Started;              // 先迁移
    if (connected_cb_) connected_cb_();   // 回调里查询到的状态是确定的
}
```

### 2.4 协议握手状态与生命周期状态：MUST 分离

握手状态（SYN 已发、等待 ACK、upgrade 协商中……）是协议细节；生命周期状态（能否收发数据）是管理语义。两者混在一个枚举里，会让状态迁移承担两种不同职责，线程安全要求互相污染。

**正确示例：**

```cpp
// 生命周期层：atomic + CAS，任何线程可查询
std::atomic<State> life_state_{State::Closed};
// life_state_ 的取值：Closed → Starting（开始连接/握手）→ Started（可收发）→ Stopping → Closed

// 握手层：只在 io 线程读写的普通成员，不需要原子
HandshakePhase handshake_phase_ = HandshakePhase::SynSent;
// 协议细节只在 io 线程演进；对外只暴露生命周期层
```

### 2.5 状态机：MUST 有完整迁移图

任何新状态机必须书面定义全部合法迁移（谁触发、在哪个线程、条件是什么）。不允许存在"任何状态都可跳转"的隐式状态机。

---

## 3. 线程模型规范

### 3.1 线程亲和性：MUST —— 对象成员只在其绑定 io 线程访问

每个异步对象绑定一个 io_context，其所有成员（socket、timer、buffer、状态、队列）只能在绑定线程访问。这是 asio 对象非线程安全这一事实的直接推论。

### 3.2 外部线程操作：MUST 投递（post）到绑定线程

**错误示例：**

```cpp
class Client {
public:
    void connect(const std::string& host, const std::string& port) {
        // 错误：用户线程直接操作 resolver_/socket_，
        // 与 io 线程上正在进行的 async_reconnect 并发 → 未定义行为
        resolver_.async_resolve(host, port, [this](auto ec, auto eps) { ... });
    }
    void enable_timeout() {
        deadline_.expires_after(std::chrono::seconds(5));  // 错误：与 io 线程的定时器操作并发
    }
};
```

**正确示例：**

```cpp
class Client {
public:
    void connect(const std::string& host, const std::string& port) {
        asio::post(executor_, [self = shared_from_this(), host, port] {
            self->do_connect(host, port);     // 实际工作在绑定 io 线程执行
        });
    }
private:
    void do_connect(const std::string& host, const std::string& port) {
        resolver_.async_resolve(host, port, ...);   // 只在 io 线程调用
    }
};
```

规则：**用户线程调用任何公共 API，要么内部 post，要么文档明确声明"必须由 io 线程调用"并断言。**

### 3.3 跨 io_context 访问：MUST 禁止

两个对象绑定了不同的 io_context，互相之间**不得直接读写对方成员**（哪怕只是读一个 bool）。必须通过对方提供的、内部 post 到其绑定线程的接口。

### 3.4 同一 socket 的写：MUST 串行化（写队列）

asio 不允许对同一 socket 并发发起多个写操作。所有待写数据进队列，任何时刻至多一个 `async_write` 在途，由完成回调驱动下一个。

**错误示例：**

```cpp
void send(const void* data, std::size_t len) {
    // 错误：每次调用直接发起 async_write；两次 send 并发 → 并发写同一 socket → UB
    auto buf = std::make_shared<std::string>(static_cast<const char*>(data), len);
    asio::async_write(socket_, asio::buffer(*buf), [buf](auto, auto) {});
}
```

**正确示例：**

```cpp
std::deque<std::shared_ptr<std::string>> write_queue_;
bool writing_ = false;

void send(const void* data, std::size_t len) {
    write_queue_.push_back(std::make_shared<std::string>(static_cast<const char*>(data), len));
    if (!writing_) do_write();               // 无在途写才发起
}

void do_write() {
    if (write_queue_.empty()) { writing_ = false; return; }
    writing_ = true;
    auto& cur = write_queue_.front();
    asio::async_write(socket_, asio::buffer(*cur->data(), cur->size()),
        [self = shared_from_this()](std::error_code ec, std::size_t) {
            if (ec) { self->close(); return; }
            self->write_queue_.pop_front();  // 只在完成回调里 pop
            self->do_write();
        });
}
```

规则：**pop 只能在写完成回调中执行；发起前必须检查是否有在途写。**

### 3.5 读循环：MUST 单一接收者

同一 socket 同一时刻只能有一个读循环。切换读取方式（如 P2P 成功后从 TCP 切到 UDP）时，必须先取消旧接收者再启动新接收者。

### 3.6 共享资源轮询：MUST 线程安全

被多线程调用的资源分配函数（如 io_context 池的 round-robin 分发）内部计数器必须原子化。

---

## 4. 资源释放规范（三层模型）

资源释放 = **显式入口**（任何线程可调，只投递）→ **关闭序列**（io 线程执行，固定顺序）→ **析构兜底**（幂等调用入口）。三层职责分离，缺一层就不完整：只有入口没有序列 = 清理分散且跨线程；只有序列没有析构兜底 = 忘记释放就泄漏/悬垂。

### 4.1 三层模型总览

| 层 | 职责 | 执行线程 | 幂等机制 |
|---|---|---|---|
| 第一层：`release_obj()` 入口 | CAS 守卫 + 投递，**不碰任何资源** | 任意线程 | `reference_.release()` CAS |
| 第二层：`do_close_sequence()` | 按固定顺序清理全部资源 | 仅绑定 io 线程 | 生命周期 CAS（try_close） |
| 第三层：析构兜底 | 幂等调用入口，防漏释放 | 任意线程（析构者） | 入口 CAS 已释放则 no-op |

### 4.2 第一层：显式入口 release_obj —— MUST 只投递，不做事

**错误示例：**

```cpp
void destroy() {
    state_ = State::Stopped;        // 错误：用户线程直接改状态
    timer_.cancel();                // 与 io 线程的定时器回调并发
    pending_.clear();               // 与 io 线程的队列操作并发
    socket_.close();                // 与 io 线程的异步操作并发
}
```

**正确示例：**

```cpp
void release_obj() {
    if (!reference_.release()) return;          // ① CAS 幂等：重复调用直接返回

    network::safe_post(executor_, weak_from_this(), [](const std::shared_ptr<Self>& self) {
        self->do_close_sequence();              // ② 清理全部在 io 线程的关闭序列里
    });
}

void do_close_sequence() {                      // 只在绑定 io 线程执行
    if (!try_close(life_state_)) return;        // CAS：started/starting → stopping（幂等）
    cancel_timers();
    network::fail_pending(read_reqs_,  [ec](auto& r) { r.complete_func(0, ec); });
    network::fail_pending(write_reqs_, [ec](auto& r) { r.complete_func(0, ec); });
    manager_->remove(this);
    if (try_mark_closed(life_state_)) {
        if (closed_cb_) closed_cb_(ec);         // 回调在状态迁移之后
    }
    socket_.close();
    user_data_.reset();                         // 断开循环引用
}
```

### 4.3 第二层：关闭序列 —— MUST 按固定顺序执行

任何连接对象的关闭序列必须按以下顺序，整条序列在绑定 io 线程执行、全程持有强引用：

```
1. CAS 迁移状态（Started/Starting → Stopping），失败则直接返回（幂等）
2. 取消所有定时器
3. 以错误码回掉所有 pending 请求（读/写）
4. 从管理器/容器移除（session 表、连接表）
5. CAS 迁移到最终态（Stopping → Stopped）
6. 触发用户回调（此时状态已确定）
7. 释放资源（socket、buffer、用户数据；断开循环引用）
```

附加约束：

- **容器移除 MUST 先于用户回调**：否则回调里遍历容器会看到幽灵对象，且同线程遍历删除会死锁。
- **重复关闭 MUST 无副作用**：关闭函数可能从多个路径进入（用户手动、错误回调、超时、容器清理）。入口 CAS 是唯一守卫，任何绕过 CAS 的清理路径都是错误。
- **用户回调内 MUST 禁止同步等待外部线程**（如 join、阻塞在 future/lock 上）：回调在 io 线程执行，若等待调用线程（例如析构线程正阻塞在 `post_and_wait` 上），将形成 A 等 B、B 等 A 的死锁。

### 4.4 第三层：析构兜底 —— MUST 幂等调用 release_obj

析构函数不做同步清理（禁止 cancel/close——析构可能在随机线程执行），但必须兜底调用入口：

```cpp
~MyObject() {
    release_obj();      // 正常路径（已显式释放）是 no-op；漏释放时兜底
    // 建议调试断言：life_state_ == connection_state::closed
}
```

析构期间的特殊性：`weak_from_this().lock()` 必然失败（引用计数已归零），无法再用 `safe_post` 保活投递。此时使用阻塞式投递 `post_and_wait`——它在 io 线程执行清理，且**阻塞等待执行完成**；由于析构函数尚未返回，对象及其成员在等待期间仍然有效，裸 `this` 安全：

```cpp
~MyObject() {
    if (!reference_.release()) return;   // 已释放 → no-op
    network::post_and_wait(executor_, [this] { this->do_close_sequence(); });
}
```

### 4.5 post_and_wait 死锁约定（MUST）

`post_and_wait` 是阻塞等待 io 线程的调用，本质存在"A 等 B、B 等 A"的风险，必须遵守三条约定：

1. **MUST 不得持有 io 线程可能需要获取的锁时调用**（io 线程执行 fn 时等锁 → 与等待方互等）
2. **MUST 保证 fn（及其中触发的用户回调）不反向等待调用线程**（不 join、不阻塞等待调用线程持有的资源）
3. **MUST 只在非 io 线程调用**（同 executor 内 dispatch 已防住自我死锁；跨 executor 的阻塞等待仍需遵守前两条）

满足约定后，`post_and_wait` 的两种路径均安全：已在 io 线程 → dispatch 立即执行（不死锁）；其他线程 → 投递并阻塞等待完成。

---

## 5. 缓冲所有权规范

### 5.1 传给异步操作的 buffer：MUST 存活到完成回调

异步操作期间的 buffer 所有权由发起方保证。写队列持有 `shared_ptr<string>` 是最低要求；禁止在完成回调之前复用或释放 buffer。

**错误示例：**

```cpp
std::string body_;
void write_body() {
    asio::async_write(socket_, asio::buffer(body_),
        [this](auto ec, auto n) { ... });
    body_.clear();                    // 错误：async_write 还在读取 body_
}
```

### 5.2 用户提供的读缓冲：MUST 在错误路径也回掉

异步读使用用户提供的 buffer 时，无论成功、失败还是对象关闭，都必须调用完成回调（失败回调带错误码）。未回掉的 pending 读会让用户内存泄漏、逻辑悬挂。

### 5.3 `string_view`/指针引用：MUST 不越过 buffer 生命周期

不得在回调之外保存指向内部 buffer 的 `string_view` 或裸指针；需要长期持有数据必须复制。

---

## 6. 定时器规范

### 6.1 定时器回调：MUST 捕获强引用（见 1.3）

### 6.2 定时器：MUST 在对象关闭时全部取消

对象持有成员定时器时，关闭序列必须显式取消全部定时器并等待其回调结束（或确认回调持有强引用且安全）。未取消的定时器会：① 持有对象强引用造成泄漏；② 在对象销毁后触发回调造成 UAF。

### 6.3 定时器回调：MUST 只在其绑定 io 线程执行

定时器必须与对象绑定同一 io_context。禁止把一个 io_context 的定时器回调里直接操作另一个 io_context 的对象成员。

---

## 7. 错误处理规范

### 7.1 io 线程：MUST 不抛出未捕获异常

io 线程的异常会终止整个事件循环（进而崩溃进程）。防御要点：对外部输入的长度/数量做上限校验（如报文长度、容器 reserve 大小），禁止让 `bad_alloc`/`length_error` 抛入 io 线程。

**错误示例：**

```cpp
void on_frame(const std::string& header) {
    std::uint32_t len = decode(header);
    payload_.resize(len);              // 错误：len 来自网络，无上限 → 远程触发 bad_alloc → io 线程崩溃
}
```

**正确示例：**

```cpp
static constexpr std::size_t kMaxPayload = 16 * 1024 * 1024;
void on_frame(const std::string& header) {
    std::uint32_t len = decode(header);
    if (len == 0 || len > kMaxPayload) { close(); return; }   // 先校验
    payload_.resize(len);
}
```

### 7.2 错误路径：MUST 收敛到统一清理，不留半死状态

任何错误分支（读失败、写失败、解析失败、超时）最终必须走向统一的关闭序列。禁止"只记日志不处理"——残留的悬空操作会悄悄积累资源或产生幽灵状态。

### 7.3 读循环停止：MUST 是显式决策

读循环的每次重挂都必须明确：要么重挂（继续），要么进入关闭序列（结束）。禁止"出错后既不重挂也不关闭"的路径——那会造成连接假死。

---

## 8. 公共工具函数

本规范的实现支撑位于 `src/network/async_utils.hpp`（header-only）。网络代码必须优先使用这些工具，禁止手写重复的错误模式。

### 8.1 生命周期状态机（实现 §2）

```cpp
enum class connection_state { closed, starting, started, stopping };
const char* to_string(connection_state s);

template <typename State>
bool try_transition(std::atomic<State>& state, State expected, State desired); // CAS 迁移，失败返回 false

bool try_start(std::atomic<connection_state>& state);        // closed → starting
bool try_mark_started(std::atomic<connection_state>& state); // starting → started
bool try_close(std::atomic<connection_state>& state);        // started/starting → stopping（幂等）
bool try_mark_closed(std::atomic<connection_state>& state);  // stopping → closed
```

用法（任何线程可调用的关闭入口）：

```cpp
void close() {
    if (!try_close(life_state_)) return;   // 已在关闭/已关闭 → 幂等返回
    asio::post(executor_, [self = shared_from_this()] { self->do_close_sequence(); });
}
```

### 8.2 线程亲和性投递（实现 §1.2、§3.2）

```cpp
// weak 提升成功才执行；对象已销毁则任务静默跳过（不会 UAF）
void safe_post(Executor&& executor, const std::weak_ptr<T>& weak, Fn&& fn);
void safe_dispatch(Executor&& executor, const std::weak_ptr<T>& weak, Fn&& fn);
// 调用方已持有强引用，任务执行期间保活对象
void post_keepalive(Executor&& executor, const std::shared_ptr<T>& strong, Fn&& fn);
void dispatch_keepalive(Executor&& executor, const std::shared_ptr<T>& strong, Fn&& fn);
// 阻塞式投递：io 线程执行 fn 并等待完成（析构兜底用，见 §4.4、§4.5）
void post_and_wait(Executor&& executor, Fn&& fn);
// executor 参数支持 asio::any_io_executor 或 io_context；回调签名 void(const std::shared_ptr<T>&)
// （post_and_wait 的 fn 无参数，捕获裸 this——调用期间对象必须存活，由阻塞等待保证）
```

用法：

```cpp
// 用户线程调用（weak 哨兵 + 全程强引用的规范写法）
void request_close() {
    network::safe_post(executor_, weak_from_this(), [](const std::shared_ptr<Client>& self) {
        self->do_close_sequence();
    });
}

// io 线程内部继续处理（对象必然存活）
network::dispatch_keepalive(executor_, shared_from_this(), [](const std::shared_ptr<Client>& self) {
    self->flush();
});

// 析构兜底（§4.4）：weak 提升必然失败，用阻塞式投递 + 裸 this
~Client() {
    if (!reference_.release()) return;
    network::post_and_wait(executor_, [this] { this->do_close_sequence(); });
}
```

`post_and_wait` 死锁约定见 §4.5（三条 MUST）。

### 8.3 串行化写队列（实现 §3.4）

```cpp
class serialized_writer : private asio::noncopyable {
public:
    // launcher：发起一次写；completion 必须原样交给 asio::async_write 作完成回调
    using write_launcher_t = std::function<void(const std::shared_ptr<std::string>& data,
                                                network_io_handler_t completion)>;
    using error_handler_t  = std::function<void(std::error_code)>;
    using keepalive_t      = std::function<void()>;

    serialized_writer(asio::any_io_executor executor, write_launcher_t launcher, keepalive_t keepalive = {});
    void push(std::shared_ptr<std::string> data);   // 任意线程可调用
    void set_error_handler(error_handler_t handler); // 写失败时触发（应由此进入关闭序列）
    void clear();                                    // 丢弃 pending，关闭序列中调用（io 线程）
    std::size_t pending() const;
    bool writing() const;
    bool empty() const;
};
```

用法（成员变量）：

```cpp
// 构造（keepalive 必须持有所属对象的强引用，保证完成回调执行时 writer 存活）
writer_ = std::make_shared<network::serialized_writer>(
    socket_.get_executor(),
    [self = shared_from_this()](const std::shared_ptr<std::string>& data, network_io_handler_t completion) {
        asio::async_write(self->socket_, asio::buffer(*data), std::move(completion));
    },
    [self = shared_from_this()] { (void)self; });

writer_->set_error_handler([self = shared_from_this()](std::error_code ec) {
    self->close();          // 进入统一关闭序列
});

// 任意线程发送
writer_->push(std::make_shared<std::string>(data, len));

// 关闭序列中
writer_->clear();
```

### 8.4 pending 请求清理（实现 §4.1、§5.2）

```cpp
// 把 pending 容器整体搬出（防止回调内重入修改原容器），再逐项回掉；返回回掉数量
template <typename PendingContainer, typename FailFn>
std::size_t fail_pending(PendingContainer& pending, FailFn&& fail_one);
```

用法（关闭序列中）：

```cpp
network::fail_pending(read_requests_, [ec](auto& req) { req.complete_func(0, ec); });
network::fail_pending(write_requests_, [ec](auto& req) { req.complete_func(0, ec); });
```

---

## 9. io_context_pool 使用规范

实现位于 `src/network/io_context_pool.hpp/cpp`（单例）。提供优雅退出与线程亲和性基础设施。

### 9.1 wait_stop()：阻塞直到 stop 被调用

```cpp
void wait_stop();   // 必须由非 io 线程调用（io 线程内调用会死锁）
```

用法（主线程等待优雅退出完成）：

```cpp
// 子线程或其他模块阻塞等待
std::thread t([] { network::io_context_pool::Instance().wait_stop(); ... });

// 停止路径
network::io_context_pool::Instance().stop();   // 阻塞，直到所有 io_context 排空并停止
```

实现机制（asio2 wait_stop_timer_ 同款）：在 `io_contexts_[0]` 上创建一个"永不自动到期"的定时器（`expires_after(nanoseconds::max)`）并登记进定时器注册表；`stop()` 时取消它唤醒所有阻塞方。定时器只在 io_contexts_[0] 线程上创建/取消，保证线程安全。

### 9.2 定时器注册表：reg_timer / unreg_timer

```cpp
void reg_timer(asio::steady_timer& timer);   // 定时器创建后登记
void unreg_timer(asio::steady_timer& timer); // 定时器析构时注销（必须）
```

契约：**凡是生命周期可能跨越 stop 的定时器，创建后必须 reg_timer，析构时 unreg_timer**。`stop()` 会统一取消所有已登记定时器——解决"stop 后仍有定时器存活导致 io_context 无法退出"的问题。

```cpp
// 成员定时器规范用法
class MyObject {
    asio::steady_timer timer_;
public:
    void start() {
        network::io_context_pool::Instance().reg_timer(timer_);
        timer_.expires_after(...);
        timer_.async_wait(...);
    }
    ~MyObject() { network::io_context_pool::Instance().unreg_timer(timer_); }
};
```

### 9.3 对象注册表：reg_object / unreg_object —— "stop 一定被调用"的保证

```cpp
// key 通常传对象的 this 指针；on_stop 通常是 release_obj 的闭包
void reg_object(const void* key, std::function<void()> on_stop);
void unreg_object(const void* key);   // 对象停止时必须注销（否则池停止时会再次调用）
```

契约与语义：

- 对象启动时注册，停止时注销；`stop()` 会**统一驱动所有已登记对象的 on_stop**——这是"池停 → 所有对象必然被 stop"的自上而下保证（asio2 `io_t::objects_` 同款机制）
- 覆盖范围：**server 动态创建的对象**（accept 出的 session、动态通道）——`make_guard`（exit 信号）只覆盖显式创建的顶层对象，两者互补
- 停止开始后注册的对象会**立即**调用 on_stop（池已停止，对象必须马上释放）
- on_stop 通常是 release_obj（内部 post，任意线程安全调用）

```cpp
// 对象注册规范用法
class MySession {
public:
    void start() {
        network::io_context_pool::Instance().reg_object(
            this, [self = shared_from_this()] { self->release_obj(); });
    }
    void stop() {   // 关闭序列内调用
        network::io_context_pool::Instance().unreg_object(this);
        ...
    }
    ~MySession() { network::io_context_pool::Instance().unreg_object(this); }
};
```

### 9.4 running_in_io_thread()：线程亲和性断言

```cpp
bool running_in_io_thread() const;   // 当前线程是否是任一 io 线程
```

用法：公共操作入口断言（调试期暴露跨线程调用）：

```cpp
void do_close_sequence() {
    FASSERT(network::io_context_pool::Instance().running_in_io_thread());
    ...
}
```

### 9.5 stop() 的语义（MUST 知晓）

- **幂等**：重复调用无副作用（内部 CAS 守卫）
- **阻塞**：等待所有 io_context 排空并停止后才返回（优雅退出）
- **序列**：唤醒 wait_stop → 驱动已登记对象释放（9.3）→ 取消已登记定时器 → 释放 work guard → 阻塞等待全部 io_context stopped
- **顺序约束**：驱动对象释放**必须先于**等待 io 排空——否则对象的关闭序列尚未入队，排空时对象还挂着
- 依赖：所有跨 stop 存活的定时器必须已登记（9.2），否则可能无法排空

### 9.6 get_io_context()：线程安全

round-robin 分发，内部计数器为原子变量，任意线程可并发调用。

---

## 10. 代码评审检查清单

提交网络代码前逐条自检：

**生命周期**
- [ ] 对象由 make_shared 创建，继承 enable_shared_from_this
- [ ] 所有异步回调捕获强引用，无裸 this
- [ ] 无"入口 weak 哨兵 + 全程裸 this"模式
- [ ] 成员持有 self 的引用链在关闭时显式断开

**状态**
- [ ] 生命周期状态是 atomic，迁移用 CAS
- [ ] close/start 幂等（重复调用无副作用）
- [ ] 用户回调在状态迁移之后触发
- [ ] 握手/协议状态与生命周期状态分离

**线程**
- [ ] 对象成员只在绑定 io 线程访问（有断言或文档约定）
- [ ] 用户线程调用全部 post，无直接操作 asio 对象
- [ ] 同一 socket 至多一个在途写，pop 只在完成回调
- [ ] 同一 socket 单一读循环，切换前先取消旧者

**关闭**
- [ ] 三层模型齐全：入口（release_obj 只投递）+ 关闭序列（io 线程固定顺序）+ 析构兜底（幂等调用入口）
- [ ] 关闭序列按 §4.3 的顺序执行，全程持有强引用
- [ ] 用户线程的 destroy 只投递
- [ ] 容器移除先于用户回调
- [ ] 所有错误路径收敛到关闭序列
- [ ] 析构调 release_obj 兜底（幂等），且析构内无同步清理
- [ ] post_and_wait 遵守三条死锁约定（§4.5：不持锁、回调不反向等待、非 io 线程）

**缓冲与定时器**
- [ ] 异步操作期间 buffer 存活（shared_ptr 持有）
- [ ] 用户缓冲在错误路径也回掉
- [ ] 所有定时器在关闭时取消，回调捕获强引用

**池与注册**
- [ ] 跨 stop 存活的定时器已 reg_timer/unreg_timer（§9.2）
- [ ] 动态创建的对象已 reg_object/unreg_object，保证池停必释放（§9.3）
- [ ] 顶层对象用 make_guard 或显式 release 覆盖（§9.3）

---
