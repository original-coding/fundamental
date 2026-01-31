#include "fundamental/basic/random_generator.hpp"
#include "fundamental/delay_queue/delay_queue.h"
#include "network/io_context_pool.hpp"
#include "network/rudp/asio_rudp.hpp"

#include <gtest/gtest.h>
using namespace network;
using namespace network::rudp;

#if 1
    #if 1
TEST(rudp_test, basic) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    EXPECT_TRUE(!ec);

    rudp_bind(server_handler, 42000, "::", ec);
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
    rudp_bind(client_handler, 0, "127.0.0.1", ec);
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
    rudp_bind(server_handler, 42000, "::", ec);
    rudp_listen(server_handler, 10, ec);
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS, 2000);
    rudp_bind(client_handler, 0, "127.0.0.1", ec);
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
    #endif

TEST(rudp_test, listen_queue_size) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_bind(server_handler, 42000, "::", ec);
    rudp_listen(server_handler, 1, ec);

    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_bind(client_handler, 0, "127.0.0.1", ec);
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
        rudp_config(client_handler2, rudp_config_type::RUDP_CONNECT_TIMEOUT_MS, 10);
        rudp_config(client_handler2, rudp_config_type::RUDP_COMMAND_MAX_TRY_CNT, 2);
        rudp_bind(client_handler2, 0, "127.0.0.1", ec);
        std::promise<void> finish_promise2;
        async_rudp_connect(client_handler2, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
            EXPECT_FALSE(!e);
            FWARN(e);
            finish_promise2.set_value();
        });
        auto f2 = finish_promise2.get_future();
        f2.wait();
    }
    {
        auto time_now = Fundamental::Timer::GetTimeNow();
        asio::steady_timer t(network::io_context_pool::Instance().get_io_context());
        std::int64_t delay_ms = 200;
        t.expires_after(std::chrono::milliseconds(delay_ms));
        // delay release listen queue
        t.async_wait([=](Fundamental::error_code) { client_handler->destroy(); });
        auto client_handler2 = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        rudp_bind(client_handler2, 0, "127.0.0.1", ec);
        std::promise<std::int64_t> finish_promise2;
        async_rudp_connect(client_handler2, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
            EXPECT_TRUE(!e);
            finish_promise2.set_value(Fundamental::Timer::GetTimeNow());
        });
        auto f2 = finish_promise2.get_future();
        f2.wait();
        auto diff = f2.get() - time_now;
        EXPECT_GE(diff, delay_ms);
    }
}
    #if 1
TEST(rudp_test, test_stream_mode_connect) {
    {
        Fundamental::error_code ec;
        auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);

        rudp_bind(server_handler, 42000, "::", ec);
        rudp_listen(server_handler, 10, ec);
        {
            auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
            rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 1);
            rudp_bind(client_handler, 0, "127.0.0.1", ec);
            std::promise<void> connect_promise;
            async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
                EXPECT_FALSE(!e);
                FWARN(e);
                connect_promise.set_value();
            });
            connect_promise.get_future().wait();
        }
        {
            auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
            rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);
            rudp_bind(client_handler, 0, "127.0.0.1", ec);
            std::promise<void> connect_promise;
            async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
                EXPECT_TRUE(!e);
                connect_promise.set_value();
            });
            connect_promise.get_future().wait();
        }
    }
    {
        Fundamental::error_code ec;
        auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 1);

        rudp_bind(server_handler, 42000, "::", ec);
        rudp_listen(server_handler, 10, ec);
        {
            auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
            rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 1);
            rudp_bind(client_handler, 0, "127.0.0.1", ec);
            std::promise<void> connect_promise;
            async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
                EXPECT_TRUE(!e);
                FWARN(e);
                connect_promise.set_value();
            });
            connect_promise.get_future().wait();
        }
        {
            auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
            rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);
            rudp_bind(client_handler, 0, "127.0.0.1", ec);
            std::promise<void> connect_promise;
            async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
                EXPECT_TRUE(!e);
                connect_promise.set_value();
            });
            connect_promise.get_future().wait();
        }
    }
}

TEST(rudp_test, connection_auto_disconnect) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_IDLE_CONNECTION_TIME_MS, 100);
    rudp_bind(server_handler, 42000, "::", ec);
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
    rudp_bind(client_handler, 0, "127.0.0.1", ec);
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

struct rudp_acceptor : public std::enable_shared_from_this<rudp_acceptor> {
    rudp_acceptor(rudp_handle_t& handle) : rudp_handle(handle) {
    }

    void start(std::function<void(rudp_handle_t)> new_connection__cb) {
        cb = new_connection__cb;
        process_accept();
    }

