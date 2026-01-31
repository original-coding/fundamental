#pragma once
#include "rpc_forward_connection.hpp"
namespace network
{
namespace proxy
{

class transparent_proxy_connection : public rpc_forward_connection {
public:
    using route_query_function = std::function<std::tuple<bool, std::string, std::string>(std::string)>;

public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<transparent_proxy_connection>(std::forward<Args>(args)...);
    }
    explicit transparent_proxy_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                          std::string dst_host,
                                          std::string dst_port,
                                          std::string pre_read_data = "") :
    rpc_forward_connection(ref_connection, std::move(pre_read_data)) {
        proxy_host    = dst_host;
        proxy_service = dst_port;
    }

protected:
    void process_protocal() override {
        StartProtocal();
    }
};
} // namespace proxy
} // namespace network