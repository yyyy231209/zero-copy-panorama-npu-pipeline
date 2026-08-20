#!/bin/bash
# fetch_deps.sh — 自动拉取并构建两个上游依赖仓库（B 模式）
#
# 用法（RK3588 板上执行）：
#   ./fetch_deps.sh
#
# 会把以下仓库 clone 到 deps/ 并构建出静态库：
#   deps/panorama_pipeline   <- zero-copy-vpu-gpu-rga-stitch (v1.0.0)
#   deps/npu_crack_detect    <- zero-copy-tri-core-npu-inference (v1.0.0)
#
# 需要已安装：git、make、cmake、MPP、RGA、RKNN Runtime、Mali OpenCL
# （软件环境详见 README 的复刻条件章节）

set -e

PANO_URL="https://github.com/yyyy231209/zero-copy-vpu-gpu-rga-stitch.git"
NPU_URL="https://github.com/yyyy231209/zero-copy-tri-core-npu-inference.git"
TAG="v1.0.0"

mkdir -p deps

# 1. 全景拼接（Makefile 项目）
if [ ! -d deps/panorama_pipeline/.git ]; then
    echo "== clone panorama pipeline ($TAG)"
    git clone --depth 1 --branch "$TAG" "$PANO_URL" deps/panorama_pipeline
fi
echo "== build panorama pipeline"
make -C deps/panorama_pipeline all

# 2. NPU 检测（CMake 项目）
if [ ! -d deps/npu_crack_detect/.git ]; then
    echo "== clone npu crack detect ($TAG)"
    git clone --depth 1 --branch "$TAG" "$NPU_URL" deps/npu_crack_detect
fi
echo "== build npu crack detect"
cmake -S deps/npu_crack_detect -B deps/npu_crack_detect/build \
      -DCMAKE_BUILD_TYPE=Release
cmake --build deps/npu_crack_detect/build -j2

echo ""
echo "deps ready:"
echo "  panorama lib: deps/panorama_pipeline/build/libpanorama_pipeline.a"
echo "  npu lib:      deps/npu_crack_detect/build/libnpu_detect_core.a"
