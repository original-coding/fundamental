#pragma once

#include "io_context_pool.hpp"
#include "use_asio.hpp"
#include <functional>
#include <memory>
#if TARGET_PLATFORM_LINUX
    #include <netinet/tcp.h>
#endif
namespace network
{
static constexpr std::size_t kSslPreReadSize = 3;
struct network_data_reference {
    Fundamental::Signal<void()> notify_release;
    bool is_valid() const {
        return !__has_released;
    }
    operator bool() const {
        return is_valid();
    }
    bool operator!() const {
        return !is_valid();
    }
    bool release() {
        auto expected_value = false;
        if (__has_released.compare_exchange_strong(expected_value, true)) {
            notify_release.Emit();
            return true;
        }
        return false;
    }
    std::atomic_bool __has_released = false;
};

struct network_server_ssl_config {
    std::function<std::string(std::string)> passwd_cb;
    std::string certificate_path;
    std::string private_key_path;
    std::string tmp_dh_path;
    std::string ca_certificate_path;
    bool verify_client = true;
    bool disable_ssl   = false;
    bool enable_no_ssl = true;
};
enum rpc_protocal_enable_mask : std::uint32_t
{
    rpc_protocal_filter_none            = 0,
    rpc_protocal_filter_rpc             = (1 << 0),
    rpc_protocal_filter_socks5          = (1 << 1),
    rpc_protocal_filter_http_ws         = (1 << 2),
    rpc_protocal_filter_pipe_connection = (1 << 3),
    rpc_protocal_filter_all             = std::numeric_limits<std::uint32_t>::max(),
};
struct network_client_ssl_config {
    std::string certificate_path;
    std::string private_key_path;
    std::string ca_certificate_path;
    bool verify_org  = false;
    bool disable_ssl = true;

#ifndef NETWORK_DISABLE_SSL

    std::shared_ptr<asio::ssl::context> ssl_context;
    std::exception_ptr load_exception;
#endif
    void preload() {
#ifndef NETWORK_DISABLE_SSL
        ssl_context = std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv13);
        try {
            if (!ca_certificate_path.empty()) {
                ssl_context->load_verify_file(ca_certificate_path);
            }
            if (!private_key_path.empty()) {
                ssl_context->use_private_key_file(private_key_path, asio::ssl::context::pem);
            }
            if (!certificate_path.empty()) {
                ssl_context->use_certificate_chain_file(certificate_path);
            }
        } catch (...) {
            load_exception = std::current_exception();
        }
#endif
    }
};

struct rpc_client_forward_config {
    //
    network_client_ssl_config ssl_config;
    std::string socks5_proxy_host;
    std::string socks5_proxy_port;
    std::string socks5_username;
    std::string socks5_passwd;
};

struct rpc_server_external_config {
    // Whether to enable transparent proxy mode. When enabled,
    // traffic will be forwarded to the service at transparent_proxy_host:transparent_proxy_port
    bool enable_transparent_proxy   = false;
    std::uint32_t rpc_protocal_mask = rpc_protocal_filter_all;
    // Transparent proxy target host
    std::string transparent_proxy_host;
    // Transparent proxy target port
    std::string transparent_proxy_port;
    rpc_client_forward_config forward_config;
};

template <typename T>
struct auto_network_storage_instance : Fundamental::NonCopyable {
    using HandleType = typename decltype(Fundamental::Application::Instance().exitStarted)::HandleType;
    auto_network_storage_instance(std::shared_ptr<T> ptr) : ref_ptr(std::move(ptr)) {
        handle_ = Fundamental::Application::Instance().exitStarted.Connect([&]() { release(); });
    }
    auto_network_storage_instance() = default;
    ~auto_network_storage_instance() {
        Fundamental::Application::Instance().exitStarted.DisConnect(handle_);
        release();
    }
    auto_network_storage_instance(auto_network_storage_instance&& other) noexcept :
    auto_network_storage_instance(std::move(other.ref_ptr)) {
    }

    auto_network_storage_instance& operator=(auto_network_storage_instance&& other) noexcept {
        release();
        ref_ptr = std::move(other.ref_ptr);
        return *this;
    }
    decltype(auto) get() {
        return ref_ptr;
    }

    decltype(auto) operator->() {
        return ref_ptr.get();
    }
    void release() {
        if (ref_ptr) ref_ptr->release_obj();
        ref_ptr = nullptr;
    }

private:
    std::shared_ptr<T> ref_ptr;
    HandleType handle_;
};

template <typename ClassType, typename... Args>
inline decltype(auto) make_guard(Args&&... args) {
    return auto_network_storage_instance(ClassType::make_shared(std::forward<Args>(args)...));
}

struct protocal_helper {
    static asio::ip::tcp::endpoint make_endpoint(std::uint16_t port) {
#ifndef NETWORK_IPV4_ONLY
        return asio::ip::tcp::endpoint(asio::ip::tcp::v6(), port);
#else
        return asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);
#endif
    }
    static void init_acceptor(asio::ip::tcp::acceptor& acceptor, std::uint16_t port) {
        auto end_point = make_endpoint(port);
        acceptor.open(end_point.protocol());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
#ifndef NETWORK_IPV4_ONLY
        asio::ip::v6_only v6_option(false);
        acceptor.set_option(v6_option);
#endif
        acceptor.bind(end_point);
        acceptor.listen();
    }
};
// ss -to | grep -i keepalive 
inline void enable_tcp_keep_alive(asio::ip::tcp::socket& socket,
                                  bool enable               = true,
                                  std::int32_t idle_sec     = 30,
                                  std::int32_t interval_sec = 5,
                                  std::int32_t max_probes   = 3) {
    asio::error_code ec;
    socket.set_option(asio::socket_base::keep_alive(enable), ec);
#if TARGET_PLATFORM_LINUX
    if (enable) {
        setsockopt(socket.native_handle(), SOL_TCP, TCP_KEEPIDLE, &idle_sec, sizeof(idle_sec));
        setsockopt(socket.native_handle(), SOL_TCP, TCP_KEEPINTVL, &interval_sec, sizeof(interval_sec));
        setsockopt(socket.native_handle(), SOL_TCP, TCP_KEEPCNT, &max_probes, sizeof(max_probes));
    }
#endif
}
} // namespace network
