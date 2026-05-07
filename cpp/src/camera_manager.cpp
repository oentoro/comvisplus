#include "camera_manager.hpp"

#include <chrono>
#include <sstream>

namespace comvisplus {

namespace {

std::string make_camera_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << std::hex << (now & 0xffffffff);
    return oss.str();
}

}  // namespace

CameraManager::CameraManager(const AppConfig& app_config, std::shared_ptr<CounterEngine> engine)
    : app_config_(app_config), engine_(std::move(engine)) {}

CameraManager::~CameraManager() {
    std::vector<CameraRuntimePtr> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : cameras_) {
            runtimes.push_back(item.second);
        }
    }

    for (const auto& runtime : runtimes) {
        runtime->running = false;
        if (runtime->worker.joinable()) {
            runtime->worker.join();
        }
    }
}

std::string CameraManager::add_camera(const CameraConfig& config) {
    auto runtime = std::make_shared<CameraRuntime>();
    runtime->config = config;
    if (runtime->config.id.empty()) {
        runtime->config.id = make_camera_id();
    }
    runtime->running = true;
    runtime->worker = std::thread([engine = engine_, runtime]() { engine->run(runtime); });

    std::lock_guard<std::mutex> lock(mutex_);
    cameras_[runtime->config.id] = runtime;
    return runtime->config.id;
}

bool CameraManager::remove_camera(const std::string& camera_id) {
    CameraRuntimePtr runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cameras_.find(camera_id);
        if (it == cameras_.end()) {
            return false;
        }
        runtime = it->second;
        cameras_.erase(it);
    }

    runtime->running = false;
    if (runtime->worker.joinable()) {
        runtime->worker.join();
    }
    return true;
}

std::vector<CameraConfig> CameraManager::list_configs() const {
    std::vector<CameraConfig> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : cameras_) {
        result.push_back(item.second->config);
    }
    return result;
}

std::vector<CameraRuntimePtr> CameraManager::list_runtimes() const {
    std::vector<CameraRuntimePtr> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : cameras_) {
        result.push_back(item.second);
    }
    return result;
}

CameraRuntimePtr CameraManager::get(const std::string& camera_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cameras_.find(camera_id);
    if (it == cameras_.end()) {
        return nullptr;
    }
    return it->second;
}

}  // namespace comvisplus
