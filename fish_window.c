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
#include <uxtheme.h>
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

#define MAX_CLIP_ENTRIES 16

typedef struct ClipEntry {
    HWND target_hwnd;
    char target_title[256];
    BOOL has_clip;
    BOOL show_border;
    BOOL is_topmost;
    RECT clip_rect_window;
    RECT last_win_rect;
    HWND border_hwnd;
    LONG orig_style;
    LONG orig_ex_style;
    BOOL style_saved;
} ClipEntry;

static ClipEntry g_clips[MAX_CLIP_ENTRIES];
static int g_clip_count = 0;
static int g_cur_idx = -1;  /* current active clip, -1 = none */

/* Current window state — used by all existing code */
static HWND g_target_hwnd = NULL;
static char g_target_title[256] = {0};
static BOOL g_has_clip = FALSE;
static BOOL g_show_border = FALSE;
static BOOL g_is_topmost = FALSE;
static RECT g_clip_rect_window = {0};
static RECT g_last_win_rect = {0};
static HWND g_border_hwnd = NULL;
static LONG g_orig_style = 0;
static LONG g_orig_ex_style = 0;
static BOOL g_style_saved = FALSE;

static HWND g_main_hwnd = NULL;
static HINSTANCE g_hinst = NULL;
static NOTIFYICONDATAW g_nid = {0};

/* Save current globals into g_clips[g_cur_idx] */
static void SaveCurClip(void)
{
    if (g_cur_idx < 0 || g_cur_idx >= g_clip_count) return;
    ClipEntry *e = &g_clips[g_cur_idx];
    e->target_hwnd = g_target_hwnd;
    memcpy(e->target_title, g_target_title, sizeof(g_target_title));
    e->has_clip = g_has_clip;
    e->show_border = g_show_border;
    e->is_topmost = g_is_topmost;
    e->clip_rect_window = g_clip_rect_window;
    e->last_win_rect = g_last_win_rect;
    e->border_hwnd = g_border_hwnd;
    e->orig_style = g_orig_style;
    e->orig_ex_style = g_orig_ex_style;
    e->style_saved = g_style_saved;
}

/* Load g_clips[idx] into globals, set g_cur_idx */
static void LoadClip(int idx)
{
    if (idx < 0 || idx >= g_clip_count) {
        g_target_hwnd = NULL;
        g_target_title[0] = 0;
        g_has_clip = FALSE;
        g_show_border = FALSE;
        g_is_topmost = FALSE;
        memset(&g_clip_rect_window, 0, sizeof(g_clip_rect_window));
        memset(&g_last_win_rect, 0, sizeof(g_last_win_rect));
        g_border_hwnd = NULL;
        g_orig_style = 0;
        g_orig_ex_style = 0;
        g_style_saved = FALSE;
        g_cur_idx = -1;
        return;
    }
    g_cur_idx = idx;
    ClipEntry *e = &g_clips[idx];
    g_target_hwnd = e->target_hwnd;
    memcpy(g_target_title, e->target_title, sizeof(g_target_title));
    g_has_clip = e->has_clip;
    g_show_border = e->show_border;
    g_is_topmost = e->is_topmost;
    g_clip_rect_window = e->clip_rect_window;
    g_last_win_rect = e->last_win_rect;
    g_border_hwnd = e->border_hwnd;
    g_orig_style = e->orig_style;
    g_orig_ex_style = e->orig_ex_style;
    g_style_saved = e->style_saved;
}

/* Find clip by hwnd, returns index or -1 */
static int FindClipIdx(HWND hwnd)
{
    for (int i = 0; i < g_clip_count; i++)
        if (g_clips[i].target_hwnd == hwnd) return i;
    return -1;
}

/* Find free slot, returns index or -1 */
static int FindFreeSlot(void)
{
    for (int i = 0; i < MAX_CLIP_ENTRIES; i++)
        if (g_clips[i].target_hwnd == NULL) return i;
    return -1;
}

/* Remove dead entries, fix g_cur_idx */
static void CompactClips(void)
{
    HWND cur_hwnd = g_target_hwnd;
    int write = 0;
    for (int i = 0; i < g_clip_count; i++) {
        ClipEntry *e = &g_clips[i];
        if (e->target_hwnd && IsWindow(e->target_hwnd)) {
            if (i != write) g_clips[write] = *e;
            write++;
        } else {
            if (e->border_hwnd && IsWindow(e->border_hwnd))
                DestroyWindow(e->border_hwnd);
        }
    }
    g_clip_count = write;
    /* Reload current if still alive */
    int new_idx = FindClipIdx(cur_hwnd);
    if (new_idx >= 0) LoadClip(new_idx);
    else LoadClip(-1);
}

