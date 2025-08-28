#include "network/io_context_pool.hpp"
#include "network/rudp/asio_rudp.hpp"

#include <gtest/gtest.h>
using namespace network;
using namespace network::rudp;
#if 0
TEST(rudp_test, basic) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    EXPECT_TRUE(!ec);

    rudp_bind(server_handler, 42000, "", ec);
    EXPECT_TRUE(!ec);
    rudp_listen(server_handler, 10, ec);
    EXPECT_TRUE(!ec);
    rudp_handle_t accept_handle;
    std::promise<void> send_promise;
    async_rudp_accept(server_handler, network::io_context_pool::Instance().get_io_context(),
                      [&](rudp_handle_t handle, Fundamental::error_code ec) {
                          EXPECT_TRUE(!ec);
                          if (!ec) FINFO("server accept success");
                          accept_handle = handle;
                          async_rudp_send(handle, "test", 4, [&](std::size_t, Fundamental::error_code e) {
                              EXPECT_TRUE(!e);
                              FINFO("send over");
                              send_promise.set_value();
                          });
                      });
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    EXPECT_TRUE(!ec);
    rudp_bind(client_handler, 0, "", ec);
    EXPECT_TRUE(!ec);
    std::promise<void> finish_promise;
    std::string recv_buf;
    recv_buf.resize(4);
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        FWARN("client connect success");
        EXPECT_TRUE(!e);
        async_rudp_recv(client_handler, recv_buf.data(), recv_buf.size(), [&](std::size_t, Fundamental::error_code e) {
            EXPECT_TRUE(!e);
            FINFO("recv over");
            finish_promise.set_value();
        });
    });
    auto f = finish_promise.get_future();
    f.wait_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(f.valid());
    f.wait();
    send_promise.get_future().wait();
    server_handler->destroy();
    EXPECT_FALSE(*server_handler);
    accept_handle->destroy();
    EXPECT_FALSE(*accept_handle);
    client_handler->destroy();
    EXPECT_FALSE(*client_handler);
}

TEST(rudp_test, no_accept_auto_disconnect) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS, 100);
    rudp_bind(server_handler, 42000, "", ec);
    rudp_listen(server_handler, 10, ec);
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS, 2000);
    rudp_bind(client_handler, 0, "", ec);
    std::promise<void> finish_promise;
    std::string recv_buf;
    recv_buf.resize(4);
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        FWARN("client connect success");
        EXPECT_TRUE(!e);
        finish_promise.set_value();
    });
    auto f = finish_promise.get_future();
    f.wait();
    EXPECT_TRUE(*client_handler);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(*client_handler);
}

TEST(rudp_test, listen_queue_size) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_bind(server_handler, 42000, "", ec);
    rudp_listen(server_handler, 1, ec);

    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_bind(client_handler, 0, "", ec);
    std::promise<void> finish_promise;
    std::string recv_buf;
    recv_buf.resize(4);
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        FWARN("client connect success");
        EXPECT_TRUE(!e);
        finish_promise.set_value();
    });
    {
        auto f = finish_promise.get_future();
        f.wait();
    }

    {
        auto client_handler2 = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        rudp_bind(client_handler2, 0, "", ec);
        std::promise<void> finish_promise2;
        async_rudp_connect(client_handler2, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
            EXPECT_FALSE(!e);
            finish_promise2.set_value();
        });
        auto f2 = finish_promise2.get_future();
        f2.wait();
    }
    // release connection
    client_handler->destroy();
    {
        auto client_handler2 = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        rudp_bind(client_handler2, 0, "", ec);
        std::promise<void> finish_promise2;
        async_rudp_connect(client_handler2, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
            EXPECT_TRUE(!e);
            finish_promise2.set_value();
        });
        auto f2 = finish_promise2.get_future();
        f2.wait();
    }
}

