#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>

// V4L2
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

// Сокеты для HTTP
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sstream>

// OpenCV & ZXing
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <ZXing/ReadBarcode.h>
#include <ZXing/ImageView.h>

const std::string LOG_FILE = "qr_scanner.log";

// --- НАСТРОЙКИ СЕРВЕРА ---
const std::string SERVER_IP = "172.32.0.100"; // Или 172.32.0.100 для USB RNDIS
const int SERVER_PORT = 8080;
const std::string SERVER_PATH = "/api/validate-qr";

// --- НАСТРОЙКИ ПОВЕДЕНИЯ ---
const int COOLDOWN_SECONDS = 4; 

std::mutex log_mutex;

void log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string time_str = std::ctime(&now);
    if (!time_str.empty() && time_str.back() == '\n') time_str.pop_back();

    std::string formatted = "[" + time_str + "] " + message;
    std::cout << formatted << std::endl;
    std::ofstream ofs(LOG_FILE, std::ios_base::app);
    if (ofs.is_open()) ofs << formatted << "\n";
}

std::string escape_json(const std::string& s) {
    std::string o;
    for (auto c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    return o;
}

bool send_to_server(const std::string& qr_text) {
    log("Отправка токена на сервер: " + SERVER_IP + ":" + std::to_string(SERVER_PORT) + SERVER_PATH);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log("ОШИБКА СЕТИ: Не удалось создать сокет");
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP.c_str(), &server_addr.sin_addr) <= 0) {
        log("ОШИБКА СЕТИ: Неверный адрес сервера");
        close(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log("ОШИБКА СЕТИ: Не удалось подключиться к серверу");
        close(sock);
        return false;
    }

    std::string body = "{\"qr_data\": \"" + escape_json(qr_text) + "\"}";
    std::stringstream request;
    request << "POST " << SERVER_PATH << " HTTP/1.1\r\n"
            << "Host: " << SERVER_IP << ":" << SERVER_PORT << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.length() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;

    std::string req_str = request.str();
    
    if (send(sock, req_str.c_str(), req_str.length(), 0) < 0) {
        log("ОШИБКА СЕТИ: Ошибка отправки данных");
        close(sock);
        return false;
    }

    std::string response = "";
    char buffer[1024];
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    close(sock);

    if (response.empty()) {
        log("ОШИБКА СЕТИ: Пустой ответ от сервера");
        return false;
    }

    size_t body_start = response.find("\r\n\r\n");
    std::string response_body = (body_start != std::string::npos) ? response.substr(body_start + 4) : response;

    if (response_body.find("\"status\": \"success\"") != std::string::npos || 
        response_body.find("\"status\":\"success\"") != std::string::npos) {
        log("Сервер ответил: УСПЕХ");
        return true;
    } else {
        log("Сервер ответил: ОТКАЗ");
        return false;
    }
}

// Класс захвата камеры с автоподбором формата
class V4L2Camera {
    int fd;
    void* buffer_start;
    size_t buffer_length;
    int width, height;
    uint32_t pixel_format;
    int num_planes;

public:
    V4L2Camera(const char* dev_name = "/dev/video12", int w = 640, int h = 480) 
        : fd(-1), buffer_start(nullptr), buffer_length(0), width(w), height(h), 
          pixel_format(0), num_planes(0) {
        
        fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) throw std::runtime_error("Не удалось открыть камеру " + std::string(dev_name));

        // Пробуем форматы в порядке приоритета
        struct { uint32_t fmt; int planes; const char* name; } formats_to_try[] = {
            { V4L2_PIX_FMT_NV12, 2, "NV12 (2 плоскости)" },
            { V4L2_PIX_FMT_NV12, 1, "NV12 (1 плоскость)" },
            { V4L2_PIX_FMT_UYVY, 1, "UYVY" },
            { V4L2_PIX_FMT_YUYV, 1, "YUYV" }
        };

