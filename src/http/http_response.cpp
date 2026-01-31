#include "http_response.hpp"
#include "http_connection.h"
#include <string_view>

#include "fundamental/basic/log.h"
#include "fundamental/basic/utils.hpp"
namespace network::http
{

namespace StatusStrings
{

static constexpr std::string_view ok                    = "HTTP/1.0 200 OK\r\n";
static constexpr std::string_view created               = "HTTP/1.0 201 Created\r\n";
static constexpr std::string_view accepted              = "HTTP/1.0 202 Accepted\r\n";
static constexpr std::string_view no_content            = "HTTP/1.0 204 No Content\r\n";
static constexpr std::string_view partial_content       = "HTTP/1.1 206 Partial Content\r\n";
static constexpr std::string_view multiple_choices      = "HTTP/1.0 300 Multiple Choices\r\n";
static constexpr std::string_view moved_permanently     = "HTTP/1.0 301 Moved Permanently\r\n";
static constexpr std::string_view moved_temporarily     = "HTTP/1.0 302 Moved Temporarily\r\n";
static constexpr std::string_view not_modified          = "HTTP/1.0 304 Not Modified\r\n";
static constexpr std::string_view bad_request           = "HTTP/1.0 400 Bad Request\r\n";
static constexpr std::string_view unauthorized          = "HTTP/1.0 401 Unauthorized\r\n";
static constexpr std::string_view forbidden             = "HTTP/1.0 403 Forbidden\r\n";
static constexpr std::string_view not_found             = "HTTP/1.0 404 Not Found\r\n";
static constexpr std::string_view internal_server_error = "HTTP/1.0 500 Internal Server Error\r\n";
static constexpr std::string_view not_implemented       = "HTTP/1.0 501 Not Implemented\r\n";
static constexpr std::string_view bad_gateway           = "HTTP/1.0 502 Bad Gateway\r\n";
static constexpr std::string_view service_unavailable   = "HTTP/1.0 503 Service Unavailable\r\n";
static constexpr std::string_view name_value_separator  = ": ";
static constexpr std::string_view crlf                  = "\r\n";
constexpr decltype(auto) status_to_buffer(http_response::response_type status) {
    switch (status) {
    case http_response::ok: return ok;
    case http_response::created: return created;
    case http_response::accepted: return accepted;
    case http_response::no_content: return no_content;
    case http_response::partial_content: return partial_content;
    case http_response::multiple_choices: return multiple_choices;
    case http_response::moved_permanently: return moved_permanently;
    case http_response::moved_temporarily: return moved_temporarily;
    case http_response::not_modified: return not_modified;
    case http_response::bad_request: return bad_request;
    case http_response::unauthorized: return unauthorized;
    case http_response::forbidden: return forbidden;
    case http_response::not_found: return not_found;
    case http_response::internal_server_error: return internal_server_error;
    case http_response::not_implemented: return not_implemented;
    case http_response::bad_gateway: return bad_gateway;
    case http_response::service_unavailable: return service_unavailable;
    default: return internal_server_error;
    }
}

} // namespace StatusStrings

namespace StockReplies
{

static constexpr std::string_view ok                    = "";
static constexpr std::string_view created               = "<html>"
                                                          "<head><title>Created</title></head>"
                                                          "<body><h1>201 Created</h1></body>"
                                                          "</html>";
static constexpr std::string_view accepted              = "<html>"
                                                          "<head><title>Accepted</title></head>"
                                                          "<body><h1>202 Accepted</h1></body>"
                                                          "</html>";
static constexpr std::string_view no_content            = "<html>"
                                                          "<head><title>No Content</title></head>"
                                                          "<body><h1>204 Content</h1></body>"
                                                          "</html>";
static constexpr std::string_view multiple_choices      = "<html>"
                                                          "<head><title>Multiple Choices</title></head>"
                                                          "<body><h1>300 Multiple Choices</h1></body>"
                                                          "</html>";
static constexpr std::string_view moved_permanently     = "<html>"
                                                          "<head><title>Moved Permanently</title></head>"
                                                          "<body><h1>301 Moved Permanently</h1></body>"
                                                          "</html>";
static constexpr std::string_view moved_temporarily     = "<html>"
                                                          "<head><title>Moved Temporarily</title></head>"
                                                          "<body><h1>302 Moved Temporarily</h1></body>"
                                                          "</html>";
static constexpr std::string_view not_modified          = "<html>"
                                                          "<head><title>Not Modified</title></head>"
                                                          "<body><h1>304 Not Modified</h1></body>"
                                                          "</html>";
static constexpr std::string_view bad_request           = "<html>"
                                                          "<head><title>Bad Request</title></head>"
                                                          "<body><h1>400 Bad Request</h1></body>"
                                                          "</html>";
static constexpr std::string_view unauthorized          = "<html>"
                                                          "<head><title>Unauthorized</title></head>"
                                                          "<body><h1>401 Unauthorized</h1></body>"
                                                          "</html>";
static constexpr std::string_view forbidden             = "<html>"
                                                          "<head><title>Forbidden</title></head>"
                                                          "<body><h1>403 Forbidden</h1></body>"
                                                          "</html>";
static constexpr std::string_view not_found             = "<html>"
                                                          "<head><title>Not Found</title></head>"
                                                          "<body><h1>404 Not Found</h1></body>"
                                                          "</html>";
static constexpr std::string_view internal_server_error = "<html>"
                                                          "<head><title>Internal Server Error</title></head>"
                                                          "<body><h1>500 Internal Server Error</h1></body>"
                                                          "</html>";
static constexpr std::string_view not_implemented       = "<html>"
                                                          "<head><title>Not Implemented</title></head>"
                                                          "<body><h1>501 Not Implemented</h1></body>"
                                                          "</html>";
static constexpr std::string_view bad_gateway           = "<html>"
                                                          "<head><title>Bad Gateway</title></head>"
                                                          "<body><h1>502 Bad Gateway</h1></body>"
                                                          "</html>";
static constexpr std::string_view service_unavailable   = "<html>"
                                                          "<head><title>Service Unavailable</title></head>"
                                                          "<body><h1>503 Service Unavailable</h1></body>"
                                                          "</html>";

decltype(auto) toString(http_response::response_type status) {
    switch (status) {
    case http_response::ok: return ok;
    case http_response::created: return created;
    case http_response::accepted: return accepted;
    case http_response::no_content: return no_content;
    case http_response::multiple_choices: return multiple_choices;
    case http_response::moved_permanently: return moved_permanently;
    case http_response::moved_temporarily: return moved_temporarily;
    case http_response::not_modified: return not_modified;
    case http_response::bad_request: return bad_request;
    case http_response::unauthorized: return unauthorized;
    case http_response::forbidden: return forbidden;
    case http_response::not_found: return not_found;
    case http_response::internal_server_error: return internal_server_error;
    case http_response::not_implemented: return not_implemented;
    case http_response::bad_gateway: return bad_gateway;
    case http_response::service_unavailable: return service_unavailable;
    default: return internal_server_error;
    }
}

} // namespace StockReplies

http_response::http_response(http_connection& http_con_ref) : http_con_ref(http_con_ref) {
}

void http_response::write_headeres() {
    response_pack_status |= http_response_status_mask::http_response_headeres_packed;
    std::vector<network_write_buffer_t> buffers;
    buffers.push_back(asio::buffer(StatusStrings::status_to_buffer(status_)));
    for (std::size_t i = 0; i < headers.size(); ++i) {
        http_header& h = headers[i];
        buffers.push_back(::asio::buffer(h.name));
        buffers.push_back(::asio::buffer(StatusStrings::name_value_separator));
        buffers.push_back(::asio::buffer(h.value));
        buffers.push_back(::asio::buffer(StatusStrings::crlf));
    }
    buffers.push_back(::asio::buffer(StatusStrings::crlf));
    http_con_ref.async_buffers_write(
        std::move(buffers), [this, ptr = http_con_ref.shared_from_this()](asio::error_code ec, std::size_t length) {
            if (!ptr->reference_.is_valid()) {
                return;
            }
            if (ec) {
                FDEBUG("http write headeres failed {}", ec.message());
                http_con_ref.release_obj();
                return;
            }
            response_pack_status |= http_response_status_mask::http_response_headeres_send_finished;
            perform_response(true);
        });
}

void http_response::prepare() {
    status_ = http_response::ok;
    headers.resize(2);
    headers[0].name  = "Content-Length";
    headers[1].name  = "Content-Type";
    headers[1].value = ExtensionToType("json");
}

bool http_response::can_set_header() const {
    FASSERT_ACTION(!(response_pack_status.load() & http_response_status_mask::http_response_all_headeres_set),
                   return false, "all http headeres has already set,check your code");
    return true;
}

bool http_response::can_set_body() const {
    FASSERT_ACTION(!(response_pack_status.load() & http_response_status_mask::http_response_body_set), return false,
                   "body has set finished ,check your code");
    return true;
}

void http_response::stock_response(http_response::response_type status) {

    status_ = status;

    auto content           = StockReplies::toString(status_);
    auto& new_item         = data_storage_.emplace_back();
    new_item.type          = data_type::ref_data;
    new_item.p_ref         = content.data();
    new_item.ref_size      = content.size();
    new_item.is_last_chunk = true;
    current_body_size      = new_item.ref_size;
    data_pending_size      = current_body_size;
    max_body_size          = new_item.ref_size;
    headers.resize(2);
    headers[0].name  = "Content-Length";
    headers[0].value = std::to_string(content.size());
    headers[1].name  = "Content-Type";
    headers[1].value = "text/html";
    response_pack_status |= http_response_status_mask::http_response_finish_set;
}

void http_response::set_status(response_type status) {

    if (!can_set_header()) return;
    status_ = status;
}

void http_response::set_content_type(const std::string& extension) {
    if (!can_set_header()) return;
    headers[1].value = ExtensionToType(extension);
}

void http_response::set_raw_content_type(const std::string& typeValue) {

    if (!can_set_header()) return;
    headers[1].value = typeValue;
}

void http_response::add_header(const std::string& name, const std::string& value) {

    if (!can_set_header()) return;
    auto& header = headers.emplace_back();
    header.name  = name;
    header.value = value;
}

void http_response::set_body_size(std::size_t max_content_length) {
    asio::post(http_con_ref.socket_.get_executor(),
               [this, max_content_length = max_content_length, ref = http_con_ref.shared_from_this()]() mutable {
                   if (!ref->reference_.is_valid()) return;
                   if (!can_set_body()) return;
                   headers[0].value = std::to_string(max_content_length);
                   max_body_size    = max_content_length;
                   response_pack_status |= http_response_status_mask::http_response_body_size_set;
                   response_pack_status |= http_response_status_mask::http_response_all_headeres_set;
               });
}

void http_response::append_body(const void* data, std::size_t size) {
    if (!can_set_body()) return;
    data_item new_item;
    if (size > 0) {
        new_item.type = data_type::vec_data;
        new_item.vec.resize(size);
        std::memcpy(new_item.vec.data(), data, size);
    }
    asio::post(http_con_ref.socket_.get_executor(),
               [this, new_item = std::move(new_item), size = size, ref = http_con_ref.shared_from_this()]() mutable {
                   if (!ref->reference_.is_valid()) return;
                   if (!can_set_body()) return;
                   Fundamental::ScopeGuard guard([this]() { perform_response(); });
                   if (size > 0) {
                       data_storage_.emplace_back() = std::move(new_item);
                       current_body_size += size;
                       data_pending_size += size;
                   }
                   if (current_body_size >= max_body_size) {
                       response_pack_status |= http_response_status_mask::http_response_body_set;
                       if (data_storage_.size() > 0) data_storage_.back().is_last_chunk = true;
                   }
               });
}

void http_response::append_body(const void* ref_data, std::size_t size, std::function<void()> finish_cb) {
    if (!can_set_body()) return;
    data_item new_item;
    if (size > 0) {
        new_item.type      = data_type::ref_data;
        new_item.p_ref     = ref_data;
        new_item.ref_size  = size;
        new_item.finish_cb = finish_cb;
    }
    asio::post(http_con_ref.socket_.get_executor(),
               [this, new_item = std::move(new_item), size = size, ref = http_con_ref.shared_from_this()]() mutable {
                   if (!ref->reference_.is_valid()) return;
                   if (!can_set_body()) return;
                   Fundamental::ScopeGuard guard([this]() { perform_response(); });
                   if (size > 0) {
                       data_storage_.emplace_back() = std::move(new_item);
                       current_body_size += size;
                       data_pending_size += size;
                   }
                   if (current_body_size >= max_body_size) {
                       response_pack_status |= http_response_status_mask::http_response_body_set;
                       if (data_storage_.size() > 0) data_storage_.back().is_last_chunk = true;
                   }
               });
}

void http_response::append_body(std::string&& body) {
    if (!can_set_body()) return;
    data_item new_item;
    auto size = body.size();
    if (body.size() > 0) {
        new_item.type = data_type::str_data;
        new_item.str  = std::move(body);
    }
    asio::post(http_con_ref.socket_.get_executor(),
               [this, new_item = std::move(new_item), size = size, ref = http_con_ref.shared_from_this()]() mutable {
                   if (!ref->reference_.is_valid()) return;
                   if (!can_set_body()) return;
                   Fundamental::ScopeGuard guard([this]() { perform_response(); });

                   if (size > 0) {
                       data_storage_.emplace_back() = std::move(new_item);
                       current_body_size += size;
                       data_pending_size += size;
                   }
                   if (current_body_size >= max_body_size) {
                       response_pack_status |= http_response_status_mask::http_response_body_set;
                       if (data_storage_.size() > 0) data_storage_.back().is_last_chunk = true;
                   }
               });
}

void http_response::append_body(std::vector<std::uint8_t>&& body) {
    if (!can_set_body()) return;
    data_item new_item;
    auto size = body.size();
    if (body.size() > 0) {
        new_item.type = data_type::vec_data;
        new_item.vec  = std::move(body);
    }
    asio::post(http_con_ref.socket_.get_executor(),
               [this, new_item = std::move(new_item), size = size, ref = http_con_ref.shared_from_this()]() mutable {
                   if (!ref->reference_.is_valid()) return;
                   if (!can_set_body()) return;
                   Fundamental::ScopeGuard guard([this]() { perform_response(); });
                   if (size > 0) {
                       data_storage_.emplace_back() = std::move(new_item);
                       current_body_size += size;
                       data_pending_size += size;
                   }
                   if (current_body_size >= max_body_size) {
                       response_pack_status |= http_response_status_mask::http_response_body_set;
                       if (data_storage_.size() > 0) data_storage_.back().is_last_chunk = true;
                   }
               });
}

std::size_t http_response::get_current_body_size() const {

    return current_body_size;
}

std::size_t http_response::get_data_pending_size() const {
    return data_pending_size;
}

http_response::response_type http_response::get_status() const {

    return status_;
}

void http_response::perform_response(bool from_async_cb) {
    asio::post(http_con_ref.socket_.get_executor(), [this, from_async_cb, ref = http_con_ref.shared_from_this()]() {
        auto current_status = response_pack_status.load();
        if (!(current_status & http_response_status_mask::http_response_all_headeres_set)) {
            FDEBUG("post http_connection {:p} ignore when response headeres has not set up", (void*)this);
            return;
        }
        if (current_status & http_response_status_mask::http_response_body_send_finished) {
            FDEBUG("post http_connection {:p} ignore when response has finished send", (void*)this);
            return;
        }
        if (!(current_status & http_response_status_mask::http_response_headeres_packed)) {
            FDEBUG("post http_connection {:p} write all headeres", (void*)this);
            write_headeres();
            return;
        }
        // wait headeres send finished
        if (!(current_status & http_response_status_mask::http_response_headeres_send_finished)) {
            FDEBUG("post http_connection {:p} waiting headeres sent finished", (void*)this);
            return;
        }
        // empty response
        if (max_body_size == 0) {
            ref->release_obj();
            return;
        }
        // handle body data
        if (data_storage_.empty()) {
            FDEBUG("post http_connection {:p} waiting data", (void*)this);
            return;
        }
        if (!(current_status & http_response_status_mask::http_response_body_size_set) ||
            (current_status & http_response_status_mask::http_response_body_send_finished)) {
            FDEBUG("post http_connection {:p} waiting body", (void*)this);
            return;
        }

        // a write body request is progressing
        if (data_storage_.size() > 1 && !from_async_cb) {
            return;
        }
        if (write_buffers_.empty()) {

            auto& item = data_storage_.front();
            switch (item.type) {
            case data_type::vec_data:
                write_buffers_.emplace_back(network_write_buffer_t(item.vec.data(), item.vec.size()));
                break;
            case data_type::str_data: {
                write_buffers_.emplace_back(network_write_buffer_t(item.str.data(), item.str.size()));
            } break;
            case data_type::ref_data: write_buffers_.emplace_back(network_write_buffer_t(item.p_ref, item.ref_size)); break;
            default: {
                FWARN("invalid data item type");
                http_con_ref.release_obj();
                return;
            }
            }
        }
        http_con_ref.async_buffers_write_some(
            std::vector<network_write_buffer_t>(write_buffers_.begin(), write_buffers_.end()),
            [this, ptr = http_con_ref.shared_from_this()](asio::error_code ec, std::size_t length) {
                if (!http_con_ref.reference_.is_valid()) {
                    return;
                }
                if (ec) {
                    FDEBUG("http  write body failed {}", ec.message());
                    ptr->release_obj();
                    return;
                }
                ptr->b_waiting_process_any_data.exchange(false);
                data_pending_size -= length;
                notify_pending_size.Emit(data_pending_size);
                {
                    while (length != 0) {
                        if (write_buffers_.empty()) break;
                        auto current_size = write_buffers_.front().size();
                        if (length >= current_size) {
                            length -= current_size;
                            write_buffers_.pop_front();
                            continue;
                        }
                        write_buffers_.front() = network_write_buffer_t(
                            (std::uint8_t*)write_buffers_.front().data() + length, current_size - length);
                        break;
                    }
                    if (!write_buffers_.empty()) { // write
                        perform_response(true);
                        return;
                    }
                    auto& front = data_storage_.front();

                    if (front.finish_cb) front.finish_cb();
                    bool last_chunk = front.is_last_chunk;
                    data_storage_.pop_front();
                    // write finished
                    if (last_chunk) {
                        FDEBUG("http_connection {:p} has sent all data,disconnect", (void*)this);
                        response_pack_status |= http_response_status_mask::http_response_body_send_finished;
                        ptr->release_obj();
                        return;
                    }
                }
                // try next chunk
                perform_response(true);
            });
    });
}

void http_response::set_bytes_range(std::size_t start, std::size_t end, std::size_t total) {
    set_status(network::http::http_response::partial_content);
    add_header("Accept-Ranges", "bytes");
    add_header("Content-Range", Fundamental::StringFormat("bytes {}-{}/{}", start, end, total));
}

} // namespace network::http