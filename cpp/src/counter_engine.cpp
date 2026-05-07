#include "counter_engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace comvisplus {

namespace {

constexpr float kHighConfidenceThreshold = 0.5F;
constexpr float kLowConfidenceThreshold = 0.15F;
constexpr float kTrackIouThreshold = 0.3F;
constexpr int kMaxLostFrames = 20;

enum class TrackState {
    Tracked,
    Lost,
};

struct Track {
    int id = 0;
    cv::Point center;
    cv::Rect box;
    cv::Point2f velocity {0.0F, 0.0F};
    float confidence = 0.0F;
    int frames_seen = 0;
    int missed_frames = 0;
    bool counted = false;
    int last_side = 0;
    TrackState state = TrackState::Tracked;
};

struct CrossingEvent {
    int track_id = 0;
    std::string direction;
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

std::string sanitize_filename_part(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (const char ch : input) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
            output += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        } else if (ch == '-' || ch == '_') {
            output += ch;
        } else {
            output += '_';
        }
    }
    if (output.empty()) {
        output = "camera";
    }
    return output;
}

std::string make_screenshot_filename(const CameraRuntime& runtime, const CrossingEvent& event) {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto seconds = clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm local_tm {};
#if defined(_WIN32)
    localtime_s(&local_tm, &seconds);
#else
    localtime_r(&seconds, &local_tm);
#endif

    std::ostringstream name;
    name
        << sanitize_filename_part(runtime.config.label.empty() ? runtime.config.id : runtime.config.label)
        << '_'
        << std::put_time(&local_tm, "%Y%m%d_%H%M%S")
        << '_'
        << std::setw(3) << std::setfill('0') << millis
        << '_'
        << event.direction
        << "_track" << event.track_id
        << ".jpg";
    return name.str();
}

void save_crossing_screenshots(
    const AppConfig& app_config,
    const CameraRuntime& runtime,
    const cv::Mat& frame,
    const std::vector<CrossingEvent>& events
) {
    if (events.empty() || frame.empty()) {
        return;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(app_config.screenshots_dir, ec);

    for (const auto& event : events) {
        const fs::path output_path = fs::path(app_config.screenshots_dir) / make_screenshot_filename(runtime, event);
        cv::imwrite(output_path.string(), frame);
    }
}

cv::Rect detection_rect(const Detection& detection) {
    const int x1 = static_cast<int>(std::round(detection.x1));
    const int y1 = static_cast<int>(std::round(detection.y1));
    const int x2 = static_cast<int>(std::round(detection.x2));
    const int y2 = static_cast<int>(std::round(detection.y2));
    return cv::Rect(cv::Point(x1, y1), cv::Point(x2, y2));
}

cv::Point detection_center(const Detection& detection) {
    return cv::Point(
        static_cast<int>(std::round((detection.x1 + detection.x2) * 0.5F)),
        static_cast<int>(std::round((detection.y1 + detection.y2) * 0.5F))
    );
}

float rect_iou(const cv::Rect& lhs, const cv::Rect& rhs) {
    const cv::Rect inter = lhs & rhs;
    const float inter_area = static_cast<float>(inter.area());
    const float union_area = static_cast<float>(lhs.area() + rhs.area() - inter.area());
    if (union_area <= 0.0F) {
        return 0.0F;
    }
    return inter_area / union_area;
}

cv::Rect predict_box(const Track& track) {
    const int dx = static_cast<int>(std::round(track.velocity.x));
    const int dy = static_cast<int>(std::round(track.velocity.y));
    return cv::Rect(track.box.x + dx, track.box.y + dy, track.box.width, track.box.height);
}

void apply_detection_to_track(
    Track& track,
    const Detection& detection,
    const LineConfig& line,
    CounterStats& stats,
    int frame_width,
    int frame_height,
    std::vector<CrossingEvent>& events
) {
    const cv::Point previous_center = track.center;
    const cv::Point center = detection_center(detection);
    track.center = center;
    track.box = detection_rect(detection);
    track.velocity = cv::Point2f(
        (track.velocity.x * 0.7F) + (static_cast<float>(center.x - previous_center.x) * 0.3F),
        (track.velocity.y * 0.7F) + (static_cast<float>(center.y - previous_center.y) * 0.3F)
    );
    track.confidence = detection.confidence;
    track.frames_seen += 1;
    track.missed_frames = 0;
    track.state = TrackState::Tracked;

    const int new_side = point_side(line, center, frame_width, frame_height);
    if (track.frames_seen >= 3 && track.last_side != 0 && new_side != track.last_side && !track.counted) {
        track.counted = true;
        stats.total += 1;
        if (new_side == 1) {
            stats.right += 1;
            events.push_back({track.id, "right"});
        } else {
            stats.left += 1;
            events.push_back({track.id, "left"});
        }
    }
    track.last_side = new_side;
}

void match_tracks_to_detections(
    std::vector<Track>& tracks,
    const std::vector<Detection>& detections,
    std::vector<bool>& matched_tracks,
    std::vector<bool>& matched_detections,
    const LineConfig& line,
    CounterStats& stats,
    int frame_width,
    int frame_height,
    float min_iou,
    std::vector<CrossingEvent>& events
) {
    while (true) {
        float best_iou = min_iou;
        int best_track = -1;
        int best_detection = -1;

        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            if (matched_tracks[ti]) {
                continue;
            }
            for (std::size_t di = 0; di < detections.size(); ++di) {
                if (matched_detections[di]) {
                    continue;
                }
                const float iou = rect_iou(predict_box(tracks[ti]), detection_rect(detections[di]));
                if (iou > best_iou) {
                    best_iou = iou;
                    best_track = static_cast<int>(ti);
                    best_detection = static_cast<int>(di);
                }
            }
        }

        if (best_track < 0 || best_detection < 0) {
            break;
        }

        matched_tracks[static_cast<std::size_t>(best_track)] = true;
        matched_detections[static_cast<std::size_t>(best_detection)] = true;
        apply_detection_to_track(
            tracks[static_cast<std::size_t>(best_track)],
            detections[static_cast<std::size_t>(best_detection)],
            line,
            stats,
            frame_width,
            frame_height,
            events
        );
    }
}