/* Get virtual desktop bounds (covers all monitors) */
static void GetVirtualDesktop(RECT *vdesk)
{
    vdesk->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vdesk->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vdesk->right  = vdesk->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vdesk->bottom = vdesk->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* Center a rect within the virtual desktop */
static POINT CenterInVirtualDesktop(int width, int height)
{
    RECT vdesk;
    GetVirtualDesktop(&vdesk);
    POINT pt;
    pt.x = vdesk.left + (vdesk.right - vdesk.left - width) / 2;
    pt.y = vdesk.top + (vdesk.bottom - vdesk.top - height) / 2;
    return pt;
}

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

        RECT vdesk;
        GetVirtualDesktop(&vdesk);
        int vw = vdesk.right - vdesk.left;
        int vh = vdesk.bottom - vdesk.top;

        /* Double-buffer: draw to offscreen DC first */
        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, vw, vh);
        HBITMAP old_bmp = SelectObject(mem_dc, mem_bmp);

        /* Draw screenshot */
        BitBlt(mem_dc, 0, 0, vw, vh, ss->snapshot_dc, 0, 0, SRCCOPY);

        /* Dark overlay using AlphaBlend */
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 140, 0};
        HDC dark_dc = CreateCompatibleDC(mem_dc);
        HBITMAP dark_bmp = CreateCompatibleBitmap(mem_dc, vw, vh);
        SelectObject(dark_dc, dark_bmp);
        RECT full = {0, 0, vw, vh};
        HBRUSH dark_brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dark_dc, &full, dark_brush);
        AlphaBlend(mem_dc, 0, 0, vw, vh, dark_dc, 0, 0, vw, vh, bf);
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
        BitBlt(hdc, 0, 0, vw, vh, mem_dc, 0, 0, SRCCOPY);
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
    RECT vdesk;
    GetVirtualDesktop(&vdesk);
    int vw = vdesk.right - vdesk.left;
    int vh = vdesk.bottom - vdesk.top;

    /* Take screenshot of entire virtual desktop */
    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    HBITMAP snapshot = CreateCompatibleBitmap(screen_dc, vw, vh);
    SelectObject(mem_dc, snapshot);
    BitBlt(mem_dc, 0, 0, vw, vh, screen_dc, vdesk.left, vdesk.top, SRCCOPY);
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

    /* Create overlay covering entire virtual desktop */
    HWND overlay = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        "FishWindowSel", "FishWindow Selection",
        WS_POPUP,
        vdesk.left, vdesk.top, vw, vh,
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
    SaveCurClip();
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
    SaveCurClip();
}

