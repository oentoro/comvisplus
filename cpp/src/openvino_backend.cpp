#include "openvino_backend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <utility>

#include <opencv2/imgproc.hpp>

#ifdef COMVISPLUS_HAVE_OPENVINO
#include <openvino/openvino.hpp>
#endif

namespace comvisplus {

namespace {

struct LetterboxResult {
    cv::Mat image;
    float scale = 1.0F;
    float pad_x = 0.0F;
    float pad_y = 0.0F;
};

std::string resolve_model_path(const std::string& input_path) {
    namespace fs = std::filesystem;

    const fs::path path(input_path);
    if (fs::is_regular_file(path)) {
        return path.string();
    }

    if (fs::is_directory(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".xml") {
                return entry.path().string();
            }
        }
    }

    return input_path;
}

float intersection_over_union(const Detection& a, const Detection& b) {
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    const float iw = std::max(0.0F, x2 - x1);
    const float ih = std::max(0.0F, y2 - y1);
    const float inter = iw * ih;
    const float area_a = std::max(0.0F, a.x2 - a.x1) * std::max(0.0F, a.y2 - a.y1);
    const float area_b = std::max(0.0F, b.x2 - b.x1) * std::max(0.0F, b.y2 - b.y1);
    const float denom = area_a + area_b - inter;
    return denom <= 0.0F ? 0.0F : inter / denom;
}

void non_max_suppression(std::vector<Detection>& detections, float iou_threshold) {
    std::sort(detections.begin(), detections.end(), [](const Detection& lhs, const Detection& rhs) {
        return lhs.confidence > rhs.confidence;
    });

    std::vector<Detection> kept;
    std::vector<bool> removed(detections.size(), false);
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (removed[i]) {
            continue;
        }
        kept.push_back(detections[i]);
        for (std::size_t j = i + 1; j < detections.size(); ++j) {
            if (removed[j]) {
                continue;
            }
            if (detections[i].class_id == detections[j].class_id
                && intersection_over_union(detections[i], detections[j]) > iou_threshold) {
                removed[j] = true;
            }
        }
    }
    detections = std::move(kept);
}

LetterboxResult make_letterbox(const cv::Mat& frame, int target_w, int target_h) {
    LetterboxResult result;
    if (frame.empty() || target_w <= 0 || target_h <= 0) {
        return result;
    }

    const float scale = std::min(
        static_cast<float>(target_w) / static_cast<float>(frame.cols),
        static_cast<float>(target_h) / static_cast<float>(frame.rows)
    );
    const int resized_w = std::max(1, static_cast<int>(std::round(frame.cols * scale)));
    const int resized_h = std::max(1, static_cast<int>(std::round(frame.rows * scale)));

    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(resized_w, resized_h));

    const int pad_w = target_w - resized_w;
    const int pad_h = target_h - resized_h;
    const int left = pad_w / 2;
    const int right = pad_w - left;
    const int top = pad_h / 2;
    const int bottom = pad_h - top;

    cv::copyMakeBorder(
        resized,
        result.image,
        top,
        bottom,
        left,
        right,
        cv::BORDER_CONSTANT,
        cv::Scalar(114, 114, 114)
    );
    result.scale = scale;
    result.pad_x = static_cast<float>(left);
    result.pad_y = static_cast<float>(top);
    return result;
}

Detection decode_detection(
    float cx,
    float cy,
    float w,
    float h,
    float score,
    int class_id,
    const LetterboxResult& letterbox,
    const cv::Mat& frame
) {
    Detection det;
    det.x1 = ((cx - (w * 0.5F)) - letterbox.pad_x) / letterbox.scale;
    det.y1 = ((cy - (h * 0.5F)) - letterbox.pad_y) / letterbox.scale;
    det.x2 = ((cx + (w * 0.5F)) - letterbox.pad_x) / letterbox.scale;
    det.y2 = ((cy + (h * 0.5F)) - letterbox.pad_y) / letterbox.scale;
    det.x1 = std::clamp(det.x1, 0.0F, static_cast<float>(frame.cols - 1));
    det.y1 = std::clamp(det.y1, 0.0F, static_cast<float>(frame.rows - 1));
    det.x2 = std::clamp(det.x2, 0.0F, static_cast<float>(frame.cols - 1));
    det.y2 = std::clamp(det.y2, 0.0F, static_cast<float>(frame.rows - 1));
    det.confidence = score;
    det.class_id = class_id;
    return det;
}

}  // namespace

#ifdef COMVISPLUS_HAVE_OPENVINO
struct OpenVinoBackend::Impl {
    ov::Core core;
    ov::CompiledModel compiled_model;
    ov::Shape input_shape;
};
#endif

