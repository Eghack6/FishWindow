/*
 * FishWindow - 摸鱼窗口裁剪工具
 * 纯 Win32 API，MinGW 交叉编译为 Windows exe
 * 全部使用 ANSI API，中文 Windows 上自动 GBK 编码
 *
 * 编译: x86_64-w64-mingw32-windres fish_window.rc fish_window_res.o &&
 *        x86_64-w64-mingw32-gcc -mwindows -O2 -o FishWindow.exe fish_window.c fish_window_res.o -lgdi32 -luser32 -lkernel32 -lshell32 -lmsimg32 -lpsapi
 */

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

/* DPI awareness - declare Per-Monitor V2 API dynamically */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
typedef BOOL (WINAPI *pfnSetProcessDpiAwarenessContext)(HANDLE);
typedef BOOL (WINAPI *pfnSetProcessDpiAwareness)(int);
typedef UINT (WINAPI *pfnGetDpiForWindow)(HWND);

static void EnableDpiAwareness(void)
{
    /* Try Per-Monitor V2 (Win10 1703+) */
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        pfnSetProcessDpiAwarenessContext fn =
            (pfnSetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (fn) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_CONTEXT_HANDLE)-4) */
            if (fn((HANDLE)-4)) return;
            /* Fallback: Per-Monitor V1 = ((DPI_CONTEXT_HANDLE)-3) */
            if (fn((HANDLE)-3)) return;
        }
    }
    /* Try Per-Monitor V1 (Win8.1+) via shcore */
    HMODULE hShcore = LoadLibraryA("shcore.dll");
    if (hShcore) {
        pfnSetProcessDpiAwareness fn2 =
            (pfnSetProcessDpiAwareness)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (fn2) fn2(2); /* PROCESS_PER_MONITOR_DPI_AWARE */
        FreeLibrary(hShcore);
    }
}
#pragma GCC diagnostic pop

/* GetDpiScale removed — EnableDpiAwareness handles DPI; per-window scale not used */

/* ======================== Constants ======================== */

#define HK_SELECT   1
#define HK_BORDER   3
#define HK_TOPMOST  4
#define HK_QUIT     5

#define ID_TRAY     1001
#define WM_TRAYICON (WM_USER + 1)
#define TIMER_TRACK 200

/* ======================== Global State ======================== */

static HWND g_target_hwnd = NULL;
static char g_target_title[256] = {0};
static BOOL g_has_clip = FALSE;
static BOOL g_show_border = FALSE;
static BOOL g_is_topmost = FALSE;
static RECT g_clip_rect_window = {0};
static RECT g_last_win_rect = {0};
static HWND g_border_hwnd = NULL;
static HWND g_main_hwnd = NULL;
static HINSTANCE g_hinst = NULL;
static NOTIFYICONDATAW g_nid = {0};
static LONG g_orig_style = 0;
static LONG g_orig_ex_style = 0;
static BOOL g_style_saved = FALSE;

/* ======================== Selection Overlay ======================== */

typedef struct {
    BOOL selecting;
    POINT start;
    POINT end;
    RECT result;
    BOOL made;
    HDC snapshot_dc;
} SelectionState;

