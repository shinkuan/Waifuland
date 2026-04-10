# Waifuland

## 安裝指南 (Installation Guide)

### 準備工作 (Prerequisites)
1. 前往 Live2D 官方網站下載 **Cubism SDK for Native**:
   [https://www.live2d.com/en/sdk/about/](https://www.live2d.com/en/sdk/about/)
2. 將下載的 SDK 解壓縮到專案根目錄 (`./`)。
   *(例如：`CubismSdkForNative-5-r.5`)*
3. 如果您下載的 SDK 版本或資料夾名稱與預設不同，請開啟 `CMakeLists.txt` 並更新對應的 SDK 名稱或路徑。

### 安裝與編譯 (Build Instructions)

您可以使用我們提供的自動安裝腳本，或是手動執行指令。

#### 方法一：使用自動安裝腳本
在終端機中，賦予腳本執行權限並執行：
```bash
chmod +x install.sh
./install.sh
```

#### 方法二：手動編譯
1. **設定第三方函式庫 (GLEW & GLFW)**
   進入 `thirdParty` 資料夾並執行設定腳本：
   ```bash
   cd thirdParty
   ./scripts/setup_glew_glfw
   cd ..
   ```

2. **建立與編譯專案**
   建立 `build` 資料夾並使用 CMake 進行編譯：
   ```bash
   mkdir build
   cd build
   cmake ..
   make -j
   ```

編譯完成後，執行檔將會產生在 `build/bin/` 目錄下。
