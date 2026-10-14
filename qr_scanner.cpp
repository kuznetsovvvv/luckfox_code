#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

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
#include <ReadBarcode.h>
#include <ImageView.h>

const std::string LOG_FILE = "qr_scanner.log";
const std::string SERVER_IP = "172.32.0.100"; 
const int SERVER_PORT = 8080;
const std::string SERVER_PATH_QR = "/api/validate-qr";
const std::string SERVER_PATH_PHRASE = "/api/phrase_validate";

const int COOLDOWN_SECONDS = 4; 

std::mutex log_mutex;
std::atomic<bool> g_running{true};
std::atomic<bool> g_capture_requested{false};

// Структура для хранения результата проверки
struct ValidationResult {
    bool is_valid;
    std::string user_name;
    std::string message;
};

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

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
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

// --- Легковесный парсер JSON (ищет значение строкового ключа) ---
std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\":";
    size_t pos = json.find(search_key);
    if (pos == std::string::npos) return "";
    
    pos += search_key.length();
    // Пропускаем пробелы после двоеточия
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    
    if (pos >= json.length() || json[pos] != '"') return ""; // Если это не строка
    
    pos++; // Пропускаем открывающую кавычку
    size_t end_pos = json.find('"', pos);
    if (end_pos == std::string::npos) return "";
    
    return json.substr(pos, end_pos - pos);
}

// --- Base64 кодировщик ---
static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64_encode(const uchar* data, size_t len) {
    std::string ret;
    int i = 0, j = 0;
    uchar char_array_3[3], char_array_4[4];
    while (len--) {
        char_array_3[i++] = *(data++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; j < (i + 1); j++) ret += base64_chars[char_array_4[j]];
        while((i++ < 3)) ret += '=';
    }
    return ret;
}

// --- Мини-энкодер BMP (Grayscale) ---
std::vector<uchar> encodeGrayscaleBMP(const cv::Mat& gray) {
    int width = gray.cols;
    int height = gray.rows;
    int rowSize = (width + 3) & ~3;
    int fileSize = 54 + 256 * 4 + rowSize * height;

    std::vector<uchar> bmp(fileSize, 0);
    
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = fileSize & 0xFF; bmp[3] = (fileSize >> 8) & 0xFF;
    bmp[4] = (fileSize >> 16) & 0xFF; bmp[5] = (fileSize >> 24) & 0xFF;
    bmp[10] = 54 + 256 * 4;

    bmp[14] = 40;
    bmp[18] = width & 0xFF; bmp[19] = (width >> 8) & 0xFF;
    bmp[20] = (width >> 16) & 0xFF; bmp[21] = (width >> 24) & 0xFF;
    bmp[22] = height & 0xFF; bmp[23] = (height >> 8) & 0xFF;
    bmp[24] = (height >> 16) & 0xFF; bmp[25] = (height >> 24) & 0xFF;
    bmp[26] = 1; bmp[27] = 0;
    bmp[28] = 8; bmp[29] = 0;

    for (int i = 0; i < 256; i++) {
        bmp[54 + i * 4] = i;
        bmp[54 + i * 4 + 1] = i;
        bmp[54 + i * 4 + 2] = i;
        bmp[54 + i * 4 + 3] = 0;
    }

    int pixelOffset = 54 + 1024;
    for (int y = 0; y < height; y++) {
        std::memcpy(&bmp[pixelOffset + (height - 1 - y) * rowSize], gray.ptr(y), width);
    }
    return bmp;
}

