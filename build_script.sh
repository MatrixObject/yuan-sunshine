# 1. 环境准备
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0="http.sslbackend"
export GIT_CONFIG_VALUE_0="openssl"

SRCROOT="I:/NBOX2024-9-6/YuanSunshine/Sunshine-2026.906.222525"
cd "$SRCROOT"

# 2. 重新配置 CMake（清理旧构建缓存以确保新目标被识别）
cmake -B cmake-build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=install \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DNPM='C:/Program Files/nodejs/npm.cmd' \
  -DNPM_NODE_EXECUTABLE='C:/Program Files/nodejs/node.exe' \
  -DDOTNET_EXECUTABLE=''

# 3. 并行构建
cmake --build cmake-build-release --parallel 8

# 4. 运行测试
./cmake-build-release/tests/test_sunshine