TEST(rudp_test, connection_auto_disconnect) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS, 100);
    rudp_bind(server_handler, 42000, "", ec);
    rudp_listen(server_handler, 10, ec);
    rudp_handle_t accept_handle;
    std::promise<void> accept_promise;
    async_rudp_accept(server_handler, network::io_context_pool::Instance().get_io_context(),
                      [&](rudp_handle_t handle, Fundamental::error_code ec) {
                          EXPECT_TRUE(!ec);
                          accept_handle = handle;
                          accept_promise.set_value();
                      });
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS, 2000);
    rudp_bind(client_handler, 0, "", ec);
    std::promise<void> finish_promise;
    std::string recv_buf;
    recv_buf.resize(4);
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        FWARN("client connect success");
        EXPECT_TRUE(!e);
        finish_promise.set_value();
    });
    auto f = finish_promise.get_future();
    f.wait();
    accept_promise.get_future().wait();
    EXPECT_TRUE(*client_handler);
    EXPECT_TRUE(*accept_handle);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(*client_handler);
    EXPECT_FALSE(*accept_handle);
}
#endif

struct rudp_acceptor {
    rudp_acceptor(rudp_handle_t& handle) : rudp_handle(handle) {
    }

    void start(std::function<void(rudp_handle_t)> new_connection__cb) {
        cb = new_connection__cb;
        process_accept();
    }

    void process_accept() {
        if (!*rudp_handle) return;
        async_rudp_accept(rudp_handle, network::io_context_pool::Instance().get_io_context(),
                          [this](rudp_handle_t handle, Fundamental::error_code ec) {
                              if (ec) return;
                              process_accept();
                              if (cb) cb(handle);
                                                    });
    }

    rudp_handle_t& rudp_handle;
    std::function<void(rudp_handle_t)> cb;
};

TEST(rudp_test, multi_connection) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);

    rudp_bind(server_handler, 42000, "", ec);
    std::size_t max_connections = 10;
    rudp_listen(server_handler, max_connections, ec);
    std::mutex data_mutex;
    std::list<rudp_handle_t> hande_list;
    std::list<rudp_handle_t> client_hande_list;
    std::list<std::shared_ptr<std::promise<void>>> send_promises;
    std::list<std::shared_ptr<std::promise<void>>> recv_promises;

    rudp_acceptor acceptor(server_handler);
    acceptor.start([&data_mutex, &hande_list, &send_promises](rudp_handle_t handle) {
        std::shared_ptr<std::promise<void>> promise = std::make_shared<std::promise<void>>();
        {
            std::scoped_lock<std::mutex> locker(data_mutex);
            hande_list.push_back(handle);
            send_promises.emplace_back(promise);
        }
        async_rudp_send(handle, "test", 4, [promise](std::size_t, Fundamental::error_code e) {
            EXPECT_TRUE(!e);
            promise->set_value();
        });
    });

    for (std::size_t i = 0; i < max_connections; ++i) {
        auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        client_hande_list.push_back(client_handler);
        rudp_bind(client_handler, 0, "", ec);
        std::shared_ptr<std::promise<void>> promise = std::make_shared<std::promise<void>>();
        recv_promises.emplace_back(promise);

        async_rudp_connect(client_handler, "127.0.0.1", 42000,
                           [client_handler, promise](Fundamental::error_code e) mutable {
                               EXPECT_TRUE(!e);
                               auto recv_buf = std::make_shared<std::string>();
                               recv_buf->resize(4);

                               async_rudp_recv(client_handler, recv_buf->data(), recv_buf->size(),
                                               [promise, recv_buf](std::size_t, Fundamental::error_code e) mutable {
                                                   EXPECT_TRUE(!e);
                                                   promise->set_value();
                                               });
                           });
    }
    for (auto& item : recv_promises)
        item->get_future().wait();
    for (auto& item : send_promises)
        item->get_future().wait();
}

int main(int argc, char** argv) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel         = Fundamental::LogLevel::debug;
    options.logFormat            = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    options.logOutputProgramName = "test";
    options.logOutputPath        = "output";
    Fundamental::Logger::Initialize(std::move(options));
    ::testing::InitGoogleTest(&argc, argv);
    network::io_context_pool::s_excutorNums = 2;
    network::io_context_pool::Instance().start();
    return RUN_ALL_TESTS();
}