// --- Отправка ФОТО на сервер (теперь возвращает ValidationResult) ---
ValidationResult send_photo_to_server(const cv::Mat& gray_frame) {
    ValidationResult result = {false, "", ""};
    
    log("Кодирование кадра в BMP (Grayscale)...");
    std::vector<uchar> bmp_data = encodeGrayscaleBMP(gray_frame);
    log("Размер BMP: " + std::to_string(bmp_data.size()) + " байт");

    log("Кодирование в Base64...");
    std::string base64_image = base64_encode(bmp_data.data(), bmp_data.size());

    log("Отправка на " + SERVER_IP + ":" + std::to_string(SERVER_PORT) + SERVER_PATH_PHRASE);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { log("ОШИБКА СЕТИ: Не удалось создать сокет"); return result; }

    struct timeval tv; tv.tv_sec = 70; tv.tv_usec = 0; 
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP.c_str(), &server_addr.sin_addr) <= 0) { close(sock); return result; }
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { close(sock); return result; }

    std::string body = "{\"image\": \"" + base64_image + "\"}";
    std::stringstream request;
    request << "POST " << SERVER_PATH_PHRASE << " HTTP/1.1\r\n"
            << "Host: " << SERVER_IP << ":" << SERVER_PORT << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.length() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;

    std::string req_str = request.str();
    size_t total_sent = 0;
    while (total_sent < req_str.length()) {
        ssize_t sent = send(sock, req_str.c_str() + total_sent, req_str.length() - total_sent, 0);
        if (sent < 0) { log("ОШИБКА СЕТИ: Ошибка отправки"); close(sock); return result; }
        total_sent += sent;
    }

    std::string response = "";
    char buffer[1024];
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    close(sock);

    size_t body_start = response.find("\r\n\r\n");
    std::string response_body = (body_start != std::string::npos) ? response.substr(body_start + 4) : response;

    if (response_body.find("\"valid\":true") != std::string::npos || response_body.find("\"valid\": true") != std::string::npos) {
        result.is_valid = true;
        result.user_name = extract_json_string(response_body, "user_name");
        result.message = extract_json_string(response_body, "message");
        log("Сервер ответил: УСПЕХ");
    } else {
        result.is_valid = false;
        result.message = extract_json_string(response_body, "message");
        log("Сервер ответил: ОТКАЗ");
    }
    
    return result;
}

// --- Отправка QR на сервер ---
bool send_to_server(const std::string& qr_text) {
    std::string clean_qr = trim(qr_text);
    log("Отправка QR на сервер...");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP.c_str(), &server_addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { close(sock); return false; }

    std::string body = "{\"token\": \"" + escape_json(clean_qr) + "\"}";
    std::stringstream request;
    request << "POST " << SERVER_PATH_QR << " HTTP/1.1\r\n"
            << "Host: " << SERVER_IP << ":" << SERVER_PORT << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.length() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;

    std::string req_str = request.str();
    send(sock, req_str.c_str(), req_str.length(), 0);

    std::string response = "";
    char buffer[1024];
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    close(sock);

    return (response.find("\"valid\":true") != std::string::npos || response.find("\"valid\": true") != std::string::npos);
}

// --- Фоновый поток консоли ---
void console_input_loop() {
    std::string cmd;
    while (g_running) {
        std::cout << "\n[УПРАВЛЕНИЕ] Введите 'photo' для отправки кадра или 'exit' для выхода:\n> " << std::flush;
        if (std::getline(std::cin, cmd)) {
            cmd = trim(cmd);
            if (cmd == "exit" || cmd == "quit") {
                log("Завершение работы...");
                g_running = false;
                break;
            } else if (cmd == "photo" || cmd == "phrase") {
                log(">>> ЗАПРОШЕНА ОТПРАВКА КАДРА НА СЕРВЕР <<<");
                g_capture_requested = true;
            }
        }
    }
}

