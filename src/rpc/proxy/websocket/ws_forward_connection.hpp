#pragma once
#include "fundamental/basic/utils.hpp"
#include "rpc/proxy/rpc_forward_connection.hpp"
#include "ws_common.hpp"
namespace network
{
namespace proxy
{
class websocket_forward_connection : public rpc_forward_connection {
public:
    using route_query_function =
        std::function<std::tuple<bool, std::string, std::string, Fundamental::ScopeGuard>(std::string)>;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<websocket_forward_connection>(std::forward<Args>(args)...);
    }
    explicit websocket_forward_connection(std::shared_ptr<rpc_service::connection> ref_connection,
                                          route_query_function query_func,
                                          std::string pre_read_data = "");

protected:
    void process_protocal() override;
    void read_more_data();
    void start_ws_proxy();

protected:
    route_query_function route_query_f;
    Fundamental::ScopeGuard release_gurad;
    websocket::http_handler_context parse_context;
};
} // namespace proxy
} // namespace network