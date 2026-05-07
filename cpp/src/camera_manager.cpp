#include "camera_manager.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <opencv2/core/persistence.hpp>
#include <sstream>

namespace comvisplus {

namespace {

std::string make_camera_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << std::hex << (now & 0xffffffff);
    return oss.str();
}

LineConfig parse_line(const cv::FileNode& node) {
    LineConfig line;
    const auto line_p1 = node["line_p1"];
    const auto line_p2 = node["line_p2"];
    if (!line_p1.empty() && !line_p2.empty() && line_p1.isSeq() && line_p2.isSeq()) {
        line.x1 = static_cast<float>(line_p1[0].real());
        line.y1 = static_cast<float>(line_p1[1].real());
        line.x2 = static_cast<float>(line_p2[0].real());
        line.y2 = static_cast<float>(line_p2[1].real());
        return line;
    }

    const std::string line_dir = static_cast<std::string>(node["line_dir"]);
    const float line_pos = node["line_pos"].empty() ? 0.5F : static_cast<float>(node["line_pos"].real());
    if (line_dir == "horizontal") {
        line.x1 = 0.0F;
        line.y1 = line_pos;
        line.x2 = 1.0F;
        line.y2 = line_pos;
    } else {
        line.x1 = line_pos;
        line.y1 = 0.0F;
        line.x2 = line_pos;
        line.y2 = 1.0F;
    }
    return line;
}

void start_runtime_worker(const std::shared_ptr<CounterEngine>& engine, const CameraRuntimePtr& runtime) {
    runtime->running = true;
    runtime->worker = std::thread([engine, runtime]() { engine->run(runtime); });
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
    start_runtime_worker(engine_, runtime);

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

bool CameraManager::update_camera(const std::string& camera_id, const CameraConfig& config) {
    CameraRuntimePtr runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cameras_.find(camera_id);
        if (it == cameras_.end()) {
            return false;
        }
        runtime = it->second;
    }

    bool must_restart = false;
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        must_restart =
            runtime->config.rtsp_url != config.rtsp_url
            || runtime->config.inference_size != config.inference_size;
        runtime->config.label = config.label;
        runtime->config.rtsp_url = config.rtsp_url;
        runtime->config.frame_skip = config.frame_skip;
        runtime->config.line = config.line;
        runtime->config.inference_size = config.inference_size;
    }

    if (must_restart) {
        runtime->running = false;
        if (runtime->worker.joinable()) {
            runtime->worker.join();
        }
        start_runtime_worker(engine_, runtime);
    }

    return true;
}

bool CameraManager::update_line(const std::string& camera_id, const LineConfig& line) {
    const auto runtime = get(camera_id);
    if (runtime == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->config.line = line;
    return true;
}

bool CameraManager::load_from_disk() {
    std::ifstream check(app_config_.cameras_file);
    if (!check.good()) {
        return false;
    }
    check.close();

    cv::FileStorage storage(app_config_.cameras_file, cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    if (!storage.isOpened()) {
        return false;
    }

    const auto cameras = storage.root();
    if (cameras.empty() || !cameras.isSeq()) {
        return false;
    }

    int loaded = 0;
    for (const auto& node : cameras) {
        CameraConfig config;
        config.label = static_cast<std::string>(node["label"]);
        config.rtsp_url = static_cast<std::string>(node["url"]);
        config.frame_skip = node["frame_skip"].empty() ? 2 : static_cast<int>(node["frame_skip"]);
        config.line = parse_line(node);

        if (config.rtsp_url.empty()) {
            continue;
        }
        add_camera(config);
        loaded += 1;
    }

    std::cout << "[camera-manager] loaded " << loaded << " camera(s) from " << app_config_.cameras_file << '\n';
    return loaded > 0;
}

bool CameraManager::save_to_disk() const {
    cv::FileStorage storage(app_config_.cameras_file, cv::FileStorage::WRITE | cv::FileStorage::FORMAT_JSON);
    if (!storage.isOpened()) {
        return false;
    }

    storage.startWriteStruct("", cv::FileNode::SEQ);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : cameras_) {
            const auto& config = item.second->config;
            storage.startWriteStruct("", cv::FileNode::MAP);
            storage << "url" << config.rtsp_url;
            storage << "label" << config.label;
            storage << "line_p1" << "[" << config.line.x1 << config.line.y1 << "]";
            storage << "line_p2" << "[" << config.line.x2 << config.line.y2 << "]";
            storage << "frame_skip" << config.frame_skip;
            storage.endWriteStruct();
        }
    }
    storage.endWriteStruct();
    storage.release();
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
