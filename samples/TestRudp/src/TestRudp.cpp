#include "network/io_context_pool.hpp"
#include "network/rudp/asio_rudp.hpp"

#include <gtest/gtest.h>
using namespace network;
using namespace network::rudp;
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
    rudp_release(server_handler);
    rudp_release(accept_handle);
    rudp_release(client_handler);
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
