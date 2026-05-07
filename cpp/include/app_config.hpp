#pragma once

#include <cstdint>
#include <string>

namespace comvisplus {

struct AppConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 5000;
    bool auth_enabled = true;
    std::string auth_username = "admin";
    std::string auth_password = "admin";
    std::string model_path = "yolov8n_openvino_model";
    std::string cameras_file = "cameras.json";
    std::string screenshots_dir = "screenshots";
};

AppConfig parse_args(int argc, char** argv);

}  // namespace comvisplus
