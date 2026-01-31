#pragma once
#include <cstdint>
#include <exception>
#include <memory>
#include <system_error>

#include "fundamental/basic/buffer.hpp"
#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
#include "fundamental/events/event_system.h"
#include "network/network.hpp"

namespace network
{
namespace rpc_service
{
enum class result_code : std::int16_t
{
    OK   = 0,
    FAIL = 1,
};

enum class request_type : uint8_t
{
    rpc_req,
    rpc_subscribe,
    rpc_unsubscribe,
    rpc_publish,
    rpc_heartbeat,
    rpc_res,
    rpc_stream
};

enum class rpc_stream_data_status : std::uint8_t
{
    rpc_stream_none,       // init status
    rpc_stream_heartbeat,  // size=0
    rpc_stream_data,       // size+data
    rpc_stream_write_done, // size=0
    rpc_stream_finish,
    rpc_stream_failed,
    rpc_stream_status_max,
};

struct message_type {
    std::uint64_t req_id  = 0;
    request_type req_type = request_type::rpc_req;
    std::string content;
};

struct rpc_stream_packet {
    std::uint32_t size = 0;
    std::uint8_t type  = 0;
    std::vector<std::uint8_t> data;
};

constexpr std::uint8_t RPC_MAGIC_NUM = 39;
struct rpc_header {
    uint8_t magic         = RPC_MAGIC_NUM;
    request_type req_type = request_type::rpc_req;
    uint32_t body_len     = 0;
    uint64_t req_id       = 0;
    uint32_t func_id      = 0;
    void Serialize(void* dst, std::size_t len) {
        FASSERT(len >= HeadLen());
        Fundamental::BufferWriter writer;
        writer.SetBuffer((std::uint8_t*)dst, len);
        writer.WriteValue(&magic);
        writer.WriteEnum(req_type);
        writer.WriteValue(&body_len);
        writer.WriteValue(&req_id);
        writer.WriteValue(&func_id);
    }

    void DeSerialize(const void* src, std::size_t len) {
        FASSERT(len >= HeadLen());
        Fundamental::BufferReader reader;
        reader.SetBuffer((const std::uint8_t*)src, len);
        reader.ReadValue(&magic);
        reader.ReadValue(&req_type);
        reader.ReadValue(&body_len);
        reader.ReadValue(&req_id);
        reader.ReadValue(&func_id);
    }
    static constexpr std::size_t HeadLen() {
        return sizeof(std::uint8_t) + sizeof(request_type) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
               sizeof(std::uint32_t);
    }
};

static constexpr std::size_t MAX_BUF_LEN                = 1024LLU * 1024 * 1024 * 2;
static constexpr std::size_t kRpcHeadLen                = rpc_header::HeadLen();
static constexpr std::size_t INIT_BUF_SIZE              = 2 * 1024;
static constexpr std::size_t kDefaultNetworkTimeOutMsec = 10000;
// always mean heartbeat
static constexpr std::size_t kDefaultPoorNetworkCheckInternalMsec = 2000;
namespace error
{
enum class rpc_errors : std::int32_t
{
    rpc_success        = 0,
    rpc_failed         = 1,
    rpc_timeout        = 2,
    rpc_broken_pipe    = 3,
    rpc_pack_failed    = 4,
    rpc_unpack_failed  = 5,
    rpc_internal_error = 6,
    rpc_bad_request    = 7,
    rpc_memory_error   = 8,
    rpc_poor_network   = 9
};

class rpc_category : public std::error_category, public Fundamental::Singleton<rpc_category> {
public:
    const char* name() const noexcept override {
        return "network.rpc";
    }
    std::string message(int value) const override {
        switch (static_cast<rpc_errors>(value)) {
        case rpc_errors::rpc_success: return "rpc success";
        case rpc_errors::rpc_failed: return "rpc failed";
        case rpc_errors::rpc_timeout: return "rpc timeout";
        case rpc_errors::rpc_broken_pipe: return "rpc broken pipe";
        case rpc_errors::rpc_pack_failed: return "rpc pack failed";
        case rpc_errors::rpc_unpack_failed: return "rpc unpack failed";
        case rpc_errors::rpc_bad_request: return "rpc bad request";
        case rpc_errors::rpc_internal_error: return "rpc internal error";
        case rpc_errors::rpc_memory_error: return "rpc memory error";
        case rpc_errors::rpc_poor_network: return "rpc poor network";
        default: return "network.rpc error";
        }
    }
};

inline std::error_code make_error_code(rpc_errors e) {
    return std::error_code(static_cast<int>(e), rpc_category::Instance());
}
} // namespace error
} // namespace rpc_service
} // namespace network