#pragma once
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

struct person {
    std::size_t id;
    std::string name;
    std::size_t age;
};
struct dummy1 {
    std::size_t id;
    std::string str;
};

struct TestProxyRequest {
    std::int32_t v = 0;
    float f = 0.f;
    std::vector<std::string> strs;
    bool operator==(const TestProxyRequest& other)const {
        return other.f == f && other.v == v && other.strs == strs;
    }
    bool operator!=(const TestProxyRequest& other)const {
        return !(operator==(other));
    }
};
//return string
struct DelayControlStream{
    std::string cmd;//"set" "echo"
    std::uint32_t process_delay;
    std::string msg;
};

constexpr const char* kProxyServiceName  = "rpc_service";
constexpr const char* kProxyServiceField = "rpc_field";
constexpr const char* kProxyServiceToken = "rpc_token";
void run_server();
void exit_server();
void server_task(std::promise<void>& sync_p);