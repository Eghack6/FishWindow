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

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Alt+F` | 框选区域（自动识别当前窗口，已裁剪则重新框选） |
| `Ctrl+Alt+B` | 显示/隐藏裁剪边框（隐藏后完全融入桌面） |
| `Ctrl+Alt+T` | 窗口置顶开关 |
| `Ctrl+Alt+Q` | 还原当前窗口（取消裁剪，恢复原样） |

## 使用方法

1. 下载 [FishWindow.exe](https://github.com/yourname/FishWindow/releases) 或自行编译
2. 双击运行 → 弹出快捷键提示 → 确定后托盘出现小鱼图标
3. 按 `Ctrl+Alt+F` → 从列表选择目标窗口
4. 拖拽框选要保留的区域 → 松开鼠标确认
5. 按 `Ctrl+Alt+B` 隐藏边框 → 裁剪后的窗口融入桌面
6. 边框可拖拽移动窗口位置
6. 右键托盘图标可切换目标窗口
7. 按 `Ctrl+Alt+Q` 还原当前窗口
8. 退出程序：右键托盘图标 → 退出（会自动还原所有窗口）

## 编译

需要 Linux + MinGW 交叉编译工具链：

```bash
# 安装 MinGW
sudo apt install gcc-mingw-w64-x86-64

# 编译资源文件
x86_64-w64-mingw32-windres fish_window.rc fish_window_res.o

# 编译 exe
x86_64-w64-mingw32-gcc -mwindows -O2 -o FishWindow.exe \
  fish_window.c fish_window_res.o \
  -lgdi32 -luser32 -lkernel32 -lshell32 -lmsimg32 -lpsapi
```

生成的 `FishWindow.exe` 零依赖，Windows 10+ 直接运行。

## 安全保证

- `SetWindowRgn` 和窗口样式修改都是临时操作，程序退出后窗口自动恢复
- 按 `Ctrl+Alt+Q` 可随时还原窗口，不影响原程序
- 不修改注册表、不注入 DLL、不 hook 消息
- 单实例运行（Mutex 防重复启动）

## 技术原理

- **SetWindowRgn**：设置窗口可见区域，区域外不渲染（自然透明）
- **SelectionOverlay**：全屏半透明覆盖层 + 双缓冲，框选交互无闪烁
- **BorderOverlay**：独立无边框窗口，绘制裁剪区域边框 + 状态文字
- **窗口跟踪**：200ms 轮询窗口位置变化，边框跟随移动
- **ANSI API**：全量使用 ANSI (A) 后缀 Win32 API，避免 MinGW 编码问题
