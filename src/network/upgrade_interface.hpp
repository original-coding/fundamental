#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace network
{
struct network_upgrade_write_buffer {
    const void* data = nullptr;
    std::size_t len  = 0;
};
struct network_upgrade_read_buffer {
    void* data      = nullptr;
    std::size_t len = 0;
};
using write_buffer_t = std::vector<network_upgrade_write_buffer>;
using read_buffer_t  = std::vector<network_upgrade_read_buffer>;
// code=0 means success
struct network_upgrade_interface : std::enable_shared_from_this<network_upgrade_interface> {
    static constexpr std::int32_t kSuccessOpCode = 0;
    using operation_cb                           = std::function<void(std::error_code ec, const std::string& msg)>;
    using write_cb = std::function<void(write_buffer_t write_buffers, const operation_cb& finish_cb)>;
    using read_cb  = std::function<void(read_buffer_t read_buffers, const operation_cb& finish_cb)>;
    using abort_cb = std::function<void()>;
    virtual ~network_upgrade_interface()         = default;
    virtual const char* interface_name() const = 0;
    virtual void abort_all_operation()         = 0;
    virtual void start()                       = 0;
    virtual void init(read_cb read_cb__, write_cb write_cb__, operation_cb finish_cb__, abort_cb abort_cb__) {
        abort_cb_  = abort_cb__;
        read_cb_   = read_cb__;
        write_cb_  = write_cb__;
        finish_cb_ = finish_cb__;
    }

    abort_cb abort_cb_;
    read_cb read_cb_;
    write_cb write_cb_;
    operation_cb finish_cb_;
};
} // namespace network