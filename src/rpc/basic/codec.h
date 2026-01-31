#ifndef REST_RPC_CODEC_H_
#define REST_RPC_CODEC_H_

#include "fundamental/rttr_handler/binary_packer.h"

#include <vector>
#include "const_vars.h"
namespace network {
namespace rpc_service {

using rpc_buffer_type = std::vector<std::uint8_t>;
struct msgpack_codec {

    template <typename... Args>
    static rpc_buffer_type pack(Args&&... args) {
        rpc_buffer_type buffer;
        Fundamental::io::binary_batch_pack(buffer,std::forward<Args>(args)...);
        return buffer;
    }

    template <typename Arg, typename... Args, typename = typename std::enable_if<std::is_enum<Arg>::value>::type>
    static std::string pack_args_str(Arg arg, Args&&... args) {
        rpc_buffer_type buffer;
        Fundamental::io::binary_batch_pack(buffer,(std::int32_t)arg,std::forward<Args>(args)...);
        return std::string(buffer.data(), buffer.data() + buffer.size());
    }

    template <typename T>
    static T unpack(const void* data, size_t length, std::size_t skip_index = 0) {
        T ret {};
        if (!Fundamental::io::binary_unpack(data, length, ret, true, skip_index)) {
            throw std::invalid_argument("unpack failed: Args not match!");
        }
        return ret;
    }
    template <typename Tuple>
    static Tuple unpack_tuple(const void* data, size_t length, std::size_t skip_index = 0) {
        Tuple ret;
        if (!Fundamental::io::binary_unpack_tuple(data, length, ret, true, skip_index)) {
            throw std::invalid_argument("unpack failed: Args not match!");
        }
        return ret;
    }
};
} // namespace rpc_service
} // namespace network

#endif // REST_RPC_CODEC_H_