#include "camera_manager.hpp"

#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace comvisplus {

namespace {

std::string make_camera_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream oss;
    oss << std::hex << (now & 0xffffffff);
    return oss.str();
}

std::string escape_json(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

void skip_ws(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        pos += 1;
    }
}

bool consume_char(const std::string& text, std::size_t& pos, const char expected) {
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] != expected) {
        return false;
    }
    pos += 1;
    return true;
}

bool parse_string_token(const std::string& text, std::size_t& pos, std::string& out) {
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }

    pos += 1;
    out.clear();
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return true;
        }
        if (ch == '\\' && pos < text.size()) {
            const char escaped = text[pos++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out += escaped;
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                default:
                    out += escaped;
                    break;
            }
            continue;
        }
        out += ch;
    }
    return false;
}

bool parse_number_token(const std::string& text, std::size_t& pos, double& out) {
    skip_ws(text, pos);
    const std::size_t start = pos;
    if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
        pos += 1;
    }
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        pos += 1;
    }
    if (pos < text.size() && text[pos] == '.') {
        pos += 1;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
            pos += 1;
        }
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        pos += 1;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
            pos += 1;
        }
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
            pos += 1;
        }
    }
    if (start == pos) {
        return false;
    }

    try {
        out = std::stod(text.substr(start, pos - start));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_float_array2(const std::string& text, std::size_t& pos, float& a, float& b) {
    if (!consume_char(text, pos, '[')) {
        return false;
    }

    double first = 0.0;
    double second = 0.0;
    if (!parse_number_token(text, pos, first)) {
        return false;
    }
    if (!consume_char(text, pos, ',')) {
        return false;
    }
    if (!parse_number_token(text, pos, second)) {
        return false;
    }
    if (!consume_char(text, pos, ']')) {
        return false;
    }

    a = static_cast<float>(first);
    b = static_cast<float>(second);
    return true;
}

std::string read_file_text(const std::string& path) {
    std::ifstream input(path);
    if (!input.good()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool parse_camera_object(const std::string& text, std::size_t& pos, CameraConfig& config) {
    if (!consume_char(text, pos, '{')) {
        return false;
    }

    bool first = true;
    std::string line_dir;
    float line_pos = 0.5F;
    bool has_line_p1 = false;
    bool has_line_p2 = false;

    while (true) {
        skip_ws(text, pos);
        if (pos < text.size() && text[pos] == '}') {
            pos += 1;
            break;
        }
        if (!first) {
            if (!consume_char(text, pos, ',')) {
                return false;
            }
        }
        first = false;

        std::string key;
        if (!parse_string_token(text, pos, key) || !consume_char(text, pos, ':')) {
            return false;
        }

        if (key == "url") {
            if (!parse_string_token(text, pos, config.rtsp_url)) {
                return false;
            }
        } else if (key == "label") {
            if (!parse_string_token(text, pos, config.label)) {
                return false;
            }
        } else if (key == "frame_skip") {
            double number = 0.0;
            if (!parse_number_token(text, pos, number)) {
                return false;
            }
            config.frame_skip = static_cast<int>(number);
        } else if (key == "inference_size") {
            double number = 0.0;
            if (!parse_number_token(text, pos, number)) {
                return false;
            }
            config.inference_size = static_cast<int>(number);
        } else if (key == "line_pos") {
            double number = 0.0;
            if (!parse_number_token(text, pos, number)) {
                return false;
            }
            line_pos = static_cast<float>(number);
        } else if (key == "line_dir") {
            if (!parse_string_token(text, pos, line_dir)) {
                return false;
            }
        } else if (key == "line_p1") {
            if (!parse_float_array2(text, pos, config.line.x1, config.line.y1)) {
                return false;
            }
            has_line_p1 = true;
        } else if (key == "line_p2") {
            if (!parse_float_array2(text, pos, config.line.x2, config.line.y2)) {
                return false;
            }
            has_line_p2 = true;
        } else {
            return false;
        }
    }

    if (!(has_line_p1 && has_line_p2)) {
        if (line_dir == "horizontal") {
            config.line = {0.0F, line_pos, 1.0F, line_pos};
        } else {
            config.line = {line_pos, 0.0F, line_pos, 1.0F};
        }
    }

    if (config.frame_skip <= 0) {
        config.frame_skip = 2;
    }
    if (config.inference_size <= 0) {
        config.inference_size = 320;
    }

    return true;
}

bool parse_cameras_json(const std::string& text, std::vector<CameraConfig>& configs) {
    std::size_t pos = 0;
    if (!consume_char(text, pos, '[')) {
        return false;
    }

    bool first = true;
    while (true) {
        skip_ws(text, pos);
        if (pos < text.size() && text[pos] == ']') {
            pos += 1;
            break;
        }
        if (!first) {
            if (!consume_char(text, pos, ',')) {
                return false;
            }
        }
        first = false;

        CameraConfig config;
        if (!parse_camera_object(text, pos, config)) {
            return false;
        }
        configs.push_back(config);
    }

    return true;
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
    const std::string text = read_file_text(app_config_.cameras_file);
    if (text.empty()) {
        return false;
    }

    std::vector<CameraConfig> configs;
    if (!parse_cameras_json(text, configs)) {
        return false;
    }

    int loaded = 0;
    for (const auto& config : configs) {
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
    std::ofstream output(app_config_.cameras_file, std::ios::trunc);
    if (!output.good()) {
        return false;
    }

    output << "[\n";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool first = true;
        for (const auto& item : cameras_) {
            const auto& config = item.second->config;
            if (!first) {
                output << ",\n";
            }
            first = false;
            output << "  {\n";
            output << "    \"url\": \"" << escape_json(config.rtsp_url) << "\",\n";
            output << "    \"label\": \"" << escape_json(config.label) << "\",\n";
            output << "    \"line_p1\": [" << config.line.x1 << ", " << config.line.y1 << "],\n";
            output << "    \"line_p2\": [" << config.line.x2 << ", " << config.line.y2 << "],\n";
            output << "    \"frame_skip\": " << config.frame_skip << ",\n";
            output << "    \"inference_size\": " << config.inference_size << "\n";
            output << "  }";
        }
    }
    output << "\n]\n";
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
