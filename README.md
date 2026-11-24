# SnapTool

[![Build](https://github.com/AnxiangLemon/SnapTool/actions/workflows/build.yml/badge.svg)](https://github.com/AnxiangLemon/SnapTool/actions/workflows/build.yml)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4)
![License](https://img.shields.io/badge/license-MIT-green)

一个使用纯 C++17 + Windows 原生 API（Win32 / GDI+ / DWM）编写的极简桌面截图小工具。  
编译产物为**单个可执行文件（约 268 KB），零任何第三方 DLL 依赖**，开箱即用。

## 功能特性

- **精准框选与窗口探测**：悬停自动识别可见窗口（基于 DWM），单击选中窗口、拖拽自由框选，支持 8 手柄与方向键微调，原生适配多显示器与高分屏（Per-Monitor DPI V2）。
- **像素放大镜与取色器**：实时显示 11×11 像素放大格、坐标与 `#RRGGBB` 色值，悬停态按 `C` 键一键复制色值。
- **常用标注工具**：矩形、椭圆、箭头、画笔、半透明荧光笔、真实像素化马赛克、自动递增序号、中文文字输入（支持系统 IME 与多行换行），提供 3 档粗细/字号、8 种配色与撤销/重做。
- **桌面贴图（钉图）**：按 `T` 将截图（含标注）置顶贴在桌面上，支持多张贴图、拖拽移动、滚轮缩放（15%～500%）、`Ctrl + 滚轮` 调透明度、双击复制、中键/`Esc` 关闭。
- **系统托盘常驻**：点击托盘图标弹出菜单（不会误触截图），支持切换全局热键、延时 3s/5s 截图、一键关闭全部贴图及开机自动启动。

## 快捷键一览

| 场景 | 快捷键 | 说明 |
| --- | --- | --- |
| 全局 | `Ctrl + Alt + A` | 开始截图（可在托盘菜单切换 `Alt+A` / `F1` / `Ctrl+Shift+A` / `PrintScreen`） |
| 悬停选窗 | `C` | 复制光标处像素 `#RRGGBB` 色值 |
| 编辑选区 | `Enter` / 双击选区 / `Ctrl + C` | 完成截图并复制到剪贴板 |
| 编辑选区 | `T` | 贴图到桌面（置顶悬浮窗） |
| 编辑选区 | `Ctrl + S` | 另存为 PNG / JPG / BMP |
| 编辑选区 | `Ctrl + Z` / `Ctrl + Y` | 撤销 / 重做标注 |
| 编辑选区 | 方向键 / `Shift + 方向键` | 1px / 10px 微调选区位置 |
| 编辑选区 | 右键 / `Esc` | 右键取消当前选区重选；未选区时右键或 `Esc` 退出 |
| 文字输入 | `Ctrl + Enter` | 确认提交文字 |
| 桌面贴图 | 滚轮 / `Ctrl + 滚轮` | 缩放大小 / 调节透明度 |
| 桌面贴图 | 双击 / 中键 / `Esc` | 复制图片 / 关闭当前贴图 |

## 从源码编译

### 方式一：MinGW-w64（推荐，生成约 268 KB 单文件）

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
│   ├── pin_window.h      # 桌面置顶贴图悬浮窗
│   ├── capture_overlay.h # 全屏双缓冲截图遮罩与交互状态机
│   └── main.cpp          # 程序入口、托盘菜单、全局热键、配置与自测
├── .github/workflows/
│   └── build.yml         # GitHub Actions 自动编译与 Release 发布
├── build.ps1             # PowerShell 一键编译脚本
├── CMakeLists.txt        # CMake 构建配置
├── LICENSE               # MIT 开源协议
└── README.md
```

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。

## 作者

**AnxiangLemon**