static LRESULT CALLBACK SelectionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SelectionState *ss = (SelectionState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_LBUTTONDOWN: {
        RECT wr;
        GetWindowRect(hwnd, &wr);
        ss->selecting = TRUE;
        ss->start.x = GET_X_LPARAM(lp) + wr.left;
        ss->start.y = GET_Y_LPARAM(lp) + wr.top;
        ss->end = ss->start;
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (ss->selecting) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            ss->end.x = GET_X_LPARAM(lp) + wr.left;
            ss->end.y = GET_Y_LPARAM(lp) + wr.top;
            /* Double-buffer: invalidate without erasing */
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (ss->selecting) {
            ss->selecting = FALSE;
            int x1 = min(ss->start.x, ss->end.x);
            int y1 = min(ss->start.y, ss->end.y);
            int x2 = max(ss->start.x, ss->end.x);
            int y2 = max(ss->start.y, ss->end.y);
            if (x2 - x1 >= 10 && y2 - y1 >= 10) {
                ss->result.left = x1;
                ss->result.top = y1;
                ss->result.right = x2;
                ss->result.bottom = y2;
                ss->made = TRUE;
            }
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE) {
            ss->made = FALSE;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        /* Double-buffer: draw to offscreen DC first */
        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, sw, sh);
        HBITMAP old_bmp = SelectObject(mem_dc, mem_bmp);

        /* Draw screenshot */
        BitBlt(mem_dc, 0, 0, sw, sh, ss->snapshot_dc, 0, 0, SRCCOPY);

        /* Dark overlay using AlphaBlend */
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 140, 0};
        HDC dark_dc = CreateCompatibleDC(mem_dc);
        HBITMAP dark_bmp = CreateCompatibleBitmap(mem_dc, sw, sh);
        SelectObject(dark_dc, dark_bmp);
        RECT full = {0, 0, sw, sh};
        HBRUSH dark_brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dark_dc, &full, dark_brush);
        AlphaBlend(mem_dc, 0, 0, sw, sh, dark_dc, 0, 0, sw, sh, bf);
        DeleteObject(dark_bmp);
        DeleteDC(dark_dc);
        DeleteObject(dark_brush);

        /* Selection rectangle */
        if (ss->selecting || ss->made) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int x1 = min(ss->start.x, ss->end.x) - wr.left;
            int y1 = min(ss->start.y, ss->end.y) - wr.top;
            int x2 = max(ss->start.x, ss->end.x) - wr.left;
            int y2 = max(ss->start.y, ss->end.y) - wr.top;
            int w = x2 - x1;
            int h = y2 - y1;

            if (w > 0 && h > 0) {
                /* Clear selected area - show original screenshot */
                BitBlt(mem_dc, x1, y1, w, h, ss->snapshot_dc, x1, y1, SRCCOPY);

                /* Cyan border */
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 255, 255));
                SelectObject(mem_dc, pen);
                SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
                Rectangle(mem_dc, x1, y1, x2, y2);
                DeleteObject(pen);

                /* Size label */
                char label[64];
                snprintf(label, sizeof(label), "%d x %d", w, h);
                SetTextColor(mem_dc, RGB(0, 255, 255));
                SetBkMode(mem_dc, TRANSPARENT);
                HFONT font = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Consolas");
                SelectObject(mem_dc, font);
                TextOutA(mem_dc, x1, y1 - 20, label, (int)strlen(label));
                DeleteObject(font);
            }
        }

        /* Blit offscreen to screen in one shot */
        BitBlt(hdc, 0, 0, sw, sh, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static BOOL RunSelectionOverlay(RECT *out_rect)
{
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    /* Take screenshot */
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP snapshot = CreateCompatibleBitmap(screen_dc, sw, sh);
    SelectObject(mem_dc, snapshot);
    BitBlt(mem_dc, 0, 0, sw, sh, screen_dc, 0, 0, SRCCOPY);
    ReleaseDC(NULL, screen_dc);

    /* Register class */
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = SelectionWndProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_CROSS);
        wc.lpszClassName = "FishWindowSel";
        RegisterClassA(&wc);
        registered = TRUE;
    }

    /* Create overlay */
    HWND overlay = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        "FishWindowSel", "FishWindow Selection",
        WS_POPUP,
        0, 0, sw, sh,
        NULL, NULL, g_hinst, NULL
    );

    if (!overlay) {
        DeleteObject(snapshot);
        DeleteDC(mem_dc);
        return FALSE;
    }

    SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);

    SelectionState ss = {0};
    ss.snapshot_dc = mem_dc;
    SetWindowLongPtrA(overlay, GWLP_USERDATA, (LONG_PTR)&ss);

    ShowWindow(overlay, SW_SHOW);
    SetForegroundWindow(overlay);
    SetFocus(overlay);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsWindow(overlay)) break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    DeleteObject(snapshot);
    DeleteDC(mem_dc);

    if (ss.made && out_rect) {
        *out_rect = ss.result;
    }
    return ss.made;
}

/* ======================== Helpers ======================== */

/* Forward declarations — functions defined later in file */
static void UpdateTrayTip(void);
static void SetBorderStatus(const wchar_t *text);
static BOOL ShowWindowPicker(void);
static void RestoreOtherWindow(HWND hwnd, LONG saved_style, LONG saved_ex_style);
static void DoSelectArea(void);

/* Full window redraw flags — used in multiple places */
#define REDRAW_FULL  (RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | \
                      RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASENOW)

/* Initialize state after picking a new target window */
static void InitTargetWindow(void)
{
    GetWindowTextA(g_target_hwnd, g_target_title, sizeof(g_target_title));
    GetWindowRect(g_target_hwnd, &g_last_win_rect);
    g_has_clip = FALSE;
    g_is_topmost = FALSE;
    g_style_saved = FALSE;
    UpdateTrayTip();
}

/* Toggle border visibility */
static void ToggleBorder(void)
{
    g_show_border = !g_show_border;
    if (g_border_hwnd && IsWindow(g_border_hwnd)) {
        ShowWindow(g_border_hwnd, g_show_border ? SW_SHOWNA : SW_HIDE);
    }
    SetBorderStatus(g_show_border ? L"\x8FB9\x6846\x663E\x793A" : L"\x8FB9\x6846\x9690\x85CF");
}

