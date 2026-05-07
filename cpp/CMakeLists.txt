cmake_minimum_required(VERSION 3.16)
project(comvisplus_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COMVISPLUS_ENABLE_OPENVINO "Enable OpenVINO inference backend" ON)

add_executable(comvisplus_native
    src/main.cpp
    src/app_config.cpp
    src/camera_manager.cpp
    src/counter_engine.cpp
    src/openvino_backend.cpp
    src/http_server.cpp
)

target_include_directories(comvisplus_native PRIVATE include)

find_package(Threads REQUIRED)
target_link_libraries(comvisplus_native PRIVATE Threads::Threads)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
if(TARGET opencv_core)
    target_link_libraries(comvisplus_native PRIVATE
        opencv_core
        opencv_imgproc
        opencv_imgcodecs
        opencv_videoio
    )
elseif(TARGET OpenCV::opencv_core)
    target_link_libraries(comvisplus_native PRIVATE
        OpenCV::opencv_core
        OpenCV::opencv_imgproc
        OpenCV::opencv_imgcodecs
        OpenCV::opencv_videoio
    )
elseif(OpenCV_LIBRARIES)
    target_link_libraries(comvisplus_native PRIVATE ${OpenCV_LIBRARIES})
elseif(OpenCV_LIBS)
    target_link_libraries(comvisplus_native PRIVATE ${OpenCV_LIBS})
else()
    message(FATAL_ERROR "OpenCV libraries were not exported by find_package(OpenCV)")
endif()
target_include_directories(comvisplus_native PRIVATE ${OpenCV_INCLUDE_DIRS})

if(COMVISPLUS_ENABLE_OPENVINO)
    find_package(OpenVINO REQUIRED COMPONENTS Runtime)
    target_link_libraries(comvisplus_native PRIVATE openvino::runtime)
    target_compile_definitions(comvisplus_native PRIVATE COMVISPLUS_HAVE_OPENVINO=1)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(comvisplus_native PRIVATE stdc++fs)
endif()
