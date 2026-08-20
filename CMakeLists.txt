cmake_minimum_required(VERSION 3.10)
project(panorama_npu_integration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 依赖定位（A 模式：路径变量；B 模式：fetch_deps.sh 自动拉取到 deps/）
# 查找顺序：
#   1. -DPANORAMA_ROOT=... -DNPU_ROOT=... 显式指定
#   2. 本仓库 deps/ 目录（运行 ./fetch_deps.sh 后）
#   3. 上级目录 ../panorama_pipeline 与 ../npu_crack_detect
# ---------------------------------------------------------------------------
set(PANORAMA_ROOT "" CACHE PATH "Panorama pipeline source root")
set(NPU_ROOT "" CACHE PATH "NPU detect source root")

if(NOT PANORAMA_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/panorama_pipeline/include")
        set(PANORAMA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/panorama_pipeline")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../panorama_pipeline/include")
        set(PANORAMA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../panorama_pipeline")
    endif()
endif()
if(NOT NPU_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/npu_crack_detect/include")
        set(NPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/npu_crack_detect")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../npu_crack_detect/include")
        set(NPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../npu_crack_detect")
    endif()
endif()

if(NOT PANORAMA_ROOT OR NOT NPU_ROOT)
    message(FATAL_ERROR
        "Dependencies not found.\n"
        "Option A: pass -DPANORAMA_ROOT=<dir> -DNPU_ROOT=<dir>\n"
        "Option B: run ./fetch_deps.sh to clone both repositories into deps/")
endif()

message(STATUS "PANORAMA_ROOT = ${PANORAMA_ROOT}")
message(STATUS "NPU_ROOT      = ${NPU_ROOT}")

# ---------------------------------------------------------------------------
# 硬件依赖
# ---------------------------------------------------------------------------
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs)
find_package(Threads REQUIRED)
find_library(RGA_LIB NAMES rga PATHS /lib /usr/lib /usr/lib/aarch64-linux-gnu /usr/local/lib REQUIRED)
find_library(RKNNRT_LIB NAMES rknnrt PATHS /lib /usr/lib /usr/lib/aarch64-linux-gnu /usr/local/lib REQUIRED)
find_library(MPP_LIB NAMES rockchip_mpp PATHS /lib /usr/lib /usr/lib/aarch64-linux-gnu /usr/local/lib REQUIRED)
find_library(MALI_OPENCL_LIB NAMES OpenCL libOpenCL.so.1
             PATHS /usr/lib/aarch64-linux-gnu/libmali-x11 NO_DEFAULT_PATH REQUIRED)

set(INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PANORAMA_ROOT}/include
    ${NPU_ROOT}/include
    ${NPU_ROOT}/third_party/rknn/include
    /usr/include/rga
)

# ---------------------------------------------------------------------------
# 集成核心：输出插件框架 + 全景→NPU 桥接
# ---------------------------------------------------------------------------
add_library(panorama_npu_adapter STATIC
    src/output_sink.cpp
    src/panorama_npu_adapter.cpp
    src/sinks/tcp_h264_sink.cpp
)
target_include_directories(panorama_npu_adapter PUBLIC ${INCLUDE_DIRS})
target_compile_options(panorama_npu_adapter PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# 演示程序：全景 -> NPU -> 输出插件
# ---------------------------------------------------------------------------
add_executable(panorama_npu_live apps/panorama_npu_live.cpp)
target_include_directories(panorama_npu_live PRIVATE ${INCLUDE_DIRS})
target_link_libraries(panorama_npu_live PRIVATE
    panorama_npu_adapter
    ${PANORAMA_ROOT}/build/libpanorama_pipeline.a
    ${NPU_ROOT}/build/libnpu_detect_core.a
    ${OpenCV_LIBS}
    ${MALI_OPENCL_LIB}
    ${MPP_LIB}
    ${RGA_LIB}
    ${RKNNRT_LIB}
    Threads::Threads
    dl
)
set_target_properties(panorama_npu_live PROPERTIES
    BUILD_RPATH "/usr/lib/aarch64-linux-gnu/libmali-x11;/usr/local/lib")
target_compile_options(panorama_npu_live PRIVATE -Wall -Wextra -Wpedantic)