/* Toggle window topmost */
static void ToggleTopmost(void)
{
    g_is_topmost = !g_is_topmost;
    SetWindowPos(g_target_hwnd,
        g_is_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetBorderStatus(g_is_topmost ? L"\x7A97\x53E3\x7F6E\x9876" : L"\x53D6\x6D88\x7F6E\x9876");
}

/* Switch to a new target window, restoring the old one if needed */
static void SwitchTargetWindow(void)
{
    HWND old_hwnd = g_target_hwnd;
    LONG old_style = g_orig_style;
    LONG old_ex_style = g_orig_ex_style;
    BOOL old_clipped = g_has_clip && g_style_saved;
    if (ShowWindowPicker() && g_target_hwnd) {
        if (old_clipped && old_hwnd && old_hwnd != g_target_hwnd && IsWindow(old_hwnd))
            RestoreOtherWindow(old_hwnd, old_style, old_ex_style);
        InitTargetWindow();
    }
}

/* ======================== Border Overlay ======================== */

static void ApplyClipRegion(void); /* forward decl */

static int g_border_width = 3;
static BOOL g_border_dragging = FALSE;
static POINT g_border_drag_start;
static RECT g_border_drag_orig;
static wchar_t g_border_status[64] = {0};
static DWORD g_border_status_time = 0;

static void SetBorderStatus(const wchar_t *text)
{
    wcsncpy(g_border_status, text ? text : L"", 63);
    g_border_status_time = GetTickCount();
    if (g_border_hwnd && IsWindow(g_border_hwnd))
        InvalidateRect(g_border_hwnd, NULL, FALSE);
}

static void ApplyBorderRgn(HWND hwnd)
{
    RECT wr;
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    HRGN hOuter = CreateRectRgn(0, 0, w, h);
    HRGN hInner = CreateRectRgn(g_border_width, g_border_width,
        w - g_border_width, h - g_border_width);
    HRGN hRing = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(hRing, hOuter, hInner, RGN_DIFF);
    SetWindowRgn(hwnd, hRing, TRUE);
    DeleteObject(hOuter);
    DeleteObject(hInner);
    /* hRing is owned by SetWindowRgn after call */
}

static LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        g_border_dragging = TRUE;
        GetCursorPos(&g_border_drag_start);
        GetWindowRect(g_target_hwnd, &g_border_drag_orig);
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_border_dragging) {
            POINT now;
            GetCursorPos(&now);
            int dx = now.x - g_border_drag_start.x;
            int dy = now.y - g_border_drag_start.y;
            /* Move target window */
            MoveWindow(g_target_hwnd,
                g_border_drag_orig.left + dx,
                g_border_drag_orig.top + dy,
                g_border_drag_orig.right - g_border_drag_orig.left,
                g_border_drag_orig.bottom - g_border_drag_orig.top,
                TRUE);
            /* Re-apply clip and update border */
            ApplyClipRegion();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_border_dragging) {
            g_border_dragging = FALSE;
            ReleaseCapture();
            GetWindowRect(g_target_hwnd, &g_last_win_rect);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZEALL));
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;
        HBRUSH cyan = CreateSolidBrush(RGB(0, 220, 255));
        RECT full = {0, 0, w, h};
        FillRect(hdc, &full, cyan);
        DeleteObject(cyan);
        /* Draw status text (fades after 3 seconds) */
        if (g_border_status[0]) {
            DWORD elapsed = GetTickCount() - g_border_status_time;
            if (elapsed < 3000) {
                int alpha = 255 - (int)(elapsed * 255 / 3000);
                SetTextColor(hdc, RGB(alpha, alpha, alpha));
                SetBkMode(hdc, TRANSPARENT);
                HFONT sfont = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Consolas");
                SelectObject(hdc, sfont);
                TextOutW(hdc, 6, 6, g_border_status, (int)wcslen(g_border_status));
                DeleteObject(sfont);
            } else {
                g_border_status[0] = 0;
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void CreateBorderOverlay(RECT *clip_screen)
{
    int bw = g_border_width;
    int w = clip_screen->right - clip_screen->left;
    int h = clip_screen->bottom - clip_screen->top;

    if (g_border_hwnd && IsWindow(g_border_hwnd)) {
        MoveWindow(g_border_hwnd,
            clip_screen->left - bw,
            clip_screen->top - bw,
            w + bw * 2, h + bw * 2, TRUE);
        ApplyBorderRgn(g_border_hwnd);
        return;
    }

    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = BorderWndProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_SIZEALL);
        wc.lpszClassName = "FishWindowBorder";
        RegisterClassA(&wc);
        registered = TRUE;
    }

    g_border_hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        "FishWindowBorder", "",
        WS_POPUP,
        clip_screen->left - bw,
        clip_screen->top - bw,
        w + bw * 2, h + bw * 2,
        NULL, NULL, g_hinst, NULL
    );

    SetLayeredWindowAttributes(g_border_hwnd, 0, 220, LWA_ALPHA);
    ApplyBorderRgn(g_border_hwnd);

    if (g_show_border)
        ShowWindow(g_border_hwnd, SW_SHOWNA);
}

static void UpdateBorderPosition(void)
{
    if (!g_has_clip || !g_target_hwnd || !IsWindow(g_target_hwnd))
        return;

    RECT wr;
    GetWindowRect(g_target_hwnd, &wr);

    RECT clip_screen = {
        wr.left + g_clip_rect_window.left,
        wr.top + g_clip_rect_window.top,
        wr.left + g_clip_rect_window.right,
        wr.top + g_clip_rect_window.bottom
    };

    CreateBorderOverlay(&clip_screen);
}

/* ======================== Window Operations ======================== */

