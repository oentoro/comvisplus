#include "app_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace comvisplus {

namespace {

std::string env_or_default(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return value;
}

}  // namespace

AppConfig parse_args(int argc, char** argv) {
    AppConfig config;
    config.auth_username = env_or_default("AUTH_USERNAME", "admin");
    config.auth_password = env_or_default("AUTH_PASSWORD", "admin");

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--model" && i + 1 < argc) {
            config.model_path = argv[++i];
        } else if (arg == "--no-auth") {
            config.auth_enabled = false;
        } else if (arg == "--help") {
            std::printf(
                "Usage: comvisplus_native [--host 0.0.0.0] [--port 5000] "
                "[--model yolov8n_openvino_model] [--no-auth]\n"
            );
            std::exit(0);
        }
    }

    return config;
}

}  // namespace comvisplus
