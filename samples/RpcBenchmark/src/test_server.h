#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <future>

constexpr const char* kProxyServiceName  = "rpc_service";
constexpr const char* kProxyServiceField = "rpc_field";
constexpr const char* kProxyServiceToken = "rpc_token";
void run_server();
void server_task(std::promise<void>& sync_p);
void exit_server();