    void process_accept() {
        if (!*rudp_handle) return;
        async_rudp_accept(rudp_handle, network::io_context_pool::Instance().get_io_context(),
                          [this, ref = weak_from_this()](rudp_handle_t handle, Fundamental::error_code ec) {
                              auto strong = ref.lock();
                              if (!strong) return;
                              EXPECT_TRUE(!ec);
                              if (max_size > 0) {
                                  --max_size;
                                  FASSERT(!ec, ec);
                                  if (cb) cb(handle);
                              }
                              process_accept();
                          });
    }
    std::atomic<std::size_t> max_size = 0;
    rudp_handle_t& rudp_handle;
    std::function<void(rudp_handle_t)> cb;
};

TEST(rudp_test, multi_connection) {
    Fundamental::error_code ec;
    auto server_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_bind(server_handler, 42000, "::", ec);
    std::size_t max_connections = 1000;
    rudp_listen(server_handler, max_connections, ec);

    std::mutex data_mutex;
    std::list<rudp_handle_t> hande_list;
    std::list<rudp_handle_t> client_hande_list;
    std::list<std::shared_ptr<std::promise<void>>> recv_promises;

    auto acceptor                 = std::make_shared<rudp_acceptor>(server_handler);
    acceptor->max_size            = max_connections;
    std::atomic<std::int32_t> cnt = 0;
    acceptor->start([&data_mutex, &hande_list, &cnt](rudp_handle_t handle) {
        {
            std::scoped_lock<std::mutex> locker(data_mutex);
            hande_list.push_back(handle);
        }
        cnt.fetch_sub(1);
    });

    for (std::size_t i = 0; i < max_connections; ++i) {
        auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
        client_hande_list.push_back(client_handler);
        rudp_bind(client_handler, 0, "127.0.0.1", ec);
        std::shared_ptr<std::promise<void>> promise = std::make_shared<std::promise<void>>();
        recv_promises.emplace_back(promise);

        async_rudp_connect(client_handler, "127.0.0.1", 42000,
                           [client_handler, promise, &cnt](Fundamental::error_code e) mutable {
                               EXPECT_TRUE(!e);
                               FASSERT(!e, e);
                               promise->set_value();
                               cnt.fetch_add(1);
                           });
    }
    for (auto& item : recv_promises)
        item->get_future().wait();
    while (cnt != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    acceptor.reset();
}

TEST(rudp_test, test_mtu_select) {
    Fundamental::error_code ec;
    std::size_t data_chunk_size   = 256;
    std::size_t send_windows_size = 100;
    std::size_t recv_windows_size = 100;
    auto server_handler           = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);
    rudp_config(server_handler, rudp_config_type::RUDP_MTU_SIZE, data_chunk_size + kRudpProtocalHeadSize);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, send_windows_size);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, recv_windows_size);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 1);

    rudp_bind(server_handler, 42000, "::", ec);
    rudp_listen(server_handler, 10, ec);
    std::promise<rudp_handle_t> accept_promise;
    async_rudp_accept(server_handler, network::io_context_pool::Instance().get_io_context(),
                      [&](rudp_handle_t handle, Fundamental::error_code ec) {
                          EXPECT_TRUE(!ec);
                          accept_promise.set_value(handle);
                      });
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);
    rudp_config(client_handler, rudp_config_type::RUDP_MTU_SIZE, data_chunk_size / 2 + kRudpProtocalHeadSize);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, send_windows_size);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, recv_windows_size);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 1);

    rudp_bind(client_handler, 0, "127.0.0.1", ec);
    std::promise<void> connect_promise;
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        EXPECT_TRUE(!e);
        connect_promise.set_value();
    });
    connect_promise.get_future().wait();
    auto remote_handler = accept_promise.get_future().get();
    {
        std::string failed_data(send_windows_size * data_chunk_size, 'c');
        std::promise<void> send_promise;
        async_rudp_send(client_handler, failed_data.data(), failed_data.size(),
                        [&](std::size_t, Fundamental::error_code ec) {
                            EXPECT_FALSE(!ec);
                            FWARN(ec);
                            send_promise.set_value();
                        });
        send_promise.get_future().wait();
    }
    {
        std::string success_data(send_windows_size * data_chunk_size / 2, 'c');
        std::string recv_data;
        recv_data.resize(success_data.size());
        std::promise<void> send_promise;
        async_rudp_send(client_handler, success_data.data(), success_data.size(),
                        [&](std::size_t, Fundamental::error_code ec) {
                            EXPECT_TRUE(!ec);
                            send_promise.set_value();
                        });
        std::promise<void> recv_promise;
        async_rudp_recv(remote_handler, recv_data.data(), recv_data.size(),
                        [&](std::size_t, Fundamental::error_code ec) {
                            EXPECT_TRUE(!ec);
                            recv_promise.set_value();
                        });
        send_promise.get_future().wait();
        recv_promise.get_future().wait();
        EXPECT_EQ(success_data, recv_data);
    }
    {
        std::string success_data(send_windows_size * data_chunk_size / 2, 'c');
        std::string recv_data;
        recv_data.resize(success_data.size());
        std::promise<void> send_promise;
        async_rudp_send(client_handler, success_data.data(), success_data.size(),
                        [&](std::size_t, Fundamental::error_code ec) {
                            EXPECT_TRUE(!ec);
                            send_promise.set_value();
                        });
        std::promise<void> recv_promise;
        async_rudp_recv(remote_handler, recv_data.data(), recv_data.size() - 1,
                        [&](std::size_t len, Fundamental::error_code ec) {
                            EXPECT_FALSE(!ec);
                            FWARN(ec);
                            EXPECT_EQ(recv_data.size(), len + 1);
                            recv_promise.set_value();
                        });
        send_promise.get_future().wait();
        recv_promise.get_future().wait();
        EXPECT_TRUE(std::strncmp(success_data.data(), recv_data.data(), recv_data.size() - 1) == 0);
    }
}

