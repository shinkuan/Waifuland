#!/bin/bash

# 0. 提醒用戶安裝 Live2D Cubism SDK for Native
echo "====================================================================="
echo "Please ensure you have downloaded the Live2D Cubism SDK for Native"
echo "from: https://www.live2d.com/en/sdk/about/"
echo "Extract it to the current directory (./) and update the SDK name or"
echo "path in CMakeLists.txt if necessary."
echo "====================================================================="
echo ""
read -p "Press [Enter] to continue if you have already set up the SDK, or Ctrl+C to abort..."

# 1. 執行 thirdParty 腳本
echo "Setting up dependencies (GLEW and GLFW)..."
cd ./thirdParty || exit 1
bash ./scripts/setup_glew_glfw
cd .. || exit 1

# 2. 建立並編譯專案
echo "Building the project..."
mkdir -p build
cd build || exit 1
cmake ..
make -j

echo "Installation and build complete!"