static void ApplyClipRegion(void)
{
    if (!g_target_hwnd || !g_has_clip) return;
    if (!IsWindow(g_target_hwnd)) { g_target_hwnd = NULL; return; }

    /* Hide original window border on first clip */
    if (!g_style_saved) {
        g_orig_style = GetWindowLongA(g_target_hwnd, GWL_STYLE);
        g_orig_ex_style = GetWindowLongA(g_target_hwnd, GWL_EXSTYLE);
        g_style_saved = TRUE;
        LONG new_style = g_orig_style & ~(WS_CAPTION | WS_THICKFRAME |
            WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongA(g_target_hwnd, GWL_STYLE, new_style);
        LONG new_ex = g_orig_ex_style & ~WS_EX_WINDOWEDGE;
        SetWindowLongA(g_target_hwnd, GWL_EXSTYLE, new_ex);
        SetWindowPos(g_target_hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    RECT wr;
    GetWindowRect(g_target_hwnd, &wr);

    int cx1 = max(0, g_clip_rect_window.left);
    int cy1 = max(0, g_clip_rect_window.top);
    int cx2 = min(wr.right - wr.left, g_clip_rect_window.right);
    int cy2 = min(wr.bottom - wr.top, g_clip_rect_window.bottom);

    if (cx2 <= cx1 || cy2 <= cy1) return;

    HRGN hrgn = CreateRectRgn(cx1, cy1, cx2, cy2);
    SetWindowRgn(g_target_hwnd, hrgn, TRUE);

    UpdateBorderPosition();
}

static void RestoreWindow(HWND hwnd)
{
    if (!IsWindow(hwnd)) return;

    /* Restore original window style */
    if (g_style_saved && hwnd == g_target_hwnd) {
        SetWindowLongA(hwnd, GWL_STYLE, g_orig_style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, g_orig_ex_style);
        g_style_saved = FALSE;
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    /* Remove clip region */
    SetWindowRgn(hwnd, NULL, TRUE);

    if (g_is_topmost)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RedrawWindow(hwnd, NULL, NULL, REDRAW_FULL);
}

/* Restore a window that may not be the current target (used when switching) */
static void RestoreOtherWindow(HWND hwnd, LONG saved_style, LONG saved_ex_style)
{
    if (!IsWindow(hwnd)) return;
    SetWindowLongA(hwnd, GWL_STYLE, saved_style);
    SetWindowLongA(hwnd, GWL_EXSTYLE, saved_ex_style);
    SetWindowRgn(hwnd, NULL, TRUE);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, NULL, NULL, REDRAW_FULL);
}

/* ======================== Window Picker Dialog ======================== */

#define MAX_WINDOWS 128
#define PICKER_W 460
#define PICKER_H 400
#define PICKER_RADIUS 12

typedef struct {
    HWND hwnd;
    char title[256];
    char process_name[256];
} WindowInfo;

static WindowInfo g_windows[MAX_WINDOWS];
static int g_window_count = 0;
static HWND g_picker_list = NULL;
static HFONT g_picker_font = NULL;
static HFONT g_picker_title_font = NULL;

/* Dark theme colors */
#define CLR_BG        RGB(32, 32, 40)
#define CLR_TITLE_BG  RGB(40, 40, 52)
#define CLR_LIST_BG   RGB(44, 44, 56)
#define CLR_TEXT      RGB(220, 220, 230)
#define CLR_ACCENT    RGB(0, 200, 255)
#define CLR_BTN_OK    RGB(0, 160, 230)
#define CLR_BTN_OK_H  RGB(0, 190, 255)
#define CLR_BTN_CANCEL RGB(60, 60, 72)
#define CLR_BTN_CANCEL_H RGB(80, 80, 92)
#define CLR_CLOSE     RGB(232, 60, 60)

static BOOL IsSkipClass(const char *cls)
{
    if (strcmp(cls, "Progman") == 0) return TRUE;
    if (strcmp(cls, "Shell_TrayWnd") == 0) return TRUE;
    if (strcmp(cls, "WorkerW") == 0) return TRUE;
    if (strncmp(cls, "IME", 3) == 0) return TRUE;
    if (strncmp(cls, "MSCTF", 5) == 0) return TRUE;
    if (strcmp(cls, "tooltips_class32") == 0) return TRUE;
    if (strcmp(cls, "#32768") == 0) return TRUE;
    return FALSE;
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lp)
{
    (void)lp;  /* unused — callback signature required by EnumWindows */
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    char title[256] = {0};
    GetWindowTextA(hwnd, title, 256);
    if (title[0] == 0) return TRUE;

    char cls[256] = {0};
    GetClassNameA(hwnd, cls, 256);
    if (IsSkipClass(cls)) return TRUE;

    /* Get process name */
    char proc_name[256] = {0};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        GetModuleFileNameExA(hProc, NULL, proc_name, 256);
        CloseHandle(hProc);
        char *slash = strrchr(proc_name, '\\');
        if (slash) memmove(proc_name, slash + 1, strlen(slash + 1) + 1);
    }

    if (g_window_count < MAX_WINDOWS) {
        g_windows[g_window_count].hwnd = hwnd;
        snprintf(g_windows[g_window_count].title, 256, "%s", title);
        snprintf(g_windows[g_window_count].process_name, 256, "%s", proc_name);
        g_window_count++;
    }
    return TRUE;
}

static void RefreshWindowList(void)
{
    g_window_count = 0;
    SendMessageA(g_picker_list, LB_RESETCONTENT, 0, 0);
    EnumWindows(EnumWindowsProc, 0);

    for (int i = 0; i < g_window_count; i++) {
        char display[512];
        if (g_windows[i].process_name[0])
            snprintf(display, sizeof(display), "%s  (%s)", g_windows[i].title, g_windows[i].process_name);
        else
            snprintf(display, sizeof(display), "%s", g_windows[i].title);
        int idx = (int)SendMessageA(g_picker_list, LB_ADDSTRING, 0, (LPARAM)display);
        SendMessageA(g_picker_list, LB_SETITEMDATA, idx, (LPARAM)g_windows[i].hwnd);
    }
    if (g_window_count > 0)
        SendMessageA(g_picker_list, LB_SETCURSEL, 0, 0);
}

#define ID_PICKER_LIST  100
#define ID_PICKER_OK    101
#define ID_PICKER_CANCEL 102
/* ID_PICKER_REFRESH removed — no refresh button created */
#define ID_PICKER_REFRESH 103  /* kept for switch-case compat */

static BOOL g_picker_result = FALSE;
static HWND g_picker_ok_btn = NULL;
static HWND g_picker_cancel_btn = NULL;
static HWND g_picker_close_btn = NULL;
static BOOL g_picker_hover_ok = FALSE;
static BOOL g_picker_hover_cancel = FALSE;
static BOOL g_picker_hover_close = FALSE;

static void PickerApplyRoundCorner(HWND hwnd)
{
    HRGN hRgn = CreateRoundRectRgn(0, 0, PICKER_W, PICKER_H,
        PICKER_RADIUS * 2, PICKER_RADIUS * 2);
    SetWindowRgn(hwnd, hRgn, TRUE);
}

static LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_picker_font = CreateFontA(18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei UI");
        g_picker_title_font = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei UI");

        /* List box - standard, A version for ANSI */
        g_picker_list = CreateWindowA("LISTBOX", "",
            WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
            16, 58, PICKER_W - 32, 270,
            hwnd, (HMENU)ID_PICKER_LIST, g_hinst, NULL);
        SendMessageA(g_picker_list, WM_SETFONT, (WPARAM)g_picker_font, TRUE);

        /* OK button */
        g_picker_ok_btn = CreateWindowA("BUTTON", "",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            PICKER_W - 200, 344, 90, 36,
            hwnd, (HMENU)ID_PICKER_OK, g_hinst, NULL);

        /* Cancel button */
        g_picker_cancel_btn = CreateWindowA("BUTTON", "",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            PICKER_W - 102, 344, 86, 36,
            hwnd, (HMENU)ID_PICKER_CANCEL, g_hinst, NULL);

        /* Close X button (top-right) */
        g_picker_close_btn = CreateWindowA("BUTTON", "",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            PICKER_W - 48, 0, 48, 48,
            hwnd, (HMENU)ID_PICKER_CANCEL, g_hinst, NULL);

        RefreshWindowList();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Background */
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(CLR_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        /* Title bar */
        RECT title_rc = {0, 0, PICKER_W, 48};
        HBRUSH title_bg = CreateSolidBrush(CLR_TITLE_BG);
        FillRect(hdc, &title_rc, title_bg);
        DeleteObject(title_bg);

        /* Title text */
        SetTextColor(hdc, CLR_ACCENT);
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, g_picker_title_font);
        TextOutW(hdc, 16, 12, L"\x263A \x9009\x62E9\x8981\x88C1\x526A\x7684\x7A97\x53E3", 8);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case ID_PICKER_OK:
        case ID_PICKER_LIST:
            if (LOWORD(wp) == ID_PICKER_LIST && HIWORD(wp) != LBN_DBLCLK)
                break;
            {
                int sel = (int)SendMessageA(g_picker_list, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) {
                    g_target_hwnd = (HWND)SendMessageA(g_picker_list, LB_GETITEMDATA, sel, 0);
                    g_picker_result = TRUE;
                    DestroyWindow(hwnd);
                }
            }
            break;
        case ID_PICKER_CANCEL:
            g_picker_result = FALSE;
            DestroyWindow(hwnd);
            break;
        case ID_PICKER_REFRESH:
            RefreshWindowList();
            break;
        }
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_BUTTON) {
            /* Owner-draw button */
            BOOL hover = FALSE;
            COLORREF bg_clr = CLR_BG, txt_clr = CLR_TEXT;
            const wchar_t *label = L"";
            int label_len = 0;
            if (dis->hwndItem == g_picker_ok_btn) {
                hover = g_picker_hover_ok;
                bg_clr = hover ? CLR_BTN_OK_H : CLR_BTN_OK;
                txt_clr = RGB(255,255,255);
                label = L"\x786E\x5B9A"; label_len = 2; /* 确定 */
            } else if (dis->hwndItem == g_picker_cancel_btn) {
                hover = g_picker_hover_cancel;
                bg_clr = hover ? CLR_BTN_CANCEL_H : CLR_BTN_CANCEL;
                txt_clr = CLR_TEXT;
                label = L"\x53D6\x6D88"; label_len = 2; /* 取消 */
            } else if (dis->hwndItem == g_picker_close_btn) {
                hover = g_picker_hover_close;
                bg_clr = hover ? CLR_CLOSE : CLR_TITLE_BG;
                txt_clr = hover ? RGB(255,255,255) : RGB(160,160,180);
                label = L"\x2715"; label_len = 1; /* ✕ */
            }
            HBRUSH bg = CreateSolidBrush(bg_clr);
            FillRect(dis->hDC, &dis->rcItem, bg);
            DeleteObject(bg);
            SetTextColor(dis->hDC, txt_clr);
            SetBkMode(dis->hDC, TRANSPARENT);
            SelectObject(dis->hDC, g_picker_font);
            SIZE sz;
            GetTextExtentPoint32W(dis->hDC, label, label_len, &sz);
            int x = dis->rcItem.left + (dis->rcItem.right - dis->rcItem.left - sz.cx) / 2;
            int y = dis->rcItem.top + (dis->rcItem.bottom - dis->rcItem.top - sz.cy) / 2;
            TextOutW(dis->hDC, x, y, label, label_len);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, CLR_LIST_BG);
        static HBRUSH list_brush = 0;
        if (!list_brush) list_brush = CreateSolidBrush(CLR_LIST_BG);
        return (LRESULT)list_brush;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rc;
        if (g_picker_ok_btn) {
            GetWindowRect(g_picker_ok_btn, &rc);
            MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
            BOOL new_hover = PtInRect(&rc, pt);
            if (new_hover != g_picker_hover_ok) {
                g_picker_hover_ok = new_hover;
                InvalidateRect(g_picker_ok_btn, NULL, TRUE);
            }
        }
        if (g_picker_cancel_btn) {
            GetWindowRect(g_picker_cancel_btn, &rc);
            MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
            BOOL new_hover = PtInRect(&rc, pt);
            if (new_hover != g_picker_hover_cancel) {
                g_picker_hover_cancel = new_hover;
                InvalidateRect(g_picker_cancel_btn, NULL, TRUE);
            }
        }
        if (g_picker_close_btn) {
            GetWindowRect(g_picker_close_btn, &rc);
            MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
            BOOL new_hover = PtInRect(&rc, pt);
            if (new_hover != g_picker_hover_close) {
                g_picker_hover_close = new_hover;
                InvalidateRect(g_picker_close_btn, NULL, TRUE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        /* Allow dragging by title bar */
        if (GET_Y_LPARAM(lp) < 48) {
            ReleaseCapture();
            SendMessageA(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        g_picker_result = FALSE;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_picker_font) { DeleteObject(g_picker_font); g_picker_font = NULL; }
        if (g_picker_title_font) { DeleteObject(g_picker_title_font); g_picker_title_font = NULL; }
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static BOOL ShowWindowPicker(void)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = PickerWndProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(CLR_BG);
        wc.lpszClassName = "FishWindowPicker";
        RegisterClassA(&wc);
        registered = TRUE;
    }

    g_picker_result = FALSE;
    g_picker_hover_ok = FALSE;
    g_picker_hover_cancel = FALSE;
    g_picker_hover_close = FALSE;

    /* Center on screen */
    int sx = (GetSystemMetrics(SM_CXSCREEN) - PICKER_W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - PICKER_H) / 2;

    HWND picker = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        "FishWindowPicker", "FishWindow",
        WS_POPUP,
        sx, sy, PICKER_W, PICKER_H,
        NULL, NULL, g_hinst, NULL
    );

    SetLayeredWindowAttributes(picker, 0, 255, LWA_ALPHA);
    PickerApplyRoundCorner(picker);

    ShowWindow(picker, SW_SHOW);
    SetForegroundWindow(picker);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsWindow(picker)) break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return g_picker_result;
}

/* ======================== Tray Icon ======================== */

static void UpdateTrayTip(void)
{
    wchar_t tip[128];
    if (g_target_hwnd && g_has_clip) {
        wchar_t wtitle[64] = {0};
        MultiByteToWideChar(CP_ACP, 0, g_target_title, -1, wtitle, 64);
        swprintf(tip, 128, L"%ls - \x5DF2\u88C1\u526A", wtitle);
    } else if (g_target_hwnd) {
        wchar_t wtitle[64] = {0};
        MultiByteToWideChar(CP_ACP, 0, g_target_title, -1, wtitle, 64);
        swprintf(tip, 128, L"%ls", wtitle);
    } else {
        wcscpy(tip, L"FishWindow");
    }
    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* ShowNotifyW + ShowNotifyFmt removed — neither was called; add back if needed */

static HICON g_fish_icon = NULL;

static HICON CreateFishIcon(void)
{
    /* Try loading from exe resources first */
    HICON icon = (HICON)LoadImageA(g_hinst, MAKEINTRESOURCEA(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTSIZE);
    if (icon) return icon;

    /* Fallback: load fish.ico from exe directory */
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "fish.ico");
    else strcpy(path, "fish.ico");

    icon = (HICON)LoadImageA(NULL, path, IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (icon) return icon;

    /* Last fallback: default application icon */
    return LoadIconA(NULL, IDI_APPLICATION);
}

static void AddTrayIcon(void)
{
    g_fish_icon = CreateFishIcon();
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_fish_icon;
    wcscpy(g_nid.szTip, L"FishWindow");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* ======================== Tray Menu ======================== */

static void ShowTrayMenu(void)
{
    HMENU hMenu = CreatePopupMenu();

    if (g_target_hwnd) {
        wchar_t wtitle[256] = {0};
        MultiByteToWideChar(CP_ACP, 0, g_target_title, -1, wtitle, 256);
        wchar_t item[256];
        swprintf(item, 256, L"\x7A97\x53E3: %ls", wtitle);
        AppendMenuW(hMenu, MF_STRING, 10, item);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    } else {
        AppendMenuW(hMenu, MF_STRING, 1, L"\x9009\x62E9\x7A97\x53E3...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    }

    AppendMenuW(hMenu, MF_STRING, 2,
        g_has_clip ? L"\x91CD\x65B0\x6846\x9009" : L"\x6846\x9009\x533A\x57DF");

    AppendMenuW(hMenu, MF_STRING | (g_show_border ? MF_CHECKED : 0), 3,
        g_show_border ? L"\x9690\x85CF\x8FB9\x6846" : L"\x663E\x793A\x8FB9\x6846");

    AppendMenuW(hMenu, MF_STRING | (g_is_topmost ? MF_CHECKED : 0), 4,
        g_is_topmost ? L"\x53D6\x6D88\x7F6E\x9876" : L"\x7A97\x53E3\x7F6E\x9876");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 99, L"\x9000\x51FA");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main_hwnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_main_hwnd, NULL);
    DestroyMenu(hMenu);

    switch (cmd) {
    case 10:
    case 1:
        /* Switch target window */
        SwitchTargetWindow();
        break;
    case 2:
        if (g_target_hwnd && IsWindow(g_target_hwnd))
            DoSelectArea();
        else
            SetBorderStatus(L"\x8BF7\x5148\x9009\x62E9\x7A97\x53E3");
        break;
    case 3:
        if (g_has_clip)
            ToggleBorder();
        break;
    case 4:
        if (g_target_hwnd && IsWindow(g_target_hwnd))
            ToggleTopmost();
        break;
    case 99:
        PostMessageA(g_main_hwnd, WM_CLOSE, 0, 0);
        break;
    }
}

/* ======================== Main Window Proc ======================== */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_HOTKEY: {
        switch (wp) {
        case HK_SELECT:
            if (!g_target_hwnd) {
                if (ShowWindowPicker() && g_target_hwnd)
                    InitTargetWindow();
            } else {
                DoSelectArea();
            }
            break;
        case HK_BORDER:
            if (g_has_clip)
                ToggleBorder();
            break;
        case HK_TOPMOST:
            if (g_target_hwnd && IsWindow(g_target_hwnd))
                ToggleTopmost();
            break;
        case HK_QUIT:
            if (g_target_hwnd && IsWindow(g_target_hwnd) && g_has_clip) {
                RestoreWindow(g_target_hwnd);
                g_has_clip = FALSE;
                g_style_saved = FALSE;
                if (g_border_hwnd && IsWindow(g_border_hwnd))
                    ShowWindow(g_border_hwnd, SW_HIDE);
                KillTimer(hwnd, TIMER_TRACK);
                SetBorderStatus(L"\x5DF2\x8FD8\x539F");
                UpdateTrayTip();
            }
            break;
        }
        return 0;
    }
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            if (!g_target_hwnd)
                PostMessageA(hwnd, WM_HOTKEY, HK_SELECT, 0);
            else
                DoSelectArea();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TRACK && g_target_hwnd && g_has_clip) {
            if (!IsWindow(g_target_hwnd)) {
                g_target_hwnd = NULL;
                KillTimer(hwnd, TIMER_TRACK);
                if (g_border_hwnd) ShowWindow(g_border_hwnd, SW_HIDE);
                return 0;
            }
            RECT cur;
            GetWindowRect(g_target_hwnd, &cur);
            if (cur.left != g_last_win_rect.left || cur.top != g_last_win_rect.top ||
                cur.right != g_last_win_rect.right || cur.bottom != g_last_win_rect.bottom) {
                UpdateBorderPosition();
                g_last_win_rect = cur;
            }
        }
        return 0;
    case WM_CLOSE:
        if (g_target_hwnd && IsWindow(g_target_hwnd))
            RestoreWindow(g_target_hwnd);
        if (g_border_hwnd && IsWindow(g_border_hwnd))
            DestroyWindow(g_border_hwnd);
        KillTimer(hwnd, TIMER_TRACK);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void DoSelectArea(void)
{
    if (!g_target_hwnd || !IsWindow(g_target_hwnd)) {
        g_target_hwnd = NULL;
        SetBorderStatus(L"\x7A97\x53E3\x5DF2\x5173\x95ED");
        return;
    }

    if (g_has_clip) {
        SetWindowRgn(g_target_hwnd, NULL, TRUE);
        RedrawWindow(g_target_hwnd, NULL, NULL, REDRAW_FULL);
        Sleep(50);
    }

    RECT sel;
    if (RunSelectionOverlay(&sel)) {
        RECT wr;
        GetWindowRect(g_target_hwnd, &wr);
        g_clip_rect_window.left = sel.left - wr.left;
        g_clip_rect_window.top = sel.top - wr.top;
        g_clip_rect_window.right = sel.right - wr.left;
        g_clip_rect_window.bottom = sel.bottom - wr.top;
        g_has_clip = TRUE;
        ApplyClipRegion();
        GetWindowRect(g_target_hwnd, &g_last_win_rect);
        SetTimer(g_main_hwnd, TIMER_TRACK, 200, NULL);

        int w = sel.right - sel.left;
        int h = sel.bottom - sel.top;
        wchar_t nmsg2[256];
        swprintf(nmsg2, 256, L"\x5DF2\x88C1\x526A (%dx%d)", w, h);
        SetBorderStatus(nmsg2);
        UpdateTrayTip();
    } else if (g_has_clip) {
        ApplyClipRegion();
    }
}

/* ======================== Welcome Dialog ======================== */

#define WELCOME_W 460
#define WELCOME_H 380

static HWND g_welcome_ok_btn = NULL;
static BOOL g_welcome_hover_ok = FALSE;

static LRESULT CALLBACK WelcomeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HFONT wfont = CreateFontA(18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei UI");
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)wfont);

        g_welcome_ok_btn = CreateWindowA("BUTTON", "",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            WELCOME_W/2 - 50, WELCOME_H - 60, 100, 38,
            hwnd, (HMENU)ID_PICKER_OK, g_hinst, NULL);
        g_welcome_hover_ok = FALSE;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HFONT wfont = (HFONT)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

        /* Background */
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(CLR_BG);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        /* Title bar */
        RECT title_rc = {0, 0, WELCOME_W, 48};
        HBRUSH title_bg = CreateSolidBrush(CLR_TITLE_BG);
        FillRect(hdc, &title_rc, title_bg);
        DeleteObject(title_bg);

        /* Title text */
        SetTextColor(hdc, CLR_ACCENT);
        SetBkMode(hdc, TRANSPARENT);
        HFONT title_font = CreateFontA(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei UI");
        SelectObject(hdc, title_font);
        TextOutW(hdc, 16, 12, L"\x263A FishWindow \x5FEB\x6377\x952E", 16);
        DeleteObject(title_font);

        /* Hotkey lines */
        SelectObject(hdc, wfont);
        SetTextColor(hdc, CLR_TEXT);
        const wchar_t *lines[] = {
            L"Ctrl+Alt+F    \x6846\x9009\x533A\x57DF",
            L"Ctrl+Alt+B    \x663E\x793A/\x9690\x85CF\x8FB9\x6846 (\x53EF\x62D6\x52A8\x7A97\x53E3)",
            L"Ctrl+Alt+T    \x7A97\x53E3\x7F6E\x9876",
            L"Ctrl+Alt+Q    \x8FD8\x539F\x7A97\x53E3",
        };
        int y = 68;
        for (int i = 0; i < 4; i++) {
            TextOutW(hdc, 30, y, lines[i], (int)wcslen(lines[i]));
            y += 34;
        }

        /* Tips */
        SetTextColor(hdc, RGB(140, 140, 160));
        HFONT small_font = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei UI");
        SelectObject(hdc, small_font);
        TextOutW(hdc, 30, y + 10, L"\x53F3\x952E\x6258\x76D8\x56FE\x6807\x53EF\x5207\x6362\x7A97\x53E3", 9);
        DeleteObject(small_font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wp) == ID_PICKER_OK) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_BUTTON && dis->hwndItem == g_welcome_ok_btn) {
            BOOL hover = g_welcome_hover_ok;
            COLORREF bg_clr = hover ? CLR_BTN_OK_H : CLR_BTN_OK;
            HBRUSH bg = CreateSolidBrush(bg_clr);
            FillRect(dis->hDC, &dis->rcItem, bg);
            DeleteObject(bg);
            SetTextColor(dis->hDC, RGB(255,255,255));
            SetBkMode(dis->hDC, TRANSPARENT);
            HFONT wfont = (HFONT)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
            SelectObject(dis->hDC, wfont);
            const wchar_t *label = L"\x786E\x5B9A";
            int label_len = 2;
            SIZE sz;
            GetTextExtentPoint32W(dis->hDC, label, label_len, &sz);
            int x = dis->rcItem.left + (dis->rcItem.right - dis->rcItem.left - sz.cx) / 2;
            int y = dis->rcItem.top + (dis->rcItem.bottom - dis->rcItem.top - sz.cy) / 2;
            TextOutW(dis->hDC, x, y, label, label_len);
            return TRUE;
        }
        return FALSE;
    }
    case WM_MOUSEMOVE: {
        if (g_welcome_ok_btn) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetWindowRect(g_welcome_ok_btn, &rc);
            MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
            BOOL new_hover = PtInRect(&rc, pt);
            if (new_hover != g_welcome_hover_ok) {
                g_welcome_hover_ok = new_hover;
                InvalidateRect(g_welcome_ok_btn, NULL, TRUE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (GET_Y_LPARAM(lp) < 48) {
            ReleaseCapture();
            SendMessageA(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        HFONT wfont = (HFONT)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        if (wfont) DeleteObject(wfont);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void ShowWelcomeDialog(void)
{
    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = WelcomeWndProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(CLR_BG);
        wc.lpszClassName = "FishWindowWelcome";
        RegisterClassA(&wc);
        registered = TRUE;
    }

    int sx = (GetSystemMetrics(SM_CXSCREEN) - WELCOME_W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - WELCOME_H) / 2;

    HWND dlg = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        "FishWindowWelcome", "FishWindow",
        WS_POPUP,
        sx, sy, WELCOME_W, WELCOME_H,
        NULL, NULL, g_hinst, NULL
    );

    SetLayeredWindowAttributes(dlg, 0, 255, LWA_ALPHA);
    HRGN hRgn = CreateRoundRectRgn(0, 0, WELCOME_W, WELCOME_H, PICKER_RADIUS*2, PICKER_RADIUS*2);
    SetWindowRgn(dlg, hRgn, TRUE);

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsWindow(dlg)) break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ======================== Entry Point ======================== */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
    (void)hPrev; (void)cmdLine; (void)show;  /* unused — WinMain signature required */
    g_hinst = hInst;

    /* Enable DPI awareness before creating any windows */
    EnableDpiAwareness();

    /* Single instance */
    HANDLE mutex = CreateMutexA(NULL, TRUE, "FishWindow_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"FishWindow \x5DF2\x7ECF\x5728\x8FD0\x884C\x4E86", L"FishWindow", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    /* First-run: show hotkey guide with custom UI */
    ShowWelcomeDialog();

    /* Register main window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "FishWindowMain";
    RegisterClassA(&wc);

    /* Create hidden message window */
    g_main_hwnd = CreateWindowA(
        "FishWindowMain", "FishWindow",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, hInst, NULL
    );

    /* Register hotkeys */
    UINT mod = MOD_CONTROL | MOD_ALT;
    RegisterHotKey(g_main_hwnd, HK_SELECT, mod, 'F');
    RegisterHotKey(g_main_hwnd, HK_BORDER, mod, 'B');
    RegisterHotKey(g_main_hwnd, HK_TOPMOST, mod, 'T');
    RegisterHotKey(g_main_hwnd, HK_QUIT, mod, 'Q');

    /* Add tray icon */
    AddTrayIcon();

    /* Auto-show window picker on first run */
    PostMessageA(g_main_hwnd, WM_HOTKEY, HK_SELECT, 0);

    /* Message loop */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Cleanup */
    UnregisterHotKey(g_main_hwnd, HK_SELECT);
    UnregisterHotKey(g_main_hwnd, HK_BORDER);
    UnregisterHotKey(g_main_hwnd, HK_TOPMOST);
    UnregisterHotKey(g_main_hwnd, HK_QUIT);
    ReleaseMutex(mutex);
    CloseHandle(mutex);

    return 0;
}