/* Toggle window topmost */
static void ToggleTopmost(void)
{
    g_is_topmost = !g_is_topmost;
    SetWindowPos(g_target_hwnd,
        g_is_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    /* Sync border Z-order with target window */
    if (g_border_hwnd && IsWindow(g_border_hwnd)) {
        if (g_is_topmost)
            SetWindowPos(g_border_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        else
            SetWindowPos(g_border_hwnd, g_target_hwnd, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetBorderStatus(g_is_topmost ? L"\x7A97\x53E3\x7F6E\x9876" : L"\x53D6\x6D88\x7F6E\x9876");
    SaveCurClip();
}

/* Switch to a new target window (add new clip entry) */
static void SwitchTargetWindow(void)
{
    /* Save current clip state */
    SaveCurClip();

    if (ShowWindowPicker() && g_target_hwnd) {
        HWND new_hwnd = g_target_hwnd;  /* picker sets this */

        /* Check if this window already has a clip entry */
        int existing = FindClipIdx(new_hwnd);
        if (existing >= 0) {
            /* Switch to existing entry */
            LoadClip(existing);
        } else {
            /* Create new clip entry */
            int slot = FindFreeSlot();
            if (slot < 0) {
                /* No free slots — compact and try again */
                CompactClips();
                slot = FindFreeSlot();
            }
            if (slot >= 0) {
                memset(&g_clips[slot], 0, sizeof(ClipEntry));
                if (slot >= g_clip_count) g_clip_count = slot + 1;
                g_clips[slot].target_hwnd = new_hwnd;
                LoadClip(slot);
                InitTargetWindow();
                /* Start selection directly (no auto-topmost) */
                DoSelectArea();
            }
        }
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
    HWND target = (HWND)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_LBUTTONDOWN: {
        g_border_dragging = TRUE;
        GetCursorPos(&g_border_drag_start);
        GetWindowRect(target, &g_border_drag_orig);
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
            MoveWindow(target,
                g_border_drag_orig.left + dx,
                g_border_drag_orig.top + dy,
                g_border_drag_orig.right - g_border_drag_orig.left,
                g_border_drag_orig.bottom - g_border_drag_orig.top,
                TRUE);
            /* Update border position directly (don't use ApplyClipRegion
               which operates on the global current clip) */
            int idx = FindClipIdx(target);
            if (idx >= 0) {
                ClipEntry *e = &g_clips[idx];
                RECT wr;
                GetWindowRect(target, &wr);
                e->last_win_rect = wr;
                if (e->border_hwnd && IsWindow(e->border_hwnd)) {
                    MoveWindow(e->border_hwnd,
                        wr.left + e->clip_rect_window.left - g_border_width,
                        wr.top + e->clip_rect_window.top - g_border_width,
                        e->clip_rect_window.right - e->clip_rect_window.left + g_border_width * 2,
                        e->clip_rect_window.bottom - e->clip_rect_window.top + g_border_width * 2,
                        TRUE);
                }
                if (idx == g_cur_idx) {
                    g_last_win_rect = wr;
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_border_dragging) {
            g_border_dragging = FALSE;
            ReleaseCapture();
            RECT wr;
            GetWindowRect(target, &wr);
            int idx = FindClipIdx(target);
            if (idx >= 0) {
                g_clips[idx].last_win_rect = wr;
                if (idx == g_cur_idx)
                    g_last_win_rect = wr;
            }
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
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        "FishWindowBorder", "",
        WS_POPUP,
        clip_screen->left - bw,
        clip_screen->top - bw,
        w + bw * 2, h + bw * 2,
        NULL, NULL, g_hinst, NULL
    );
    /* Store target hwnd so BorderWndProc uses the right window */
    SetWindowLongPtrA(g_border_hwnd, GWLP_USERDATA, (LONG_PTR)g_target_hwnd);

    SetLayeredWindowAttributes(g_border_hwnd, 0, 220, LWA_ALPHA);
    ApplyBorderRgn(g_border_hwnd);

    /* Place border just above target window in Z-order (not always-on-top) */
    SetWindowPos(g_border_hwnd, g_target_hwnd, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

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
    SaveCurClip();
}

static void RestoreWindow(HWND hwnd)
{
    if (!IsWindow(hwnd)) return;

    /* Find the clip entry for this window to get saved styles */
    int idx = FindClipIdx(hwnd);
    if (idx >= 0) {
        ClipEntry *e = &g_clips[idx];
        if (e->style_saved) {
            SetWindowLongA(hwnd, GWL_STYLE, e->orig_style);
            SetWindowLongA(hwnd, GWL_EXSTYLE, e->orig_ex_style);
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        if (e->is_topmost)
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        /* Destroy border overlay */
        if (e->border_hwnd && IsWindow(e->border_hwnd))
            DestroyWindow(e->border_hwnd);
        /* Remove entry */
        memset(e, 0, sizeof(ClipEntry));
    } else {
        /* Fallback: use current globals if this is the current target */
        if (g_style_saved && hwnd == g_target_hwnd) {
            SetWindowLongA(hwnd, GWL_STYLE, g_orig_style);
            SetWindowLongA(hwnd, GWL_EXSTYLE, g_orig_ex_style);
            g_style_saved = FALSE;
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        if (g_is_topmost)
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    /* Remove clip region */
    SetWindowRgn(hwnd, NULL, TRUE);
    RedrawWindow(hwnd, NULL, NULL, REDRAW_FULL);

    /* Compact and reload */
    CompactClips();
}

/* Restore a window that may not be the current target (used when switching) */
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
static WNDPROC g_list_orig_proc = NULL;  /* original listbox wndproc */

/* Dark scrollbar colors — visible but not jarring */
#define CLR_SB_TRACK    RGB(50, 50, 64)   /* scrollbar track */
#define CLR_SB_THUMB   RGB(80, 80, 100)  /* scrollbar thumb */
#define CLR_SB_HOVER   RGB(100, 100, 120) /* thumb hover */
#define CLR_SB_ARROW   RGB(140, 140, 160) /* arrow button color */
#define CLR_SB_ARROWBG RGB(50, 50, 64)   /* arrow button background */

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

/* Force redraw of the custom scrollbar */
static void RedrawDarkScrollbar(HWND hwnd)
{
    RECT rc;
    GetWindowRect(hwnd, &rc);
    HDC hdc = GetWindowDC(hwnd);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    /* Scrollbar geometry */
    int sbw = GetSystemMetrics(SM_CXVSCROLL);
    int sb_left = w - sbw;

    /* Get scroll info */
    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(hwnd, SB_VERT, &si);

    int arrow_h = sbw;
    int track_h = h - arrow_h * 2;
    int thumb_h = 0, thumb_y = 0;
    if (si.nMax > si.nMin && track_h > 0) {
        int range = si.nMax - si.nMin + 1 - (int)si.nPage;
        if (range <= 0) range = 1;
        thumb_h = (int)((double)si.nPage / (si.nMax - si.nMin + 1) * track_h);
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > track_h) thumb_h = track_h;
        thumb_y = arrow_h + (int)((double)(si.nPos - si.nMin) / range * (track_h - thumb_h));
    }

    /* Paint track background */
    HBRUSH track_brush = CreateSolidBrush(CLR_SB_TRACK);
    RECT track_rc = {sb_left, arrow_h, w, h - arrow_h};
    FillRect(hdc, &track_rc, track_brush);
    DeleteObject(track_brush);

    /* Paint top arrow button */
    HBRUSH arrow_brush = CreateSolidBrush(CLR_SB_ARROWBG);
    RECT top_arrow = {sb_left, 0, w, arrow_h};
    FillRect(hdc, &top_arrow, arrow_brush);
    SetTextColor(hdc, CLR_SB_ARROW);
    SetBkMode(hdc, TRANSPARENT);
    HFONT arrow_font = CreateFontA(sbw - 8, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Marlett");
    SelectObject(hdc, arrow_font);
    SIZE asz;
    const wchar_t *up_arrow = L"\x35";
    GetTextExtentPoint32W(hdc, up_arrow, 1, &asz);
    TextOutW(hdc, sb_left + (sbw - asz.cx) / 2, (arrow_h - asz.cy) / 2, up_arrow, 1);

    /* Paint bottom arrow button */
    RECT bot_arrow = {sb_left, h - arrow_h, w, h};
    FillRect(hdc, &bot_arrow, arrow_brush);
    const wchar_t *dn_arrow = L"\x36";
    GetTextExtentPoint32W(hdc, dn_arrow, 1, &asz);
    TextOutW(hdc, sb_left + (sbw - asz.cx) / 2, h - arrow_h + (arrow_h - asz.cy) / 2, dn_arrow, 1);
    DeleteObject(arrow_font);
    DeleteObject(arrow_brush);

    /* Paint thumb */
    if (thumb_h > 0) {
        HBRUSH thumb_brush = CreateSolidBrush(CLR_SB_THUMB);
        RECT thumb_rc = {sb_left + 2, thumb_y, w - 2, thumb_y + thumb_h};
        FillRect(hdc, &thumb_rc, thumb_brush);
        DeleteObject(thumb_brush);
    }

    /* Paint left border (replacing WS_BORDER) */
    HBRUSH border_brush = CreateSolidBrush(RGB(60, 60, 76));
    RECT left_border = {0, 0, 1, h};
    FillRect(hdc, &left_border, border_brush);
    DeleteObject(border_brush);

    ReleaseDC(hwnd, hdc);
}

/* Subclass listbox to custom-draw scrollbar */
static LRESULT CALLBACK DarkListboxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCPAINT:
    case WM_PRINT:
    case WM_PAINT:
    {
        LRESULT ret = CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
        RedrawDarkScrollbar(hwnd);
        return ret;
    }
    /* Handle scrollbar clicks ourselves to prevent system white flash */
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int sbw = GetSystemMetrics(SM_CXVSCROLL);
        int sb_left = rc.right - sbw;

        if (pt.x >= sb_left) {
            int h = rc.bottom;
            int arrow_h = sbw;
            SCROLLINFO si = {0};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int track_h = h - arrow_h * 2;
            int thumb_h = 0, thumb_y = 0;
            if (si.nMax > si.nMin && track_h > 0) {
                int range = si.nMax - si.nMin + 1 - (int)si.nPage;
                if (range <= 0) range = 1;
                thumb_h = (int)((double)si.nPage / (si.nMax - si.nMin + 1) * track_h);
                if (thumb_h < 20) thumb_h = 20;
                if (thumb_h > track_h) thumb_h = track_h;
                thumb_y = arrow_h + (int)((double)(si.nPos - si.nMin) / range * (track_h - thumb_h));
            }
            int cmd = SB_ENDSCROLL;
            if (pt.y < arrow_h) cmd = SB_LINEUP;
            else if (pt.y >= h - arrow_h) cmd = SB_LINEDOWN;
            else if (thumb_h > 0 && pt.y >= thumb_y && pt.y < thumb_y + thumb_h) {
                /* Thumb drag — let system handle but repaint after */
                LRESULT ret = CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
                RedrawDarkScrollbar(hwnd);
                return ret;
            }
            else if (pt.y < thumb_y) cmd = SB_PAGEUP;
            else cmd = SB_PAGEDOWN;
            SendMessageA(hwnd, WM_VSCROLL, MAKELONG(cmd, 0), 0);
            RedrawDarkScrollbar(hwnd);
            return 0;
        }
        /* Not in scrollbar — pass through */
        LRESULT ret = CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
        RedrawDarkScrollbar(hwnd);
        return ret;
    }
    case WM_NCLBUTTONUP:
    case WM_NCMOUSEMOVE:
    {
        LRESULT ret = CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
        RedrawDarkScrollbar(hwnd);
        return ret;
    }
    case WM_VSCROLL:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    {
        LRESULT ret = CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
        RedrawDarkScrollbar(hwnd);
        return ret;
    }
    case WM_TIMER:
    {
        LRESULT ret = CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
        RedrawDarkScrollbar(hwnd);
        return ret;
    }
    }
    return CallWindowProcA(g_list_orig_proc, hwnd, msg, wp, lp);
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
#define ID_PICKER_REFRESH 103

static BOOL g_picker_result = FALSE;
static HWND g_picker_ok_btn = NULL;
static HWND g_picker_cancel_btn = NULL;
static HWND g_picker_refresh_btn = NULL;
static HWND g_picker_close_btn = NULL;
static BOOL g_picker_hover_ok = FALSE;
static BOOL g_picker_hover_cancel = FALSE;
static BOOL g_picker_hover_refresh = FALSE;
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
            WS_VISIBLE | WS_CHILD | LBS_NOTIFY | WS_VSCROLL,
            16, 58, PICKER_W - 32, 270,
            hwnd, (HMENU)ID_PICKER_LIST, g_hinst, NULL);
        SendMessageA(g_picker_list, WM_SETFONT, (WPARAM)g_picker_font, TRUE);
        /* Remove theme so WM_CTLCOLORSCROLLBAR works for dark scrollbar */
        SetWindowTheme(g_picker_list, L"", L"");
        /* Subclass listbox to custom-draw scrollbar */
        g_list_orig_proc = (WNDPROC)SetWindowLongPtrA(g_picker_list, GWLP_WNDPROC, (LONG_PTR)DarkListboxProc);

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

        /* Refresh button */
        g_picker_refresh_btn = CreateWindowA("BUTTON", "",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            16, 344, 90, 36,
            hwnd, (HMENU)ID_PICKER_REFRESH, g_hinst, NULL);

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
        TextOutW(hdc, 16, 12, L"\x263A \x9009\x62E9\x8981\x88C1\x526A\x7684\x7A97\x53E3", 10);

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
            BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg_clr = CLR_BG, txt_clr = CLR_TEXT;
            const wchar_t *label = L"";
            int label_len = 0;
            int btn_radius = 6;  /* round corner radius */
            if (dis->hwndItem == g_picker_ok_btn) {
                hover = g_picker_hover_ok;
                if (pressed) bg_clr = RGB(0, 90, 60);
                else bg_clr = hover ? CLR_BTN_OK_H : CLR_BTN_OK;
                txt_clr = RGB(255,255,255);
                label = L"\x786E\x5B9A"; label_len = 2; /* 确定 */
            } else if (dis->hwndItem == g_picker_cancel_btn) {
                hover = g_picker_hover_cancel;
                if (pressed) bg_clr = RGB(100, 100, 120);
                else bg_clr = hover ? CLR_BTN_CANCEL_H : CLR_BTN_CANCEL;
                txt_clr = CLR_TEXT;
                label = L"\x53D6\x6D88"; label_len = 2; /* 取消 */
            } else if (dis->hwndItem == g_picker_refresh_btn) {
                hover = g_picker_hover_refresh;
                if (pressed) bg_clr = RGB(40, 40, 60);
                else bg_clr = hover ? CLR_BTN_OK_H : RGB(80, 80, 100);
                txt_clr = RGB(255,255,255);
                label = L"\x5237\x65B0"; label_len = 2; /* 刷新 */
            } else if (dis->hwndItem == g_picker_close_btn) {
                hover = g_picker_hover_close;
                bg_clr = hover ? CLR_CLOSE : CLR_TITLE_BG;
                txt_clr = hover ? RGB(255,255,255) : RGB(160,160,180);
                label = L"\x2715"; label_len = 1; /* ✕ */
                btn_radius = 0;  /* close button: no rounding */
            }
            /* Draw rounded rect background */
            if (btn_radius > 0) {
                /* Fill corners with parent background to avoid white edges */
                HBRUSH parent_bg = CreateSolidBrush(CLR_BG);
                FillRect(dis->hDC, &dis->rcItem, parent_bg);
                DeleteObject(parent_bg);
                /* Draw rounded button */
                HRGN rgn = CreateRoundRectRgn(
                    dis->rcItem.left, dis->rcItem.top,
                    dis->rcItem.right, dis->rcItem.bottom,
                    btn_radius * 2, btn_radius * 2);
                HBRUSH bg = CreateSolidBrush(bg_clr);
                FillRgn(dis->hDC, rgn, bg);
                DeleteObject(bg);
                DeleteObject(rgn);
            } else {
                HBRUSH bg = CreateSolidBrush(bg_clr);
                FillRect(dis->hDC, &dis->rcItem, bg);
                DeleteObject(bg);
            }
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
    case WM_CTLCOLORSCROLLBAR: {
        /* Scrollbar background — slightly lighter than list bg */
        static HBRUSH sb_brush = 0;
        if (!sb_brush) sb_brush = CreateSolidBrush(RGB(50, 50, 64));
        return (LRESULT)sb_brush;
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
        if (g_picker_refresh_btn) {
            GetWindowRect(g_picker_refresh_btn, &rc);
            MapWindowPoints(NULL, hwnd, (POINT*)&rc, 2);
            BOOL new_hover = PtInRect(&rc, pt);
            if (new_hover != g_picker_hover_refresh) {
                g_picker_hover_refresh = new_hover;
                InvalidateRect(g_picker_refresh_btn, NULL, TRUE);
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
    POINT center = CenterInVirtualDesktop(PICKER_W, PICKER_H);
    int sx = center.x;
    int sy = center.y;

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
    if (g_clip_count > 1) {
        if (g_target_hwnd && g_has_clip) {
            wchar_t wtitle[64] = {0};
            MultiByteToWideChar(CP_ACP, 0, g_target_title, -1, wtitle, 64);
            swprintf(tip, 128, L"%ls - \x5DF2\x88C1\x526A (%d)", wtitle, g_clip_count);
        } else {
            swprintf(tip, 128, L"FishWindow (%d)", g_clip_count);
        }
    } else if (g_target_hwnd && g_has_clip) {
        wchar_t wtitle[64] = {0};
        MultiByteToWideChar(CP_ACP, 0, g_target_title, -1, wtitle, 64);
        swprintf(tip, 128, L"%ls - \x5DF2\x88C1\x526A", wtitle);
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
    if (g_fish_icon) { DestroyIcon(g_fish_icon); g_fish_icon = NULL; }
}

/* ======================== Tray Menu ======================== */

static void ShowTrayMenu(void)
{
    HMENU hMenu = CreatePopupMenu();

    /* List all clipped windows (only those with has_clip) */
    int clipped_count = 0;
    for (int i = 0; i < g_clip_count; i++) {
        ClipEntry *e = &g_clips[i];
        if (!e->target_hwnd || !IsWindow(e->target_hwnd)) continue;
        if (!e->has_clip) continue;
        clipped_count++;
        wchar_t wtitle[64] = {0};
        MultiByteToWideChar(CP_ACP, 0, e->target_title, -1, wtitle, 64);
        wchar_t item[128];
        swprintf(item, 128, L"%ls%s", wtitle,
            (i == g_cur_idx) ? L" \x2190" : L"");  /* arrow marks current */
        /* Menu IDs 100+i for clip entries */
        AppendMenuW(hMenu, MF_STRING | (i == g_cur_idx ? MF_CHECKED : 0),
            100 + i, item);
    }
    if (clipped_count > 0) {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    }

    AppendMenuW(hMenu, MF_STRING, 1, L"\x9009\x62E9\x7A97\x53E3...");

    if (g_target_hwnd) {
        AppendMenuW(hMenu, MF_STRING, 2,
            g_has_clip ? L"\x91CD\x65B0\x6846\x9009" : L"\x6846\x9009\x533A\x57DF");

        AppendMenuW(hMenu, MF_STRING | (g_show_border ? MF_CHECKED : 0), 3,
            g_show_border ? L"\x9690\x85CF\x8FB9\x6846" : L"\x663E\x793A\x8FB9\x6846");

        AppendMenuW(hMenu, MF_STRING | (g_is_topmost ? MF_CHECKED : 0), 4,
            g_is_topmost ? L"\x53D6\x6D88\x7F6E\x9876" : L"\x7A97\x53E3\x7F6E\x9876");

        if (g_has_clip) {
            AppendMenuW(hMenu, MF_STRING, 5, L"\x8FD8\x539F\x7A97\x53E3");
        }

        AppendMenuW(hMenu, MF_STRING, 6, L"\x53D6\x6D88\x9009\x4E2D");  /* 取消选中 */
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 99, L"\x9000\x51FA");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main_hwnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, g_main_hwnd, NULL);
    DestroyMenu(hMenu);

    if (cmd >= 100 && cmd < 100 + g_clip_count) {
        /* Switch to a different clip entry */
        int idx = cmd - 100;
        SaveCurClip();
        LoadClip(idx);
        UpdateTrayTip();
    } else switch (cmd) {
    case 1:
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
    case 5:
        /* Restore current window */
        if (g_target_hwnd && IsWindow(g_target_hwnd)) {
            SaveCurClip();
            RestoreWindow(g_target_hwnd);
            UpdateTrayTip();
        }
        break;
    case 6:
        /* Deselect current window — remove entry if not clipped */
        if (g_target_hwnd) {
            if (!g_has_clip) {
                /* No clip applied — just remove the entry */
                if (g_cur_idx >= 0 && g_cur_idx < g_clip_count)
                    memset(&g_clips[g_cur_idx], 0, sizeof(ClipEntry));
                CompactClips();
                LoadClip(-1);
            } else {
                /* Has clip — just switch focus, keep the entry */
                SaveCurClip();
                /* Try to switch to another entry */
                int next = -1;
                for (int i = 0; i < g_clip_count; i++) {
                    if (i != g_cur_idx && g_clips[i].target_hwnd && g_clips[i].has_clip) {
                        next = i;
                        break;
                    }
                }
                LoadClip(next);
            }
            UpdateTrayTip();
        }
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
        {
            /* Smart select: use foreground window if possible */
            HWND fg = GetForegroundWindow();
            /* Check if foreground is one of our own windows */
            char fg_class[256] = {0};
            if (fg) GetClassNameA(fg, fg_class, sizeof(fg_class));
            BOOL is_our_window = (fg == g_main_hwnd) || (fg == g_border_hwnd) ||
                (strcmp(fg_class, "FishWindowPicker") == 0) ||
                (strcmp(fg_class, "FishWindowWelcome") == 0) ||
                (strcmp(fg_class, "FishWindowBorder") == 0) ||
                (strcmp(fg_class, "FishWindowSelection") == 0);

            if (fg && !is_our_window) {
                int existing = FindClipIdx(fg);
                if (existing >= 0) {
                    /* Foreground window already clipped — re-select it */
                    SaveCurClip();
                    LoadClip(existing);
                    DoSelectArea();
                } else {
                    /* Foreground window not clipped — select it directly */
                    SaveCurClip();
                    int slot = FindFreeSlot();
                    if (slot < 0) { CompactClips(); slot = FindFreeSlot(); }
                    if (slot >= 0) {
                        memset(&g_clips[slot], 0, sizeof(ClipEntry));
                        if (slot >= g_clip_count) g_clip_count = slot + 1;
                        g_clips[slot].target_hwnd = fg;
                        g_cur_idx = slot;
                        LoadClip(slot);
                        InitTargetWindow();
                        DoSelectArea();
                    }
                }
            } else if (g_target_hwnd) {
                /* Foreground is our window but we have a current clip — re-select */
                DoSelectArea();
            } else {
                /* No foreground window or first launch — show picker */
                if (ShowWindowPicker() && g_target_hwnd) {
                    int slot = FindFreeSlot();
                    if (slot >= 0) {
                        memset(&g_clips[slot], 0, sizeof(ClipEntry));
                        if (slot >= g_clip_count) g_clip_count = slot + 1;
                        g_cur_idx = slot;
                        InitTargetWindow();
                        DoSelectArea();
                    }
                }
            }
            break;
        }
        case HK_BORDER:
        case HK_TOPMOST:
        case HK_QUIT:
        {
            /* All hotkeys operate on foreground window's clip entry */
            HWND fg = GetForegroundWindow();
            int fg_idx = -1;
            if (fg && fg != g_main_hwnd && fg != g_border_hwnd)
                fg_idx = FindClipIdx(fg);
            /* If foreground not found, fall back to current clip */
            if (fg_idx < 0 && g_target_hwnd)
                fg_idx = g_cur_idx;
            if (fg_idx < 0) break;

            /* Switch to that clip entry */
            SaveCurClip();
            LoadClip(fg_idx);

            switch (wp) {
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
                    if (g_clip_count == 0) KillTimer(hwnd, TIMER_TRACK);
                    SetBorderStatus(L"\x5DF2\x8FD8\x539F");
                    UpdateTrayTip();
                }
                break;
            }
            break;
        }
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
        if (wp == TIMER_TRACK) {
            /* Poll all clip entries for window moves */
            BOOL any_alive = FALSE, need_compact = FALSE;
            for (int i = 0; i < g_clip_count; i++) {
                ClipEntry *e = &g_clips[i];
                if (!e->target_hwnd || !IsWindow(e->target_hwnd)) {
                    /* Window died — clean up its border */
                    if (e->border_hwnd && IsWindow(e->border_hwnd))
                        DestroyWindow(e->border_hwnd);
                    memset(e, 0, sizeof(ClipEntry));
                    need_compact = TRUE;
                    continue;
                }
                any_alive = TRUE;
                if (!e->has_clip) continue;

                /* Keep border Z-order just above target window */
                if (e->border_hwnd && IsWindow(e->border_hwnd) && IsWindowVisible(e->border_hwnd)) {
                    SetWindowPos(e->border_hwnd, e->target_hwnd, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }

                RECT cur;
                GetWindowRect(e->target_hwnd, &cur);
                if (cur.left != e->last_win_rect.left || cur.top != e->last_win_rect.top ||
                    cur.right != e->last_win_rect.right || cur.bottom != e->last_win_rect.bottom) {
                    /* If this is the current clip, update globals too */
                    if (i == g_cur_idx) {
                        g_last_win_rect = cur;
                        UpdateBorderPosition();
                    } else {
                        e->last_win_rect = cur;
                        /* Update border for non-current clips */
                        if (e->border_hwnd && IsWindow(e->border_hwnd)) {
                            RECT wr;
                            GetWindowRect(e->target_hwnd, &wr);
                            MoveWindow(e->border_hwnd,
                                wr.left + e->clip_rect_window.left - g_border_width,
                                wr.top + e->clip_rect_window.top - g_border_width,
                                e->clip_rect_window.right - e->clip_rect_window.left + g_border_width * 2,
                                e->clip_rect_window.bottom - e->clip_rect_window.top + g_border_width * 2,
                                TRUE);
                        }
                    }
                }
            }
            if (need_compact) CompactClips();
            if (!any_alive) {
                KillTimer(hwnd, TIMER_TRACK);
                CompactClips();
                LoadClip(-1);
                UpdateTrayTip();
            }
        }
        return 0;
    case WM_CLOSE:
        /* Restore all clipped windows — collect hwnds first to avoid
           CompactClips invalidating the loop (issue #1) */
    {
        HWND to_restore[MAX_CLIP_ENTRIES];
        int n = 0;
        for (int i = 0; i < g_clip_count; i++) {
            if (g_clips[i].target_hwnd && IsWindow(g_clips[i].target_hwnd))
                to_restore[n++] = g_clips[i].target_hwnd;
        }
        for (int i = 0; i < n; i++)
            RestoreWindow(to_restore[i]);
    }
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
        RedrawWindow(g_target_hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        UpdateWindow(g_target_hwnd);
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
        SaveCurClip();
        UpdateTrayTip();
    } else if (g_has_clip) {
        ApplyClipRegion();
        SaveCurClip();
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
        HFONT wfont = CreateFontA(20, 0, 0, 0, FW_NORMAL, 0, 0, 0,
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

        /* Hotkey lines — two-column layout */
        SelectObject(hdc, wfont);
        int y = 80;
        const wchar_t *keys[] = {
            L"Ctrl+Alt+F",
            L"Ctrl+Alt+B",
            L"Ctrl+Alt+T",
            L"Ctrl+Alt+Q",
        };
        const wchar_t *descs[] = {
            L"\x6846\x9009\x533A\x57DF",                          /* 框选区域 */
            L"\x663E\x793A/\x9690\x85CF\x8FB9\x6846",               /* 显示/隐藏边框 */
            L"\x7A97\x53E3\x7F6E\x9876",                          /* 窗口置顶 */
            L"\x8FD8\x539F\x7A97\x53E3",                          /* 还原窗口 */
        };
        const wchar_t *extras[] = {
            NULL,
            L"\x53EF\x62D6\x52A8\x7A97\x53E3",                   /* 可拖动窗口 */
            NULL,
            NULL,
        };
        for (int i = 0; i < 4; i++) {
            /* Key column — accent color */
            SetTextColor(hdc, CLR_ACCENT);
            TextOutW(hdc, 30, y, keys[i], (int)wcslen(keys[i]));
            /* Description column — text color */
            SetTextColor(hdc, CLR_TEXT);
            TextOutW(hdc, 170, y, descs[i], (int)wcslen(descs[i]));
            /* Extra note in parentheses */
            if (extras[i]) {
                SetTextColor(hdc, RGB(140, 140, 160));
                int desc_w = 0;
                SIZE sz;
                GetTextExtentPoint32W(hdc, descs[i], (int)wcslen(descs[i]), &sz);
                desc_w = sz.cx;
                wchar_t extra[64];
                swprintf(extra, 64, L"(%ls)", extras[i]);
                TextOutW(hdc, 170 + desc_w + 6, y, extra, (int)wcslen(extra));
            }
            y += 42;
        }

        /* Tips */
        SetTextColor(hdc, RGB(140, 140, 160));
        HFONT small_font = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei UI");
        SelectObject(hdc, small_font);
        TextOutW(hdc, 30, y + 16, L"\x53F3\x952E\x6258\x76D8\x56FE\x6807\x53EF\x5207\x6362\x7A97\x53E3", 9);
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
            BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg_clr;
            if (pressed) bg_clr = RGB(0, 90, 60);
            else bg_clr = hover ? CLR_BTN_OK_H : CLR_BTN_OK;
            /* Fill corners with parent background */
            HBRUSH parent_bg = CreateSolidBrush(CLR_BG);
            FillRect(dis->hDC, &dis->rcItem, parent_bg);
            DeleteObject(parent_bg);
            /* Draw rounded button */
            HRGN rgn = CreateRoundRectRgn(
                dis->rcItem.left, dis->rcItem.top,
                dis->rcItem.right, dis->rcItem.bottom,
                12, 12);  /* 6px radius */
            HBRUSH bg = CreateSolidBrush(bg_clr);
            FillRgn(dis->hDC, rgn, bg);
            DeleteObject(bg);
            DeleteObject(rgn);
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

    POINT center = CenterInVirtualDesktop(WELCOME_W, WELCOME_H);
    int sx = center.x;
    int sy = center.y;

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
    SwitchTargetWindow();

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
