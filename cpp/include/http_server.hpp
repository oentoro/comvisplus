#pragma once

#include "app_config.hpp"
#include "camera_manager.hpp"

#include <atomic>
#include <memory>

namespace comvisplus {

class HttpServer {
public:
    HttpServer(const AppConfig& app_config, std::shared_ptr<CameraManager> manager);

    void run();

private:
    void accept_loop(int server_fd);
    void handle_client(int client_fd) const;
    void print_routes() const;

    AppConfig app_config_;
    std::shared_ptr<CameraManager> manager_;
    std::atomic<bool> running_ {true};
};

}  // namespace comvisplus