TEST(rudp_test, test_stream_mode) {
    Fundamental::error_code ec;
    std::size_t data_chunk_size   = 256;
    std::size_t send_windows_size = 100;
    std::size_t recv_windows_size = 80;
    auto server_handler           = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 1);
    rudp_config(server_handler, rudp_config_type::RUDP_MTU_SIZE, data_chunk_size + kRudpProtocalHeadSize);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, send_windows_size);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, recv_windows_size);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 1);

    rudp_bind(server_handler, 42000, "::", ec);
    rudp_listen(server_handler, 10, ec);
    std::promise<rudp_handle_t> accept_promise;
    async_rudp_accept(server_handler, network::io_context_pool::Instance().get_io_context(),
                      [&](rudp_handle_t handle, Fundamental::error_code ec) {
                          EXPECT_TRUE(!ec);
                          accept_promise.set_value(handle);
                      });
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 1);
    rudp_config(client_handler, rudp_config_type::RUDP_MTU_SIZE, data_chunk_size + kRudpProtocalHeadSize);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, send_windows_size);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, recv_windows_size);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 1);

    rudp_bind(client_handler, 0, "127.0.0.1", ec);
    std::promise<void> connect_promise;
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        EXPECT_TRUE(!e);
        connect_promise.set_value();
    });
    connect_promise.get_future().wait();
    auto remote_handler = accept_promise.get_future().get();
    {
        std::string success_data;
        success_data.resize(send_windows_size * data_chunk_size);
        auto generate = Fundamental::DefaultNumberGenerator<char>('a', 'z');
        generate.gen(success_data.data(), success_data.size());
        std::string recv_data;
        recv_data.resize(success_data.size());
        std::promise<void> send_promise;
        async_rudp_send(client_handler, success_data.data(), success_data.size(),
                        [&](std::size_t, Fundamental::error_code ec) {
                            EXPECT_TRUE(!ec);
                            send_promise.set_value();
                        });
        for (std::size_t i = 0; i < send_windows_size; ++i) {
            std::promise<void> recv_promise;
            async_rudp_recv(remote_handler, recv_data.data() + i * data_chunk_size, data_chunk_size,
                            [&](std::size_t len, Fundamental::error_code ec) {
                                EXPECT_TRUE(!ec);
                                EXPECT_TRUE(len == data_chunk_size);
                                recv_promise.set_value();
                            });
            recv_promise.get_future().wait();
        }

        send_promise.get_future().wait();

        EXPECT_TRUE(success_data == recv_data);
    }
    {
        std::string success_data;
        success_data.resize(data_chunk_size);
        auto generate = Fundamental::DefaultNumberGenerator<char>('a', 'z');
        generate.gen(success_data.data(), success_data.size());
        std::string recv_data;
        recv_data.resize(data_chunk_size);
        std::vector<std::promise<void>> send_promises;
        send_promises.resize(data_chunk_size);
        for (std::size_t i = 0; i < data_chunk_size; ++i) {
            async_rudp_send(client_handler, success_data.data() + i, 1,
                            [&, num = i](std::size_t len, Fundamental::error_code ec) {
                                EXPECT_TRUE(!ec);
                                EXPECT_EQ(len, 1);
                                send_promises[num].set_value();
                            });
        }

        std::promise<void> recv_promise;
        async_rudp_recv(remote_handler, recv_data.data(), data_chunk_size,
                        [&](std::size_t len, Fundamental::error_code ec) {
                            EXPECT_TRUE(!ec);
                            EXPECT_TRUE(len == data_chunk_size);
                            recv_promise.set_value();
                        });
        recv_promise.get_future().wait();
        for (auto& item : send_promises)
            item.get_future().wait();
        EXPECT_TRUE(success_data == recv_data);
    }
}
    #endif

