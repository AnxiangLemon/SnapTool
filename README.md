# SnapTool

[![Build](https://github.com/AnxiangLemon/SnapTool/actions/workflows/build.yml/badge.svg)](https://github.com/AnxiangLemon/SnapTool/actions/workflows/build.yml)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4)
![License](https://img.shields.io/badge/license-MIT-green)

一个使用纯 C++17 + Windows 原生 API（Win32 / GDI+ / DWM）编写的极简桌面截图小工具。  
编译产物为**单个可执行文件（约 288 KB），零任何第三方 DLL 依赖**，开箱即用。

## 功能特性

- **精准框选与窗口探测**：悬停自动识别可见窗口（基于 DWM），单击选中窗口、拖拽自由框选，支持 8 手柄与方向键微调，原生适配多显示器与高分屏（Per-Monitor DPI V2）。
- **滚动长截图（长图拼接）**：选区后点击长截图按钮或按 `L` 进入长图模式，**纯 C++ 零依赖拼接引擎**，支持**手动滚轮**与**一键自动滚动**（到达底部自动停止）、自动识别并剔除网页吸顶/吸底固定栏、侧边实时预览拼接进度，完成后可直接复制、保存或贴图。
- **像素放大镜与取色器**：实时显示 11×11 像素放大格、坐标与 `#RRGGBB` 色值，悬停态按 `C` 键一键复制色值。
- **常用标注工具**：矩形、椭圆、箭头、画笔、半透明荧光笔、真实像素化马赛克、自动递增序号、中文文字输入（支持系统 IME 与多行换行），提供 3 档粗细/字号、8 种配色与撤销/重做。
- **桌面贴图（钉图）**：按 `T` 将截图（含标注或长图）置顶贴在桌面上，超长图片自动适配屏幕初始缩放，支持多张贴图、拖拽移动、滚轮缩放（15%～500%）、`Ctrl + 滚轮` 调透明度、双击复制、中键/`Esc` 关闭。
- **系统托盘常驻**：点击托盘图标弹出菜单（不会误触截图），支持切换全局热键、延时 3s/5s 截图、一键关闭全部贴图及开机自动启动。

## 快捷键一览

| 场景 | 快捷键 | 说明 |
| --- | --- | --- |
| 全局 | `Ctrl + Alt + A` | 开始截图（可在托盘菜单切换 `Alt+A` / `F1` / `Ctrl+Shift+A` / `PrintScreen`） |
| 悬停选窗 | `C` | 复制光标处像素 `#RRGGBB` 色值 |
| 编辑选区 | `Enter` / 双击选区 / `Ctrl + C` | 完成截图并复制到剪贴板 |
| 编辑选区 | `L` | 进入滚动长截图模式 |
| 编辑选区 | `T` | 贴图到桌面（置顶悬浮窗） |
| 编辑选区 | `Ctrl + S` | 另存为 PNG / JPG / BMP |
| 编辑选区 | `Ctrl + Z` / `Ctrl + Y` | 撤销 / 重做标注 |
| 编辑选区 | 方向键 / `Shift + 方向键` | 1px / 10px 微调选区位置 |
| 编辑选区 | 右键 / `Esc` | 右键取消当前选区重选；未选区时右键或 `Esc` 退出 |
| 长截图模式 | 鼠标滚轮 / 点击「自动滚动」 | 手动向下滚动实时拼接，或开启自动滚动（到底自动停止） |
| 长截图模式 | `Esc` | 随时取消退出长截图 |
| 文字输入 | `Ctrl + Enter` | 确认提交文字 |
| 桌面贴图 | 滚轮 / `Ctrl + 滚轮` | 缩放大小 / 调节透明度 |
| 桌面贴图 | 双击 / 中键 / `Esc` | 复制图片 / 关闭当前贴图 |

## 从源码编译

### 方式一：MinGW-w64（推荐，生成约 288 KB 单文件）

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 -RunSelfTest
```

### 方式二：CMake（兼容 MinGW / MSVC）

```powershell
cmake -B build
cmake --build build --config Release
```

> 也可前往 [Releases](https://github.com/AnxiangLemon/SnapTool/releases) 或 [Actions](https://github.com/AnxiangLemon/SnapTool/actions) 页面获取自动构建的 `SnapTool.exe`。

## 目录结构

```text
├── src/
│   ├── common.h          # 虚拟桌面截屏、DWM 窗口枚举、剪贴板与图片保存
│   ├── renderer.h        # GDI+ 标注渲染、像素马赛克、放大镜与矢量图标
│   ├── pin_window.h      # 桌面置顶贴图悬浮窗（支持超长图自适应缩放）
│   ├── scroll_capture.h  # 纯 C++ 垂直长图拼接引擎、外侧边框、控制条与实时预览
│   ├── capture_overlay.h # 全屏双缓冲截图遮罩与交互状态机
│   └── main.cpp          # 程序入口、托盘菜单、全局热键、配置与自测
├── .github/workflows/
│   └── build.yml         # GitHub Actions 自动编译与 Release 发布
├── build.ps1             # PowerShell 一键编译脚本
├── CMakeLists.txt        # CMake 构建配置
├── LICENSE               # MIT 开源协议
└── README.md
```

## 更新日志

### v1.1.0
- 新增**滚动长截图（长图拼接）**功能：工具栏新增「长截图 (L)」入口，支持手动滚轮与自动滚动双模式。
- 内置纯 C++ 零依赖垂直拼接引擎：自动剔除吸顶/吸底固定栏、避开滚动条区域、抗噪精匹配，并附带侧边实时拼接预览窗。
- 优化桌面贴图窗口：贴入超长图片时自动按屏幕工作区比例缩放，不再超出屏幕范围。

### v1.0.0
- 首个开源版本：支持窗口探测框选、像素放大镜取色、全套标注工具、桌面贴图与系统托盘管理。

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。

## 作者

**AnxiangLemon**