void update_tracks(
    std::vector<Track>& tracks,
    const std::vector<Detection>& detections,
    const LineConfig& line,
    CounterStats& stats,
    int frame_width,
    int frame_height,
    int& next_track_id,
    std::vector<CrossingEvent>& events
) {
    std::vector<Detection> high_confidence;
    std::vector<Detection> low_confidence;
    high_confidence.reserve(detections.size());
    low_confidence.reserve(detections.size());

    for (const auto& detection : detections) {
        if (detection.confidence >= kHighConfidenceThreshold) {
            high_confidence.push_back(detection);
        } else if (detection.confidence >= kLowConfidenceThreshold) {
            low_confidence.push_back(detection);
        }
    }

    std::vector<bool> matched_tracks(tracks.size(), false);
    std::vector<bool> matched_high(high_confidence.size(), false);
    match_tracks_to_detections(
        tracks,
        high_confidence,
        matched_tracks,
        matched_high,
        line,
        stats,
        frame_width,
        frame_height,
        kTrackIouThreshold,
        events
    );

    std::vector<bool> matched_low(low_confidence.size(), false);
    match_tracks_to_detections(
        tracks,
        low_confidence,
        matched_tracks,
        matched_low,
        line,
        stats,
        frame_width,
        frame_height,
        kTrackIouThreshold * 0.8F,
        events
    );

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (matched_tracks[i]) {
            continue;
        }
        tracks[i].center.x += static_cast<int>(std::round(tracks[i].velocity.x));
        tracks[i].center.y += static_cast<int>(std::round(tracks[i].velocity.y));
        tracks[i].box = predict_box(tracks[i]);
        tracks[i].velocity *= 0.85F;
        tracks[i].missed_frames += 1;
        tracks[i].state = TrackState::Lost;
    }

    tracks.erase(
        std::remove_if(
            tracks.begin(),
            tracks.end(),
            [](const Track& track) { return track.missed_frames > kMaxLostFrames; }
        ),
        tracks.end()
    );

    for (std::size_t i = 0; i < high_confidence.size(); ++i) {
        if (matched_high[i]) {
            continue;
        }
        const auto& detection = high_confidence[i];
        Track track;
        track.id = next_track_id++;
        track.center = detection_center(detection);
        track.box = detection_rect(detection);
        track.confidence = detection.confidence;
        track.frames_seen = 1;
        track.last_side = point_side(line, track.center, frame_width, frame_height);
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
            std::vector<CrossingEvent> crossing_events;
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
                        next_track_id,
                        crossing_events
                    );
                } else {
                    update_tracks(
                        tracks,
                        {},
                        runtime->config.line,
                        runtime->stats,
                        frame.cols,
                        frame.rows,
                        next_track_id,
                        crossing_events
                    );
                }

                for (const auto& track : tracks) {
                    if (track.state != TrackState::Tracked) {
                        continue;
                    }
                    Detection detection;
                    detection.x1 = static_cast<float>(track.box.x);
                    detection.y1 = static_cast<float>(track.box.y);
                    detection.x2 = static_cast<float>(track.box.x + track.box.width);
                    detection.y2 = static_cast<float>(track.box.y + track.box.height);
                    detection.confidence = track.confidence;
                    detection.class_id = 0;
                    draw_detection(frame, detection, track.id);
                }

                draw_overlay(*runtime, frame);
                save_crossing_screenshots(app_config_, *runtime, frame, crossing_events);
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
