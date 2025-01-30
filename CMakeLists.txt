cmake_minimum_required(VERSION 3.15...3.31)

project(rpi_simple_video LANGUAGES CXX)

add_executable(simple_video main.cpp simple_video.cpp h264_encoder.cpp)
target_include_directories(simple_video PUBLIC /usr/include/libcamera)
target_link_libraries(simple_video PUBLIC camera-base camera)

add_executable(simple_capture simple_capture.cpp)
target_include_directories(simple_capture PUBLIC /usr/include/libcamera)
target_link_libraries(simple_capture PUBLIC camera-base camera jpeg)
