#include "http_server.hpp"
#include "http_connection.h"
#include "http_request.hpp"
#include "http_response.hpp"
#include "http_router.hpp"

#include "fundamental/basic/filesystem_utils.hpp"
#include "fundamental/basic/url_utils.hpp"
#include "fundamental/basic/utils.hpp"
#include "fundamental/thread_pool/thread_pool.h"
#include "fundamental/tracker/time_tracker.hpp"

#include <istream>

namespace network::http
{
static std::string s_tag = "http_file_handler";
using TrackerType        = Fundamental::STimeTracker<std::chrono::milliseconds>;
[[maybe_unused]] static void TrackerOutPut(const std::string_view& msg) {
    FDEBUG("{}", msg);
}

const http_handler http_server::s_default_file_handler =
    [](std::shared_ptr<http_connection> conn, http_response& response, http_request& request) {
        auto root_path = conn->get_root_path();
        if (root_path.empty()) {
            response.stock_response(http_response::response_type::forbidden);
            return;
        }
        //     // Decode url to path.
        std::string requestPath = Fundamental::UrlDecode(request.get_uri());
        if (requestPath.empty()) {
            requestPath = "/";
        } else {
            // Request path must be absolute and not contain "..".
            if (requestPath[0] != '/' || requestPath.find("..") != std::string::npos) {
                response.stock_response(http_response::response_type::bad_request);
                return;
            }
        }
        // If path ends in slash (i.e. is a directory) then add "index.html".
        if (requestPath[requestPath.size() - 1] == '/') {
            requestPath += "index.html";
        }
        std::string full_path = root_path + requestPath;
        auto method           = request.get_method();
        auto& pool            = Fundamental::ThreadPool::LongTimePool();
        switch (method) {
        case MethodFilter::HttpGet: {
            // Determine the file extension.
            std::size_t last_slash_pos = requestPath.find_last_of("/");
            std::size_t last_dot_pos   = requestPath.find_last_of(".");
            std::string extension;
            if (last_dot_pos != std::string::npos && last_dot_pos > last_slash_pos) {
                extension = requestPath.substr(last_dot_pos + 1);
            }

            pool.Enqueue([conn, full_path, extension]() {
                Fundamental::Utils::SetThreadName("http_file_get");
                // Open the file to send back.
                auto& request  = conn->get_request();
                auto& response = conn->get_response();
                // maybe we shoudle probe file type by it's content
                response.set_raw_content_type(ExtensionToType(extension));

                Fundamental::ScopeGuard g([&]() { response.perform_response(); });
                if (!std_fs::is_regular_file(full_path)) {
                    FDEBUG("{} is not existed", full_path);
                    response.stock_response(http_response::response_type::not_found);
                    return;
                }
                std::ifstream is(full_path.c_str(), std::ios::in | std::ios::binary);
                if (!is) {
                    response.stock_response(http_response::not_found);
                    return;
                }

                auto size = static_cast<std::size_t>(std_fs::file_size(full_path));
                try {
                    auto fetch_range = request.get_bytes_range(size);
                    auto iter        = fetch_range.begin();
                    if (iter != fetch_range.end()) {
                        response.set_bytes_range(iter->low, iter->up - 1, size);
                        size = iter->up - iter->low;
                        is.seekg(iter->low);
                    }
                } catch (const std::exception& e) {
                    response.stock_response(http_response::forbidden);
                    return;
                }

                DeclareTimeTacker(TrackerType, send_t, s_tag,
                                  Fundamental::StringFormat("http send file:{} size:{}", full_path, size), 1000, true,
                                  TrackerOutPut);
                // notify headeres already
                response.set_body_size(size);
                std::array<std::uint8_t, 4096> read_buf;
                std::condition_variable cv;
                std::mutex notify_mutex;
                std::size_t max_pending_size = 8 * 1024 * 1024; // 8M
                std::size_t max_wait_ms      = 5000;
                auto h                       = response.notify_pending_size.Connect([&](std::size_t pending_size) {
                    if (pending_size >= max_pending_size) return;
                    std::scoped_lock<std::mutex> locker(notify_mutex);
                    cv.notify_one();
                });
                Fundamental::ScopeGuard g2([&]() { // disconnect to wait for send finished
                    response.notify_pending_size.DisConnect(h);
                });
                while (size > 0) {
                    auto read_count = is.read((char*)read_buf.data(), read_buf.size()).gcount();
                    if (read_count > 0) {
                        auto read_size = static_cast<std::size_t>(read_count);
                        response.append_body(read_buf.data(), read_size);
                        if (read_size >= size) {
                            break;
                        }
                        size -= read_size;
                        std::unique_lock<std::mutex> locker(notify_mutex);
                        auto ret = cv.wait_for(locker, std::chrono::milliseconds(max_wait_ms), [&]() -> bool {
                            return response.get_data_pending_size() < max_pending_size;
                        });
                        if (!ret) {
                            FDEBUG("http connection send abort,network is too poor");
                            break;
                        }
                    } else {
                        break;
                    }
                }
            });
            break;
        }
        case MethodFilter::HttpPut: {
            pool.Enqueue([conn, full_path]() {
                Fundamental::Utils::SetThreadName("http_put");
                auto& response = conn->get_response();
                auto& request  = conn->get_request();

                Fundamental::ScopeGuard g([&]() { response.perform_response(); });
                if (std_fs::is_regular_file(full_path)) {
                    response.stock_response(http_response::response_type::forbidden);
                    return;
                }
                auto& body        = request.get_body();
                auto write_ret    = Fundamental::fs::WriteFile(full_path, body.data(), body.size());
                auto response_str = Fundamental::StringFormat("write {} bytes result:{}", body.size(), write_ret);
                response.set_content_type(".txt");
                response.set_body_size(response_str.size());
                FINFO("{}", response_str);
                response.append_body(std::move(response_str));
            });
            break;
        }
        default: response.stock_response(http_response::response_type::not_implemented); break;
        }
    };

void http_server::enable_default_handler(http_handler handler, std::uint32_t methodMask) {
    router_.set_default_route_table({ handler, methodMask });
}
} // namespace network::http
