#pragma once

#include "counter_engine.hpp"
#include "types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace comvisplus {

class CameraManager {
public:
    CameraManager(const AppConfig& app_config, std::shared_ptr<CounterEngine> engine);
    ~CameraManager();

    std::string add_camera(const CameraConfig& config);
    bool remove_camera(const std::string& camera_id);
    bool update_camera(const std::string& camera_id, const CameraConfig& config);
    bool update_line(const std::string& camera_id, const LineConfig& line);
    bool load_from_disk();
    bool save_to_disk() const;

    std::vector<CameraConfig> list_configs() const;
    std::vector<CameraRuntimePtr> list_runtimes() const;
    CameraRuntimePtr get(const std::string& camera_id) const;

private:
    AppConfig app_config_;
    std::shared_ptr<CounterEngine> engine_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, CameraRuntimePtr> cameras_;
};

}  // namespace comvisplus
