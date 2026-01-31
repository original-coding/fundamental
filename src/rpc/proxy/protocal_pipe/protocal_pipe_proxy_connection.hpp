#pragma once
#include "ws_port_pipe_forward_connection.hpp"

namespace network
{
namespace proxy
{
/*
connect proxy host->[ssl_handle shake]  -> protocal pipe handshake->raw proxy
*/
class protocal_pipe_proxy_connection : public ws_port_pipe_forward_connection {
public:
    template <typename... Args>
    static decltype(auto) make_shared(Args&&... args) {
        return std::make_shared<protocal_pipe_proxy_connection>(std::forward<Args>(args)...);
    }
    explicit protocal_pipe_proxy_connection(std::shared_ptr<proxy_upstream_interface> ref_connection,
                                            std::string local_proxy_host,
                                            std::string local_proxy_port,
                                            std::string remote_host,
                                            std::string remote_port,
                                            std::string remote_api_path);

protected:
    virtual void process_pipe_handshake() override;
};
} // namespace proxy
} // namespace network