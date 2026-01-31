#pragma once
#include "http_definitions.hpp"
#include <deque>
#include <list>
#include <mutex>
#include <vector>

#include "fundamental/events/event_system.h"
#include "network/network.hpp"

namespace network::http
{
class http_connection;
/// response send to remote,owned by a http connection
class http_response {
    friend class http_connection;

public:
    Fundamental::Signal<void(std::size_t)> notify_pending_size;

private:
    enum http_response_status_mask : std::uint32_t
    {
        http_response_status_none      = 0,
        http_response_all_headeres_set = (1 << 0),
        http_response_body_size_set    = (1 << 1),
        http_response_body_set         = (1 << 2),
        http_response_finish_set =
            http_response_all_headeres_set | http_response_body_size_set | http_response_body_set,
        http_response_headeres_packed        = (1 << 3),
        http_response_headeres_send_finished = (1 << 4),
        http_response_body_send_finished     = (1 << 5),
    };
    enum data_type : std::uint32_t
    {
        vec_data,
        str_data,
        ref_data
    };
    struct data_item {
        std::vector<std::uint8_t> vec;
        std::string str;
        const void* p_ref               = nullptr;
        std::size_t ref_size            = 0;
        std::function<void()> finish_cb = nullptr;
        data_type type                  = data_type::vec_data;
        bool is_last_chunk              = false;
    };

public:
    http_response(http_connection& http_con_ref);
    /// The status of the reply.
    enum response_type
    {
        ok                    = 200,
        created               = 201,
        accepted              = 202,
        no_content            = 204,
        partial_content       = 206,
        multiple_choices      = 300,
        moved_permanently     = 301,
        moved_temporarily     = 302,
        not_modified          = 304,
        bad_request           = 400,
        unauthorized          = 401,
        forbidden             = 403,
        not_found             = 404,
        internal_server_error = 500,
        not_implemented       = 501,
        bad_gateway           = 502,
        service_unavailable   = 503
    };

    /// [thread-unsafe] Get a stock reply.
    void stock_response(response_type status);

    /// [thread-unsafe] Set status code. if not set, default is ok.
    void set_status(response_type status);
    /// [thread-unsafe] Set content-type. if not set, default is 'json'.
    /// [thread-unsafe] It will use ExtensionToType to convert `extension`.
    void set_content_type(const std::string& extension);
    /// [thread-unsafe] Set raw content-type.
    void set_raw_content_type(const std::string& typeValue);
    /// [thread-unsafe] Add header info.
    void add_header(const std::string& name, const std::string& value);

    /// [thread-safe] set body size first
    void set_body_size(std::size_t max_content_length);

    /// [thread-safe]
    void append_body(const void* data, std::size_t size);
    /// [thread-safe]
    void append_body(const void* ref_data, std::size_t size, std::function<void()> finish_cb);
    /// [thread-safe]
    void append_body(std::string&& body);
    /// [thread-safe]
    void append_body(std::vector<std::uint8_t>&& body);

    std::size_t get_current_body_size() const;
    std::size_t get_data_pending_size() const;
    // Get current status
    response_type get_status() const;
    /// [thread-safe] we need from_async_cb finish flag to start body write processing when we finish writing headeres
    /// you shoule call this function your self when you finish fill response asynchronously
    void perform_response(bool from_async_cb = false);
    // utils
    void set_bytes_range(std::size_t start, std::size_t end, std::size_t total);

private:
    void write_headeres();
    void prepare();
    bool can_set_header() const;
    bool can_set_body() const;

private:
    http_connection& http_con_ref;
    std::atomic<std::uint32_t> response_pack_status = http_response_status_mask::http_response_status_none;
    response_type status_                           = response_type::ok;

    /// The headers to be included in the reply.
    std::vector<http_header> headers;

    std::size_t data_pending_size = 0;
    std::size_t current_body_size = 0;
    std::size_t max_body_size     = 0;
    std::deque<data_item> data_storage_;
    std::list<network_write_buffer_t> write_buffers_;
};
} // namespace network::http