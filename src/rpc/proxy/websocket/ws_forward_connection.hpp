#pragma once
#include "fundamental/basic/utils.hpp"
#include "rpc/proxy/proxy_defines.h"
#include "rpc/proxy/rpc_forward_connection.hpp"
#include "ws_common.hpp"

namespace network
{
namespace proxy
{
class websocket_forward_connection : public rpc_forward_connection {
public:
    using route_query_function =
        std::function<std::tuple<bool, network::proxy::ProxyHost, Fundamental::ScopeGuard>(std::string)>;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<websocket_forward_connection>(std::forward<Args>(args)...);
    }
    explicit websocket_forward_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                          route_query_function query_func,
                                          std::string pre_read_data = "");
protected:
    void StartForward() override;
    void StartProtocal() override;
    void HandleConnectSuccess() override;
    void process_protocal() override;

protected:
    void read_more_data();
    void start_ws_proxy();
    void start_ws_proxy_to_next_layer();
    void do_pipe_proxy();

protected:
    route_query_function route_query_f;
    Fundamental::ScopeGuard release_gurad;
    websocket::http_handler_context parse_context;
    websocket::http_handler_context response_context;
    std::string request_uri;
    std::size_t forward_depth = 0;
    //
    network::proxy::proxy_host_type host_type = network::proxy::proxy_host_type::raw_tcp_proxy;
};
} // namespace proxy
} // namespace network