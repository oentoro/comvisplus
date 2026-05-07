#pragma once

#include "app_config.hpp"
#include "types.hpp"

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

namespace comvisplus {

class OpenVinoBackend {
public:
    explicit OpenVinoBackend(const AppConfig& app_config);

    bool available() const;
    const std::string& status() const;
    std::vector<Detection> infer(const cv::Mat& frame) const;

private:
    AppConfig app_config_;
    bool available_ = false;
    std::string status_ = "OpenVINO backend not initialized";

#ifdef COMVISPLUS_HAVE_OPENVINO
    struct Impl;
    std::shared_ptr<Impl> impl_;
#endif
};

}  // namespace comvisplus