#endif
TEST(rudp_test, benchmark) {
    Fundamental::error_code ec;
    std::size_t data_chunk_size = 1024 - kRudpProtocalHeadSize;
    std::size_t window_size     = 256;
    std::size_t update_interval = 10;
    auto server_handler         = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);
    rudp_config(server_handler, rudp_config_type::RUDP_MTU_SIZE, data_chunk_size + kRudpProtocalHeadSize);
    rudp_config(server_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, window_size);
    rudp_config(server_handler, rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, window_size);
    rudp_config(server_handler, rudp_config_type::RUDP_UPDATE_INTERVAL_MS, update_interval);
    rudp_bind(server_handler, 42000, "::", ec);
    rudp_listen(server_handler, 10, ec);
    std::promise<rudp_handle_t> accept_promise;
    async_rudp_accept(server_handler, network::io_context_pool::Instance().get_io_context(),
                      [&](rudp_handle_t handle, Fundamental::error_code ec) {
                          EXPECT_TRUE(!ec);
                          accept_promise.set_value(handle);
                      });
    auto client_handler = rudp_create(network::io_context_pool::Instance().get_io_context(), ec);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_STREAM_MODE, 0);
    rudp_config(client_handler, rudp_config_type::RUDP_MTU_SIZE, data_chunk_size + kRudpProtocalHeadSize);
    rudp_config(client_handler, rudp_config_type::RUDP_ENABLE_AUTO_KEEPALIVE, 0);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_RECV_WINDOW_SIZE, window_size);
    rudp_config(client_handler, rudp_config_type::RUDP_MAX_SEND_WINDOW_SIZE, window_size);
    rudp_config(client_handler, rudp_config_type::RUDP_UPDATE_INTERVAL_MS, update_interval);

    rudp_bind(client_handler, 0, "127.0.0.1", ec);
    std::promise<void> connect_promise;
    async_rudp_connect(client_handler, "127.0.0.1", 42000, [&](Fundamental::error_code e) {
        EXPECT_TRUE(!e);
        connect_promise.set_value();
    });
    connect_promise.get_future().wait();
    auto remote_handler = accept_promise.get_future().get();
    FINFO("start rudp benckmark");
    auto now              = Fundamental::Timer::GetTimeNow();
    std::size_t send_nums = 64 * 1024; // 1M
    auto group_size       = 128;
    auto send_task_token  = Fundamental::ThreadPool::DefaultPool().Enqueue([&]() {
        std::string send_data(data_chunk_size * group_size, 'c');
        std::promise<void> send_promise;
        auto loop_func = [&](auto self, std::size_t left_times) {
            if (left_times == 0) {
                send_promise.set_value();
                return;
            }
            --left_times;
            async_rudp_send(client_handler, send_data.data(), send_data.size(),
                             [left_times, self](std::size_t len, Fundamental::error_code ec) {
                                EXPECT_TRUE(!ec);
                                self(self, left_times);
                            });
        };
        loop_func(loop_func, send_nums / group_size);
        send_promise.get_future().get();
    });
    auto recv_task_token  = Fundamental::ThreadPool::DefaultPool().Enqueue([&]() {
        std::string recv_data;
        recv_data.resize(data_chunk_size * group_size);
        std::promise<void> recv_promise;
        auto loop_func = [&](auto self, std::size_t left_times) {
            if (left_times == 0) {
                recv_promise.set_value();
                return;
            }
            --left_times;
            async_rudp_recv(remote_handler, recv_data.data(), recv_data.size(),
                             [left_times, self](std::size_t len, Fundamental::error_code ec) {
                                EXPECT_TRUE(!ec);
                                self(self, left_times);
                            });
        };
        loop_func(loop_func, send_nums / group_size);
        recv_promise.get_future().get();
    });
    send_task_token.resultFuture.get();
    recv_task_token.resultFuture.get();
    auto cost_ms         = Fundamental::Timer::GetTimeNow() - now;
    double send_cost_sec = cost_ms / 1000.0;
    double send_bytes    = static_cast<double>(data_chunk_size * send_nums);
    FWARN("local network speed {}MB/s", send_bytes / 1024.0 / (send_cost_sec * 1024.0));
}

int main(int argc, char** argv) {
    Fundamental::Logger::LoggerInitOptions options;
    options.minimumLevel         = Fundamental::LogLevel::debug;
    options.logFormat            = "%^[%L]%H:%M:%S.%e%$[%t] %v ";
    options.logOutputProgramName = "test";
    options.logOutputPath        = "output";
    Fundamental::Logger::Initialize(std::move(options));
    ::testing::InitGoogleTest(&argc, argv);
    network::io_context_pool::s_excutorNums = 10;
    network::io_context_pool::Instance().start();
    return RUN_ALL_TESTS();
}
