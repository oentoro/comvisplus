#pragma once

#include "app_config.hpp"
#include "openvino_backend.hpp"
#include "types.hpp"

#include <memory>

namespace comvisplus {

class CounterEngine {
public:
    explicit CounterEngine(const AppConfig& app_config);

    void run(const CameraRuntimePtr& runtime) const;

private:
    AppConfig app_config_;
    std::shared_ptr<OpenVinoBackend> backend_;
};

}  // namespace comvisplus
