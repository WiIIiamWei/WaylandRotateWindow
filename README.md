# Wayland Rotate Window

在 Wayland 上实时预览、旋转和翻转选定窗口。

**本项目的代码、界面、测试和文档完全由 AI 完成。**

## 功能

- 顺时针旋转 0 / 90 / 180 / 270°，支持左右和上下翻转。
- 保持画面比例，自动适应源窗口尺寸变化。
- C++17、Qt 6、PipeWire 与 OpenGL；优先使用 DMA-BUF，支持共享内存回退。
- GPU 完成画面变换，只保留最新帧，避免延迟累积。

## 运行要求

- Wayland 会话、OpenGL 3.3、Qt 6.4+、PipeWire 0.3.65+。
- `xdg-desktop-portal` 及支持窗口采集的桌面后端，例如 KDE 或 GNOME 后端。

预览不会向源窗口转发鼠标和键盘输入。实际延迟及最小化后的采集行为取决于桌面后端、显卡和刷新率。

## 构建与使用

安装 CMake 3.20+、C++17 编译器、Ninja、pkg-config，以及 Qt 6 Widgets / OpenGL / D-Bus、PipeWire、EGL 和 libdrm 的开发包。

Arch Linux / KDE 示例：

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-wayland pipewire libdrm mesa xdg-desktop-portal xdg-desktop-portal-kde
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
QT_QPA_PLATFORM=wayland ./build/wayland-rotate-window
```

点击“选择窗口”，在系统对话框中授权后调整旋转和翻转。状态栏显示采集尺寸和缓冲区类型，点击“停止”结束采集。

可选安装：

```sh
cmake --install build --prefix "$HOME/.local"
```

## 许可证

[MIT](LICENSE)
