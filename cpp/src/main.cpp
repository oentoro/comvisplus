#include "app_config.hpp"
#include "camera_manager.hpp"
#include "counter_engine.hpp"
#include "http_server.hpp"
#include "types.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    using namespace comvisplus;

    const AppConfig config = parse_args(argc, argv);

    std::cout << "Comvisplus Native Scaffold\n";
    std::cout << "Date target: May 7, 2026\n";
    std::cout << "Listen     : http://" << config.host << ':' << config.port << '\n';
    std::cout << "Model      : " << config.model_path << '\n';
    std::cout << "Auth       : " << (config.auth_enabled ? "enabled" : "disabled") << '\n';

    auto engine = std::make_shared<CounterEngine>(config);
    auto manager = std::make_shared<CameraManager>(config, engine);

    const char* rtsp_url = std::getenv("RTSP_URL");
    if (rtsp_url != nullptr && *rtsp_url != '\0') {
        CameraConfig camera;
        camera.label = "RTSP Camera";
        camera.rtsp_url = rtsp_url;
        manager->add_camera(camera);
    } else {
        std::cout << "RTSP_URL not set. Starting with demo placeholder camera.\n";
        CameraConfig demo_camera;
        demo_camera.label = "Demo RTSP Camera";
        demo_camera.rtsp_url = "rtsp://user:password@camera/stream";
        manager->add_camera(demo_camera);
    }

    HttpServer server(config, manager);
    server.run();

    return 0;
}