// --- Класс камеры ---
class V4L2Camera {
    int fd; void* buffer_start; size_t buffer_length; int width, height;
    uint32_t pixel_format; int num_planes;
public:
    V4L2Camera(const char* dev_name = "/dev/video12", int w = 640, int h = 480) 
        : fd(-1), buffer_start(nullptr), buffer_length(0), width(w), height(h), pixel_format(0), num_planes(0) {
        fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
        if (fd < 0) throw std::runtime_error("Не удалось открыть камеру");

        struct { uint32_t fmt; int planes; const char* name; } formats[] = {
            { V4L2_PIX_FMT_NV12, 2, "NV12 (2 пл.)" }, { V4L2_PIX_FMT_NV12, 1, "NV12 (1 пл.)" },
            { V4L2_PIX_FMT_UYVY, 1, "UYVY" }, { V4L2_PIX_FMT_YUYV, 1, "YUYV" }
        };

        bool format_set = false;
        for (auto& f : formats) {
            struct v4l2_format fmt = {};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            fmt.fmt.pix_mp.width = width; fmt.fmt.pix_mp.height = height;
            fmt.fmt.pix_mp.pixelformat = f.fmt; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            fmt.fmt.pix_mp.num_planes = f.planes;
            if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix_mp.pixelformat == f.fmt) {
                pixel_format = f.fmt; num_planes = f.planes;
                width = fmt.fmt.pix_mp.width; height = fmt.fmt.pix_mp.height;
                format_set = true;
                log("Камера: " + std::string(f.name) + " " + std::to_string(width) + "x" + std::to_string(height));
                break;
            }
        }
        if (!format_set) throw std::runtime_error("Не удалось установить формат");

        struct v4l2_requestbuffers req = {};
        req.count = 1; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
        ioctl(fd, VIDIOC_REQBUFS, &req);

        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP; buf.index = 0;
        struct v4l2_plane planes[2] = {};
        buf.length = num_planes; buf.m.planes = planes;
        ioctl(fd, VIDIOC_QUERYBUF, &buf);

        buffer_length = planes[0].length;
        buffer_start = mmap(NULL, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
        ioctl(fd, VIDIOC_QBUF, &buf);

        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(fd, VIDIOC_STREAMON, &type);
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
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP; buf.index = 0;
        struct v4l2_plane planes[2] = {};
        buf.length = num_planes; buf.m.planes = planes;

        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        struct timeval tv = {1, 0};
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) return false;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) return false;

        if (pixel_format == V4L2_PIX_FMT_NV12) {
            gray_mat = cv::Mat(height, width, CV_8UC1, buffer_start).clone();
        } else {
            cv::Mat raw_mat(height, width, CV_8UC2, buffer_start);
            gray_mat.create(height, width, CV_8UC1);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) gray_mat.at<uchar>(y, x) = raw_mat.at<uchar>(y, x * 2 + 1);
            }
        }
        ioctl(fd, VIDIOC_QBUF, &buf);
        return true;
    }
};

int main() {
    log("=== Запуск СКУД (QR + Фото по команде) ===");
    try {
        std::thread input_thread(console_input_loop);
        V4L2Camera cam("/dev/video12", 640, 480);
        cv::Mat gray_frame;
        auto cooldown_end_time = std::chrono::steady_clock::now();
        log("Ожидание QR-кода... (или введите 'photo' в консоль)");

        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            if (now < cooldown_end_time) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }

            if (!cam.grab_gray_frame(gray_frame)) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }

            if (g_capture_requested) {
                g_capture_requested = false;
                
                // Получаем расширенный результат от сервера
                ValidationResult res = send_photo_to_server(gray_frame);
                
                if (res.is_valid) {
                    log("========================================");
                    log("✅ ДОСТУП РАЗРЕШЕН (по кодовой фразе)");
                    if (!res.user_name.empty()) {
                        log("👤 Пользователь: " + res.user_name);
                    }
                    if (!res.message.empty()) {
                        log("💬 Сообщение: " + res.message);
                    }
                    log("========================================");
                } else {
                    log("========================================");
                    log("❌ ДОСТУП ЗАПРЕЩЕН (неверная фраза)");
                    if (!res.message.empty()) {
                        log("💬 Причина: " + res.message);
                    }
                    log("========================================");
                }
                
                cooldown_end_time = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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
                    log("========================================\n✅ ДОСТУП РАЗРЕШЕН (по QR-коду)\n========================================");
                } else {
                    log("========================================\n❌ ДОСТУП ЗАПРЕЩЕН (по QR-коду)\n========================================");
                }
                cooldown_end_time = std::chrono::steady_clock::now() + std::chrono::seconds(COOLDOWN_SECONDS);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30)); 
        }
        log("Остановка...");
        input_thread.join();
    } catch (const std::exception& e) {
        log("ФАТАЛЬНАЯ ОШИБКА: " + std::string(e.what()));
    }
    return 0;
}