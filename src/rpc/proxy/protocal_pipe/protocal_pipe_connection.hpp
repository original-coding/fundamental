#pragma once
#include "forward_pipe_codec.hpp"
#include "network/network.hpp"
#include "rpc/proxy/rpc_forward_connection.hpp"

namespace network
{
namespace proxy
{
/*
connect -> socks5 handshake->tls handshake -> ws handshake->raw proxy
*/
class protocal_pipe_connection : public rpc_forward_connection {
public:
    using add_route_entry_function =
        std::function<std::tuple<bool, std::string>(std::string route, std::string host, std::string service)>;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<protocal_pipe_connection>(std::forward<Args>(args)...);
    }
    explicit protocal_pipe_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                      rpc_client_forward_config forward_config,
                                      std::string pre_read_data = "");
    void set_add_route_entry_function(const add_route_entry_function& func) {
        add_server_handler = func;
    }

protected:
    void process_protocal() override;
    void HandleConnectSuccess() override;
    void StartProtocal() override;
    void StartForward() override;
    void StartRawForward();
    void read_more_data();
    void start_pipe_proxy_handler();
    bool verify_protocal_request_param(forward::forward_response_context& response_context);
    bool handle_none_forward_phase_protocal(forward::forward_response_context& response_context);
    bool need_forward_phase() const;
    bool need_fallback() const;
    void handle_fallback();
    void process_socks5_proxy();
    void handle_tls();
    void process_pipe_handshake();
    void process_ws_proxy();

protected:
    virtual void do_pipe_proxy();

protected:
    std::vector<std::uint8_t> read_cache;
    rpc_client_forward_config forward_config_;
    forward::forward_request_context request_context;
    bool need_socks5_proxy = false;
    add_route_entry_function add_server_handler;
};
} // namespace proxy
} // namespace network