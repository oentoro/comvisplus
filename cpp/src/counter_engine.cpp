#include "counter_engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace comvisplus {

namespace {

struct Track {
    int id = 0;
    cv::Point center;
    cv::Rect box;
    int frames_seen = 0;
    int missed_frames = 0;
    bool counted = false;
    int last_side = 0;
};

cv::Point to_point(const LineConfig& line, int width, int height, bool first) {
    if (first) {
        return {
            static_cast<int>(line.x1 * static_cast<float>(width)),
            static_cast<int>(line.y1 * static_cast<float>(height))
        };
    }
    return {
        static_cast<int>(line.x2 * static_cast<float>(width)),
        static_cast<int>(line.y2 * static_cast<float>(height))
    };
}

void write_jpeg(CameraRuntime& runtime, const cv::Mat& frame) {
    std::vector<unsigned char> buffer;
    cv::imencode(".jpg", frame, buffer, {cv::IMWRITE_JPEG_QUALITY, 75});
    runtime.latest_frame.jpeg = std::move(buffer);
    runtime.latest_frame.sequence += 1;
}

cv::Mat make_placeholder_frame(const CameraRuntime& runtime, const std::string& message) {
    cv::Mat frame(720, 1280, CV_8UC3, cv::Scalar(18, 20, 27));
    cv::rectangle(frame, cv::Rect(40, 40, 1200, 120), cv::Scalar(28, 30, 40), -1);
    cv::putText(
        frame,
        runtime.config.label.empty() ? runtime.config.id : runtime.config.label,
        cv::Point(60, 100),
        cv::FONT_HERSHEY_SIMPLEX,
        1.0,
        cv::Scalar(56, 189, 248),
        2,
        cv::LINE_AA
    );
    cv::putText(
        frame,
        message,
        cv::Point(60, 180),
        cv::FONT_HERSHEY_SIMPLEX,
        0.8,
        cv::Scalar(240, 240, 240),
        2,
        cv::LINE_AA
    );
    cv::putText(
        frame,
        runtime.config.rtsp_url,
        cv::Point(60, 240),
        cv::FONT_HERSHEY_SIMPLEX,
        0.7,
        cv::Scalar(140, 148, 160),
        2,
        cv::LINE_AA
    );
    return frame;
}

void draw_overlay(CameraRuntime& runtime, cv::Mat& frame) {
    const auto p1 = to_point(runtime.config.line, frame.cols, frame.rows, true);
    const auto p2 = to_point(runtime.config.line, frame.cols, frame.rows, false);
    cv::line(frame, p1, p2, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    cv::circle(frame, p1, 5, cv::Scalar(0, 255, 255), -1, cv::LINE_AA);
    cv::circle(frame, p2, 5, cv::Scalar(0, 255, 255), -1, cv::LINE_AA);

    cv::putText(
        frame,
        "Total: " + std::to_string(runtime.stats.total),
        cv::Point(24, 44),
        cv::FONT_HERSHEY_SIMPLEX,
        0.7,
        cv::Scalar(255, 255, 255),
        2,
        cv::LINE_AA
    );
    cv::putText(
        frame,
        "Right/Down: " + std::to_string(runtime.stats.right),
        cv::Point(24, 76),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(100, 255, 100),
        2,
        cv::LINE_AA
    );
    cv::putText(
        frame,
        "Left/Up: " + std::to_string(runtime.stats.left),
        cv::Point(24, 108),
        cv::FONT_HERSHEY_SIMPLEX,
        0.6,
        cv::Scalar(100, 200, 255),
        2,
        cv::LINE_AA
    );
}

int point_side(const LineConfig& line, const cv::Point& point, int width, int height) {
    const auto p1 = to_point(line, width, height, true);
    const auto p2 = to_point(line, width, height, false);
    const int value = (p2.x - p1.x) * (point.y - p1.y) - (p2.y - p1.y) * (point.x - p1.x);
    return value >= 0 ? 1 : -1;
}

void draw_detection(cv::Mat& frame, const Detection& detection, int track_id) {
    const cv::Rect box(
        cv::Point(static_cast<int>(detection.x1), static_cast<int>(detection.y1)),
        cv::Point(static_cast<int>(detection.x2), static_cast<int>(detection.y2))
    );
    cv::rectangle(frame, box, cv::Scalar(0, 200, 0), 2, cv::LINE_AA);
    cv::putText(
        frame,
        std::string(class_label(detection.class_id)) + " #" + std::to_string(track_id),
        cv::Point(box.x, std::max(16, box.y - 8)),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        cv::Scalar(0, 200, 0),
        2,
        cv::LINE_AA
    );
}

float center_distance(const cv::Point& lhs, const cv::Point& rhs) {
    const float dx = static_cast<float>(lhs.x - rhs.x);
    const float dy = static_cast<float>(lhs.y - rhs.y);
    return std::sqrt((dx * dx) + (dy * dy));
}

void update_tracks(
    std::vector<Track>& tracks,
    const std::vector<Detection>& detections,
    const LineConfig& line,
    CounterStats& stats,
    int frame_width,
    int frame_height,
    int& next_track_id
) {
    std::vector<bool> matched(detections.size(), false);

    for (auto& track : tracks) {
        float best_distance = 90.0F;
        int best_index = -1;

        for (std::size_t i = 0; i < detections.size(); ++i) {
            if (matched[i]) {
                continue;
            }
            const auto& detection = detections[i];
            const cv::Point center(
                static_cast<int>((detection.x1 + detection.x2) * 0.5F),
                static_cast<int>((detection.y1 + detection.y2) * 0.5F)
            );
            const float distance = center_distance(track.center, center);
            if (distance < best_distance) {
                best_distance = distance;
                best_index = static_cast<int>(i);
            }
        }

        if (best_index >= 0) {
            const auto& detection = detections[static_cast<std::size_t>(best_index)];
            const cv::Point center(
                static_cast<int>((detection.x1 + detection.x2) * 0.5F),
                static_cast<int>((detection.y1 + detection.y2) * 0.5F)
            );
            track.center = center;
            track.box = cv::Rect(
                cv::Point(static_cast<int>(detection.x1), static_cast<int>(detection.y1)),
                cv::Point(static_cast<int>(detection.x2), static_cast<int>(detection.y2))
            );
            track.frames_seen += 1;
            track.missed_frames = 0;

            const int new_side = point_side(line, center, frame_width, frame_height);
            if (track.frames_seen >= 3 && track.last_side != 0 && new_side != track.last_side && !track.counted) {
                track.counted = true;
                stats.total += 1;
                if (new_side == 1) {
                    stats.right += 1;
                } else {
                    stats.left += 1;
                }
            }
            track.last_side = new_side;
            matched[static_cast<std::size_t>(best_index)] = true;
        } else {
            track.missed_frames += 1;
        }
    }

    tracks.erase(
        std::remove_if(
            tracks.begin(),
            tracks.end(),
            [](const Track& track) { return track.missed_frames > 12; }
        ),
        tracks.end()
    );

    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (matched[i]) {
            continue;
        }
        const auto& detection = detections[i];
        const cv::Point center(
            static_cast<int>((detection.x1 + detection.x2) * 0.5F),
            static_cast<int>((detection.y1 + detection.y2) * 0.5F)
        );
        Track track;
        track.id = next_track_id++;
        track.center = center;
        track.box = cv::Rect(
            cv::Point(static_cast<int>(detection.x1), static_cast<int>(detection.y1)),
            cv::Point(static_cast<int>(detection.x2), static_cast<int>(detection.y2))
        );
        track.frames_seen = 1;
        track.last_side = point_side(line, center, frame_width, frame_height);
        tracks.push_back(track);
    }
}

bool open_capture(const std::string& source, cv::VideoCapture& capture) {
    if (source.size() == 1 && std::isdigit(source[0]) != 0) {
        return capture.open(source[0] - '0');
    }
    return capture.open(source, cv::CAP_FFMPEG);
}

}  // namespace

