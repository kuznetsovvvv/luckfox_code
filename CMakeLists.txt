cmake_minimum_required(VERSION 3.10)

# Подключение тулчейна ДО project()
set(ZXING_DIR "/home/kali/zxing-cpp")
include(${ZXING_DIR}/luckfox_toolchain.cmake)

project(qr_scanner)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Запрещаем использовать системные заголовки/библиотеки хоста (x86_64)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# OpenCV 5.x Mobile
set(OPENCV_MOBILE_DIR "/home/kali/opencv-mobile-5.0.0-luckfox-pico")
set(OpenCV_INCLUDE_DIRS ${OPENCV_MOBILE_DIR}/include/opencv5)

# Статические библиотеки OpenCV
set(OpenCV_LIBRARIES 
    ${OPENCV_MOBILE_DIR}/lib/libopencv_core.a
    ${OPENCV_MOBILE_DIR}/lib/libopencv_imgproc.a
    ${OPENCV_MOBILE_DIR}/lib/libopencv_photo.a
    ${OPENCV_MOBILE_DIR}/lib/libopencv_geometry.a
    ${OPENCV_MOBILE_DIR}/lib/libopencv_features.a
    ${OPENCV_MOBILE_DIR}/lib/libopencv_video.a
)

# ZXing
add_subdirectory(${ZXING_DIR} ${CMAKE_BINARY_DIR}/zxing-build)

add_executable(qr_scanner qr_scanner.cpp)

target_include_directories(qr_scanner PRIVATE
    ${ZXING_DIR}/core/src
    ${OpenCV_INCLUDE_DIRS}
)

# Линкуем только OpenCV, ZXing и стандартные библиотеки Linux (сокеты встроены в libc)
target_link_libraries(qr_scanner
    ZXing
    ${OpenCV_LIBRARIES}
    pthread
    dl
    z
    m
)