        bool format_set = false;
        for (auto& f : formats_to_try) {
            struct v4l2_format fmt = {};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            fmt.fmt.pix_mp.width = width;
            fmt.fmt.pix_mp.height = height;
            fmt.fmt.pix_mp.pixelformat = f.fmt;
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            fmt.fmt.pix_mp.num_planes = f.planes;

            if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0) {
                if (fmt.fmt.pix_mp.pixelformat == f.fmt) {
                    pixel_format = f.fmt;
                    num_planes = f.planes;
                    width = fmt.fmt.pix_mp.width;
                    height = fmt.fmt.pix_mp.height;
                    format_set = true;
                    
                    log("Камера: установлен формат " + std::string(f.name) + " " + 
                        std::to_string(width) + "x" + std::to_string(height));
                    break;
                }
            }
        }

        if (!format_set) {
            throw std::runtime_error("Не удалось установить поддерживаемый формат на " + std::string(dev_name));
        }

        struct v4l2_requestbuffers req = {};
        req.count = 1;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) throw std::runtime_error("VIDIOC_REQBUFS ошибка");

        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = 0;
        
        struct v4l2_plane planes[2] = {};
        buf.length = num_planes;
        buf.m.planes = planes;
        
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) throw std::runtime_error("VIDIOC_QUERYBUF ошибка");

        // Мапим первую плоскость (для NV12 это Y-канал)
        buffer_length = planes[0].length;
        buffer_start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
        if (buffer_start == MAP_FAILED) throw std::runtime_error("mmap ошибка");

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) throw std::runtime_error("VIDIOC_QBUF ошибка");

        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) throw std::runtime_error("VIDIOC_STREAMON ошибка");
    }

    ~V4L2Camera() {
        if (fd >= 0) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl(fd, VIDIOC_STREAMOFF, &type);
            if (buffer_start && buffer_start != MAP_FAILED) munmap(buffer_start, buffer_length);
            close(fd);
        }
    }

    bool grab_gray_frame(cv::Mat& gray_mat) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = 0;
        
        struct v4l2_plane planes[2] = {};
        buf.length = num_planes;
        buf.m.planes = planes;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {1, 0};

        int r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) return false;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) return false;

        if (pixel_format == V4L2_PIX_FMT_NV12) {
            // Для NV12 первая плоскость - это чистый Y-канал (Grayscale)
            gray_mat = cv::Mat(height, width, CV_8UC1, buffer_start).clone();
        } else {
            // Для UYVY/YUYV извлекаем Y-компоненту
            cv::Mat raw_mat(height, width, CV_8UC2, buffer_start);
            gray_mat.create(height, width, CV_8UC1);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    gray_mat.at<uchar>(y, x) = raw_mat.at<uchar>(y, x * 2 + 1);
                }
            }
        }

        ioctl(fd, VIDIOC_QBUF, &buf);
        return true;
    }

    int getWidth() const { return width; }
    int getHeight() const { return height; }
};

int main() {
    log("=== Запуск системы контроля доступа (Непрерывный режим) ===");

    try {
        V4L2Camera cam("/dev/video12", 640, 480);
        cv::Mat gray_frame;

        auto cooldown_end_time = std::chrono::steady_clock::now();

        log("Ожидание появления QR-кода в кадре...");

        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (now < cooldown_end_time) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (!cam.grab_gray_frame(gray_frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            ZXing::ImageView image(gray_frame.data, gray_frame.cols, gray_frame.rows, ZXing::ImageFormat::Lum);
            
            ZXing::ReaderOptions options;
            options.setFormats(ZXing::BarcodeFormat::QRCode);
            options.setTryHarder(true); 
            
            auto barcodes = ZXing::ReadBarcodes(image, options);

            if (!barcodes.empty()) {
                std::string current_qr = barcodes.front().text();
                log("QR-код считан: " + current_qr);

                bool is_allowed = send_to_server(current_qr);

                if (is_allowed) {
                    log("========================================");
                    log("✅ ДОСТУП РАЗРЕШЕН (Здесь будет зеленый диод)");
                    log("========================================");
                } else {
                    log("========================================");
                    log("❌ ДОСТУП ЗАПРЕЩЕН (Здесь будет красный диод)");
                    log("========================================");
                }

                cooldown_end_time = std::chrono::steady_clock::now() + std::chrono::seconds(COOLDOWN_SECONDS);
                log("Активирована пауза на " + std::to_string(COOLDOWN_SECONDS) + " сек. перед следующим сканированием...");
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30)); 
        }
    } catch (const std::exception& e) {
        log("ФАТАЛЬНАЯ ОШИБКА: " + std::string(e.what()));
    }

    return 0;
}