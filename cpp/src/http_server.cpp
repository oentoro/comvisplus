#include "http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::string json_escape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (const char ch : input) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += ch; break;
        }
    }
    return output;
}

std::string http_ok(const std::string& content_type, const std::string& body) {
    std::ostringstream out;
    out
        << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return out.str();
}

std::string http_not_found() {
    return
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":\"not found\"}";
}

std::string http_method_not_allowed() {
    return
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":\"method not allowed\"}";
}

std::string root_page() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Comvisplus Native</title>
  <style>
    body { background:#111827; color:#e5e7eb; font-family:system-ui,sans-serif; margin:0; padding:24px; }
    h1 { margin:0 0 20px; color:#38bdf8; }
    #grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(460px,1fr)); gap:16px; }
    .card { background:#1f2937; border:1px solid #374151; border-radius:12px; overflow:hidden; }
    .head { padding:12px 14px; border-bottom:1px solid #374151; display:flex; justify-content:space-between; gap:12px; }
    .feed { width:100%; aspect-ratio:16/9; object-fit:contain; background:#000; display:block; }
    .stats { display:flex; gap:12px; padding:12px 14px; font-size:14px; color:#9ca3af; }
    .pill { color:#fff; font-weight:700; }
  </style>
</head>
<body>
  <h1>Comvisplus Native</h1>
  <div id="grid"></div>
  <script>
    async function load() {
      const cameras = await fetch('/cameras').then(r => r.json()).catch(() => []);
      const grid = document.getElementById('grid');
      grid.innerHTML = '';
      cameras.forEach(cam => {
        const card = document.createElement('div');
        card.className = 'card';
        card.innerHTML = `
          <div class="head">
            <div>
              <div style="font-weight:700">${cam.label}</div>
              <div style="font-size:12px;color:#9ca3af">${cam.url}</div>
            </div>
            <div style="font-size:12px;color:${cam.online ? '#4ade80' : '#f87171'}">${cam.online ? 'online' : 'offline'}</div>
          </div>
          <img class="feed" src="/feed/${cam.id}" alt="">
          <div class="stats">
            <span>Total <span class="pill">${cam.total}</span></span>
            <span>Right <span class="pill">${cam.right}</span></span>
            <span>Left <span class="pill">${cam.left}</span></span>
          </div>`;
        grid.appendChild(card);
      });
    }
    load();
    setInterval(load, 2000);
  </script>
</body>
</html>)HTML";
}

}  // namespace
namespace comvisplus {

HttpServer::HttpServer(const AppConfig& app_config, std::shared_ptr<CameraManager> manager)
    : app_config_(app_config), manager_(std::move(manager)) {}

void HttpServer::run() {
    print_routes();
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("failed to create socket");
    }

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(app_config_.port);
    address.sin_addr.s_addr = app_config_.host == "0.0.0.0"
        ? htonl(INADDR_ANY)
        : inet_addr(app_config_.host.c_str());

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(server_fd);
        throw std::runtime_error("failed to bind socket");
    }
    if (::listen(server_fd, 16) < 0) {
        ::close(server_fd);
        throw std::runtime_error("failed to listen");
    }

    std::cout << "[web] listening on http://" << app_config_.host << ':' << app_config_.port << '\n';
    accept_loop(server_fd);
    ::close(server_fd);
}

void HttpServer::accept_loop(int server_fd) {
    while (running_) {
        sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);
        const int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[web] accept failed: " << std::strerror(errno) << '\n';
            continue;
        }
        std::thread(&HttpServer::handle_client, this, client_fd).detach();
    }
}

void HttpServer::handle_client(int client_fd) const {
    char buffer[4096];
    const ssize_t bytes_read = ::recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        ::close(client_fd);
        return;
    }
    buffer[bytes_read] = '\0';

    std::istringstream request_stream(std::string(buffer, static_cast<std::size_t>(bytes_read)));
    std::string method;
    std::string path;
    std::string version;
    request_stream >> method >> path >> version;

    if (method != "GET") {
        const auto response = http_method_not_allowed();
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/") {
        const auto response = http_ok("text/html; charset=utf-8", root_page());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path == "/cameras") {
        std::ostringstream body;
        body << '[';
        bool first = true;
        for (const auto& runtime : manager_->list_runtimes()) {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            if (!first) {
                body << ',';
            }
            first = false;
            body
                << '{'
                << "\"id\":\"" << json_escape(runtime->config.id) << "\","
                << "\"label\":\"" << json_escape(runtime->config.label) << "\","
                << "\"url\":\"" << json_escape(runtime->config.rtsp_url) << "\","
                << "\"online\":" << (runtime->online ? "true" : "false") << ','
                << "\"reconnecting\":" << (runtime->reconnecting ? "true" : "false") << ','
                << "\"error\":\"" << json_escape(runtime->last_error) << "\","
                << "\"total\":" << runtime->stats.total << ','
                << "\"right\":" << runtime->stats.right << ','
                << "\"left\":" << runtime->stats.left
                << '}';
        }
        body << ']';
        const auto response = http_ok("application/json", body.str());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.rfind("/stats/", 0) == 0) {
        const std::string camera_id = path.substr(std::strlen("/stats/"));
        const auto runtime = manager_->get(camera_id);
        if (runtime == nullptr) {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        std::ostringstream body;
        {
            std::lock_guard<std::mutex> lock(runtime->mutex);
            body
                << '{'
                << "\"id\":\"" << json_escape(runtime->config.id) << "\","
                << "\"online\":" << (runtime->online ? "true" : "false") << ','
                << "\"reconnecting\":" << (runtime->reconnecting ? "true" : "false") << ','
                << "\"total\":" << runtime->stats.total << ','
                << "\"right\":" << runtime->stats.right << ','
                << "\"left\":" << runtime->stats.left
                << '}';
        }
        const auto response = http_ok("application/json", body.str());
        ::send(client_fd, response.data(), response.size(), 0);
        ::close(client_fd);
        return;
    }

    if (path.rfind("/feed/", 0) == 0) {
        const std::string camera_id = path.substr(std::strlen("/feed/"));
        const auto runtime = manager_->get(camera_id);
        if (runtime == nullptr) {
            const auto response = http_not_found();
            ::send(client_fd, response.data(), response.size(), 0);
            ::close(client_fd);
            return;
        }

        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
        ::send(client_fd, header.data(), header.size(), 0);

        std::uint64_t last_sequence = 0;
        while (runtime->running) {
            std::vector<unsigned char> jpeg;
            std::uint64_t sequence = 0;
            {
                std::lock_guard<std::mutex> lock(runtime->mutex);
                sequence = runtime->latest_frame.sequence;
                if (sequence != last_sequence) {
                    jpeg = runtime->latest_frame.jpeg;
                    last_sequence = sequence;
                }
            }

            if (!jpeg.empty()) {
                std::ostringstream part_header;
                part_header
                    << "--frame\r\n"
                    << "Content-Type: image/jpeg\r\n"
                    << "Content-Length: " << jpeg.size() << "\r\n\r\n";
                const std::string part_header_str = part_header.str();
                if (::send(client_fd, part_header_str.data(), part_header_str.size(), 0) <= 0) {
                    break;
                }
                if (::send(client_fd, reinterpret_cast<const char*>(jpeg.data()), jpeg.size(), 0) <= 0) {
                    break;
                }
                if (::send(client_fd, "\r\n", 2, 0) <= 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        ::close(client_fd);
        return;
    }

    const auto response = http_not_found();
    ::send(client_fd, response.data(), response.size(), 0);
    ::close(client_fd);
}

void HttpServer::print_routes() const {
    std::cout << "[routes] GET /\n";
    std::cout << "[routes] GET /cameras\n";
    std::cout << "[routes] GET /feed/{id}\n";
    std::cout << "[routes] GET /stats/{id}\n";
}

}  // namespace comvisplus
