# FishWindow 🐟 - 摸鱼窗口裁剪工具

让任意窗口只保留指定区域可见，其余部分完全透明露出桌面。老板看过去就是桌面上飘着一块内容，看不出是什么程序。

## 效果

```
┌─────────────────────────┐
│  原窗口（大部分透明）    │
│                          │
│   ┌──────────┐          │
│   │ 保留区域  │ ← 只有这块有内容
│   └──────────┘          │
│                          │
└─────────────────────────┘
     ↓ 透明，能看到后面的桌面/窗口
```

## ✨ 特性

- **多窗口同时裁切** — 最多 16 个窗口同时裁剪，互不干扰，各自独立管理
- **多显示器支持** — 跨显示器框选，虚拟桌面全覆盖
- **智能识别前台窗口** — 快捷键自动作用于当前焦点窗口，不用手动切换
- **边框可拖拽移动** — 裁切后通过边框拖拽移动窗口，比原窗口更方便
- **深色主题 UI** — 选择器、欢迎页、滚动条全部深色风格
- **零依赖单文件** — 一个 exe 直接运行，不需要安装任何运行时
- **安全还原** — 退出时自动还原所有窗口，裁切是临时操作不影响原程序

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Alt+F` | 框选区域（自动识别前台窗口，已裁剪则重新框选） |
| `Ctrl+Alt+B` | 显示/隐藏裁剪边框（隐藏后完全融入桌面） |
| `Ctrl+Alt+T` | 窗口置顶开关 |
| `Ctrl+Alt+Q` | 还原当前窗口（取消裁剪，恢复原样） |

## 使用方法

1. 从 [Releases](https://github.com/Eghack6/FishWindow/releases) 下载 `FishWindow.exe`
2. 双击运行 → 弹出快捷键说明 → 确定后弹出窗口选择器
3. 从列表选择目标窗口 → 拖拽框选要保留的区域
4. 按 `Ctrl+Alt+B` 隐藏边框 → 裁剪后的窗口融入桌面
5. 边框可拖拽移动窗口位置
6. 按 `Ctrl+Alt+F` 可再次裁剪当前窗口（自动识别前台窗口）
7. 右键托盘图标可切换目标窗口、置顶、还原
8. 退出程序自动还原所有窗口

## 安全保证

- `SetWindowRgn` 和窗口样式修改都是临时操作，程序退出后窗口自动恢复
- 按 `Ctrl+Alt+Q` 可随时还原窗口，不影响原程序
- 不修改注册表、不注入 DLL、不 hook 消息
- 单实例运行（Mutex 防重复启动）

## 编译

### Windows（MSVC）

需要 Visual Studio 2019+ 或 Build Tools：

```bash
# 编译资源文件
rc fish_window.rc

# 编译 exe
cl /O2 /Fe:FishWindow.exe fish_window.c fish_window_res.o user32.lib gdi32.lib kernel32.lib shell32.lib msimg32.lib psapi.lib uxtheme.lib dwmapi.lib
```

### Windows（MinGW）

```bash
windres fish_window.rc fish_window_res.o
gcc -mwindows -O2 -o FishWindow.exe fish_window.c fish_window_res.o -lgdi32 -luser32 -lkernel32 -lshell32 -lmsimg32 -lpsapi -luxtheme -ldwmapi
```

### Linux 交叉编译（MinGW）

```bash
sudo apt install gcc-mingw-w64-x86-64
x86_64-w64-mingw32-windres fish_window.rc fish_window_res.o
x86_64-w64-mingw32-gcc -mwindows -O2 -o FishWindow.exe fish_window.c fish_window_res.o -lgdi32 -luser32 -lkernel32 -lshell32 -lmsimg32 -lpsapi -luxtheme -ldwmapi
```

## 技术原理

- **SetWindowRgn**：设置窗口可见区域，区域外不渲染（自然透明）
- **SelectionOverlay**：全屏半透明覆盖层 + 双缓冲，框选交互无闪烁
- **BorderOverlay**：独立无边框窗口，绘制裁剪区域边框 + 状态文字
- **窗口跟踪**：200ms 轮询窗口位置变化，边框跟随移动
- **多窗口管理**：ClipEntry 数组管理多个裁切窗口，全局变量 + 同步层

## 系统要求

- Windows 10+
- 无需安装任何运行时