OpenVinoBackend::OpenVinoBackend(const AppConfig& app_config) : app_config_(app_config) {
#ifdef COMVISPLUS_HAVE_OPENVINO
    try {
        impl_ = std::make_shared<Impl>();
        const std::string resolved_model_path = resolve_model_path(app_config_.model_path);
        auto model = impl_->core.read_model(resolved_model_path);
        impl_->compiled_model = impl_->core.compile_model(model, "CPU");
        const auto input = impl_->compiled_model.input();
        impl_->input_shape = input.get_shape();
        available_ = true;
        status_ = "OpenVINO model loaded from " + resolved_model_path;
    } catch (const std::exception& ex) {
        available_ = false;
        status_ = std::string("OpenVINO unavailable: ") + ex.what();
    }
#else
    status_ = "OpenVINO support not compiled in";
#endif
}

bool OpenVinoBackend::available() const {
    return available_;
}

const std::string& OpenVinoBackend::status() const {
    return status_;
}

std::vector<Detection> OpenVinoBackend::infer(const cv::Mat& frame) const {
    std::vector<Detection> detections;

#ifdef COMVISPLUS_HAVE_OPENVINO
    if (!available_ || impl_ == nullptr || frame.empty()) {
        return detections;
    }

    const std::size_t input_h = impl_->input_shape.at(2);
    const std::size_t input_w = impl_->input_shape.at(3);
    constexpr float kConfidenceThreshold = 0.25F;
    constexpr float kIouThreshold = 0.45F;
    constexpr int kPersonClassId = 0;

    const auto letterbox = make_letterbox(frame, static_cast<int>(input_w), static_cast<int>(input_h));
    if (letterbox.image.empty() || letterbox.scale <= 0.0F) {
        return detections;
    }

    cv::Mat rgb;
    cv::cvtColor(letterbox.image, rgb, cv::COLOR_BGR2RGB);
    rgb.convertTo(rgb, CV_32F, 1.0 / 255.0);

    ov::Tensor input_tensor(ov::element::f32, impl_->input_shape);
    float* input_data = input_tensor.data<float>();
    const std::array<int, 3> channels = {0, 1, 2};
    const std::size_t plane_size = input_h * input_w;
    for (int c : channels) {
        for (std::size_t y = 0; y < input_h; ++y) {
            for (std::size_t x = 0; x < input_w; ++x) {
                input_data[c * plane_size + y * input_w + x] = rgb.at<cv::Vec3f>(
                    static_cast<int>(y), static_cast<int>(x)
                )[c];
            }
        }
    }

    auto infer_request = impl_->compiled_model.create_infer_request();
    infer_request.set_input_tensor(input_tensor);
    infer_request.infer();
    const ov::Tensor output_tensor = infer_request.get_output_tensor();
    const ov::Shape output_shape = output_tensor.get_shape();
    const float* data = output_tensor.data<const float>();

    if (output_shape.size() == 3) {
        if (output_shape[1] >= 6) {
            const std::size_t channels_count = output_shape[1];
            const std::size_t predictions = output_shape[2];
            for (std::size_t i = 0; i < predictions; ++i) {
                const float cx = data[i];
                const float cy = data[predictions + i];
                const float w = data[(2 * predictions) + i];
                const float h = data[(3 * predictions) + i];

                int best_class = -1;
                float best_score = 0.0F;
                for (std::size_t c = 4; c < channels_count; ++c) {
                    const float score = data[(c * predictions) + i];
                    if (score > best_score) {
                        best_score = score;
                        best_class = static_cast<int>(c - 4);
                    }
                }

                if (best_class != kPersonClassId || best_score < kConfidenceThreshold) {
                    continue;
                }
                detections.push_back(
                    decode_detection(cx, cy, w, h, best_score, best_class, letterbox, frame)
                );
            }
        } else if (output_shape[2] >= 6) {
            const std::size_t predictions = output_shape[1];
            const std::size_t values_per_prediction = output_shape[2];
            for (std::size_t i = 0; i < predictions; ++i) {
                const float* row = data + (i * values_per_prediction);
                const float cx = row[0];
                const float cy = row[1];
                const float w = row[2];
                const float h = row[3];

                int best_class = -1;
                float best_score = 0.0F;
                for (std::size_t c = 4; c < values_per_prediction; ++c) {
                    const float score = row[c];
                    if (score > best_score) {
                        best_score = score;
                        best_class = static_cast<int>(c - 4);
                    }
                }

                if (best_class != kPersonClassId || best_score < kConfidenceThreshold) {
                    continue;
                }
                detections.push_back(
                    decode_detection(cx, cy, w, h, best_score, best_class, letterbox, frame)
                );
            }
        }
    }
    non_max_suppression(detections, kIouThreshold);
#else
    (void) frame;
#endif

    return detections;
}

}  // namespace comvisplus
