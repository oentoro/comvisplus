#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

namespace comvisplus {

struct LineConfig {
    float x1 = 0.5F;
    float y1 = 0.0F;
    float x2 = 0.5F;
    float y2 = 1.0F;
};

struct CameraConfig {
    std::string id;
    std::string label;
    std::string rtsp_url;
    LineConfig line;
    int frame_skip = 2;
    int inference_size = 320;
};

struct CounterStats {
    std::uint64_t total = 0;
    std::uint64_t right = 0;
    std::uint64_t left = 0;
};

struct Detection {
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float confidence = 0.0F;
    int class_id = -1;
};

inline std::string_view class_label(int class_id) {
    if (class_id == 0) {
        return "person";
    }
    return "object";
}

struct SharedFrame {
    std::vector<unsigned char> jpeg;
    std::uint64_t sequence = 0;
};

struct CameraRuntime {
    CameraConfig config;
    CounterStats stats;
    SharedFrame latest_frame;
    bool online = false;
    bool reconnecting = false;
    std::string last_error;
    std::atomic<bool> running {false};
    std::thread worker;
    std::mutex mutex;
};

using CameraRuntimePtr = std::shared_ptr<CameraRuntime>;

}  // namespace comvisplus