CounterEngine::CounterEngine(const AppConfig& app_config)
    : app_config_(app_config), backend_(std::make_shared<OpenVinoBackend>(app_config)) {}

void CounterEngine::run(const CameraRuntimePtr& runtime) const {
    std::cout
        << "[counter] starting camera id=" << runtime->config.id
        << " label=" << runtime->config.label
        << " source=" << runtime->config.rtsp_url
        << " model=" << app_config_.model_path
        << '\n';
    std::cout << "[counter] backend status: " << backend_->status() << '\n';

    while (runtime->running) {
        cv::VideoCapture capture;
        if (!open_capture(runtime->config.rtsp_url, capture)) {
            {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                runtime->online = false;
                runtime->reconnecting = true;
                runtime->last_error = "failed to open source";
                cv::Mat offline = make_placeholder_frame(*runtime, "RTSP tidak bisa dibuka. Reconnect 3 detik...");
                write_jpeg(*runtime, offline);
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            runtime->online = true;
            runtime->reconnecting = false;
            runtime->last_error.clear();
        }

        cv::Mat frame;
        std::uint64_t processed_frames = 0;
        std::vector<Track> tracks;
        int next_track_id = 1;

        while (runtime->running && capture.isOpened()) {
            if (!capture.read(frame) || frame.empty()) {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                runtime->online = false;
                runtime->reconnecting = true;
                runtime->last_error = "stream read failed";
                break;
            }

            processed_frames += 1;
            std::vector<Detection> detections;
            if (processed_frames % static_cast<std::uint64_t>(std::max(runtime->config.frame_skip, 1)) == 0U) {
                detections = backend_->infer(frame);
            }

            {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                if (!detections.empty()) {
                    update_tracks(
                        tracks,
                        detections,
                        runtime->config.line,
                        runtime->stats,
                        frame.cols,
                        frame.rows,
                        next_track_id
                    );
                } else {
                    update_tracks(
                        tracks,
                        {},
                        runtime->config.line,
                        runtime->stats,
                        frame.cols,
                        frame.rows,
                        next_track_id
                    );
                }

                for (const auto& detection : detections) {
                    int track_id = 0;
                    const cv::Point center(
                        static_cast<int>((detection.x1 + detection.x2) * 0.5F),
                        static_cast<int>((detection.y1 + detection.y2) * 0.5F)
                    );
                    for (const auto& track : tracks) {
                        if (center_distance(track.center, center) < 5.0F) {
                            track_id = track.id;
                            break;
                        }
                    }
                    draw_detection(frame, detection, track_id);
                }

                draw_overlay(*runtime, frame);
                write_jpeg(*runtime, frame);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        capture.release();
        if (runtime->running) {
            cv::Mat reconnect = make_placeholder_frame(*runtime, "Koneksi RTSP terputus. Mencoba reconnect...");
            {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                write_jpeg(*runtime, reconnect);
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        runtime->online = false;
        runtime->reconnecting = false;
    }
}

}  // namespace comvisplus
