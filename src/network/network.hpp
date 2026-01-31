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
    bool verify_client = false;
    bool disable_ssl   = true;
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
            } else {
                ssl_context->set_default_verify_paths();
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
    // tarffic proxy speed limit bytes per second,0 means no limit
    std::size_t proxy_speed_limit_bytes_per_second = 0;
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
    static asio::ip::udp::endpoint make_udp_endpoint(std::uint16_t port) {
#ifndef NETWORK_IPV4_ONLY
        return asio::ip::udp::endpoint(asio::ip::udp::v6(), port);
#else
        return asio::ip::udp::endpoint(asio::ip::udp::v4(), port);
#endif
    }
    static std::error_code udp_bind_endpoint(asio::ip::udp::socket& udp_socket,
                                             const asio::ip::udp::endpoint& end_point) {

        std::error_code ec;
        if (!udp_socket.is_open()) {
            udp_socket.open(end_point.protocol(), ec);
            if (ec) return ec;
        }
        if (end_point.port() != 0) udp_socket.set_option(asio::ip::udp::socket::reuse_address(true), ec);
        if (ec) return ec;
#ifndef NETWORK_IPV4_ONLY
        std::error_code ignore_ec;
        asio::ip::v6_only v6_option(false);
        udp_socket.set_option(v6_option, ignore_ec);
#endif
        udp_socket.bind(end_point, ec);
        return ec;
    }

    static std::error_code udp_bind_endpoint(asio::ip::udp::socket& udp_socket,
                                             std::uint16_t port,
                                             const std::string& address = "") {
        asio::ip::udp::endpoint end_point;
        if (!address.empty()) {
            std::error_code ec;
            auto bind_address = asio::ip::make_address(address, ec);
            if (ec) return ec;
            end_point = asio::ip::udp::endpoint(bind_address, port);
        } else {
            end_point = make_udp_endpoint(port);
        }
        return udp_bind_endpoint(udp_socket, end_point);
    }

    static void init_acceptor(asio::ip::tcp::acceptor& acceptor, std::uint16_t port, const std::string& address = "") {
        asio::ip::tcp::endpoint end_point;
        if (!address.empty()) {
            auto bind_address = asio::ip::make_address(address);
            end_point         = asio::ip::tcp::endpoint(bind_address, port);
        } else {
            end_point = make_endpoint(port);
        }
        acceptor.open(end_point.protocol());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
#ifndef NETWORK_IPV4_ONLY
        std::error_code ec;
        asio::ip::v6_only v6_option(false);
        acceptor.set_option(v6_option, ec);
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
