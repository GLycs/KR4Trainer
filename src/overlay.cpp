// ============================================================================
// KR4Mod - Step 0 skeleton
//   x64 DLL: hook wglSwapBuffers / gdi32!SwapBuffers, draw an ImGui overlay.
//   Goal of this step: prove the injection + rendering chain works inside
//   Kingdom Rush Vengeance (Cocos2d-x, OpenGL) before touching any game logic.
//
// Hotkeys:
//   INSERT  : toggle overlay visibility
//   END     : unhook everything (leaves MinHook trampolines disabled)
//
// Log: %KR4MOD_LOG% env var, otherwise %TEMP%\kr4mod.log
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

#include <GL/gl.h>

// GL 3.2 constants are missing from MinGW's GL/gl.h (GL 1.1 only)
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK            0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT        0x00000001
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"
#include "MinHook.h"

// The backend intentionally keeps this declaration inside a '#if 0' block
// (see imgui_impl_win32.h); we must forward declare it ourselves.
// NOTE: IMGUI_IMPL_API / IMGUI_API expand to nothing by default, so the
// symbol uses C++ linkage -> no extern "C" here.
LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static char              g_logPath[MAX_PATH] = {0};
static CRITICAL_SECTION  g_logCs;
static bool              g_logReady = false;

static void LogInit()
{
    InitializeCriticalSection(&g_logCs);
    g_logReady = true;

    DWORD n = GetEnvironmentVariableA("KR4MOD_LOG", g_logPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        _snprintf(g_logPath, MAX_PATH, "%skr4mod.log", tmp);
        g_logPath[MAX_PATH - 1] = 0;
    }
}

static void Log(const char* fmt, ...)
{
    if (!g_logReady) return;
    EnterCriticalSection(&g_logCs);
    FILE* f = fopen(g_logPath, "a");
    if (f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI* PFN_wglSwapBuffers)(HDC);
typedef BOOL (WINAPI* PFN_SwapBuffers)(HDC);

static PFN_wglSwapBuffers g_origWglSwap = nullptr;
static PFN_SwapBuffers    g_origGdiSwap = nullptr;

static HWND     g_hwnd         = nullptr;
static WNDPROC  g_origWndProc  = nullptr;
static bool     g_imguiReady   = false;
static bool     g_initTried    = false;
static bool     g_overlayShown = true;
static bool     g_prevInsert   = false;
static bool     g_unhooked     = false;
static bool     g_pendingUnhook = false;
static ULONGLONG g_lastRender  = 0;
static volatile LONG g_inRender = 0;
static volatile LONG g_wglCalls  = 0;   // 诊断：wglSwapBuffers 命中次数
static volatile LONG g_gdiCalls  = 0;   // 诊断：gdi32!SwapBuffers 命中次数
static int           g_scanTick  = 0;   // 扫描切片降频计数
static int      g_frameCount   = 0;
static int      g_presentCount = 0;
static int      g_glErrorCount = 0;
static ULONGLONG g_lastStatsTick = 0;

// ---------------------------------------------------------------------------
// window procedure hook (input feed only, never swallows messages)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_imguiReady)
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// font
// ---------------------------------------------------------------------------
static void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    static const char* cands[] = {
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); ++i)
    {
        if (GetFileAttributesA(cands[i]) == INVALID_FILE_ATTRIBUTES)
            continue;

        ImFontConfig cfg;
        cfg.FontNo      = 0;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH  = false;

        // ImGui 内置的“常用简体中文”表只有 2500 字，面板里用到的
        // 屏/帧/桩/烁/频 等字不在表内，渲染出来就是“？”。这里在常用表的
        // 基础上把实际用到的字补进去，避免再出现方框或问号。
        static ImVector<ImWchar> glyphRanges;
        if (glyphRanges.empty())
        {
            ImFontGlyphRangesBuilder b;
            b.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            b.AddRanges(io.Fonts->GetGlyphRangesDefault());
            b.AddText("屏帧桩烁频挂钩秒毫扫描候选落进后备缓冲缺失快照");
            b.BuildRanges(&glyphRanges);
        }
        ImFont* f = io.Fonts->AddFontFromFileTTF(
            cands[i], 18.0f, &cfg, glyphRanges.Data);
        if (f)
        {
            Log("font loaded: %s", cands[i]);
            return;
        }
    }
    io.Fonts->AddFontDefault();
    Log("font: falling back to default (no CJK glyphs)");
}

// ---------------------------------------------------------------------------
// imgui bootstrap (must run with a current GL context)
// ---------------------------------------------------------------------------
static bool InitImGui(HDC dc)
{
    HWND hwnd = WindowFromDC(dc);
    if (!hwnd || !IsWindow(hwnd))
    {
        Log("InitImGui: no window for DC %p", dc);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 面板背景强制完全不透明：默认 WindowBg 的 alpha 是 0.94，游戏动画会从
    // 面板背后透出来，观感就是面板一直在明暗闪烁。这里把所有子窗口背景一起
    // 设为 alpha=1.0，彻底断掉"透出游戏画面"的通道。
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.Colors[ImGuiCol_WindowBg]         = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        st.Colors[ImGuiCol_ChildBg]          = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        st.Colors[ImGuiCol_PopupBg]          = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        st.Colors[ImGuiCol_MenuBarBg]        = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        st.WindowRounding    = 0.0f;   // 圆角会让边缘出现与游戏混合的半透明像素
        st.WindowBorderSize  = 1.0f;
        st.FrameRounding     = 2.0f;
        st.ChildBorderSize   = 1.0f;
    }

    LoadFonts();

    if (!ImGui_ImplWin32_Init((void*)hwnd))
    {
        Log("ImGui_ImplWin32_Init failed");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 150"))
    {
        Log("ImGui_ImplOpenGL3_Init failed");
        return false;
    }

    {
        GLint major = 0, minor = 0;
        glGetIntegerv(0x821B /*GL_MAJOR_VERSION*/, &major);
        glGetIntegerv(0x821C /*GL_MINOR_VERSION*/, &minor);
        Log("  GL numeric  = %d.%d", (int)major, (int)minor);
    }

    g_hwnd = hwnd;
    SetLastError(0);
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    if (!g_origWndProc)
        Log("SetWindowLongPtrW failed, err=%lu (input will not reach ImGui)", GetLastError());

    const char* glVersion = (const char*)glGetString(GL_VERSION);
    const char* glVendor  = (const char*)glGetString(GL_VENDOR);
    const char* glRender  = (const char*)glGetString(GL_RENDERER);
    Log("imgui ready. hwnd=%p", (void*)hwnd);
    Log("  GL_VERSION  = %s", glVersion ? glVersion : "(null)");
    Log("  GL_VENDOR   = %s", glVendor ? glVendor : "(null)");
    Log("  GL_RENDERER = %s", glRender ? glRender : "(null)");
    GLint profileMask = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
    Log("  GL_PROFILE  = 0x%x core=%d compat=%d", (unsigned)profileMask,
        (profileMask & GL_CONTEXT_CORE_PROFILE_BIT) ? 1 : 0,
        (profileMask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) ? 1 : 0);
    RECT crc = {0, 0, 0, 0};
    GetClientRect(hwnd, &crc);
    Log("  client area = %ldx%ld", (long)(crc.right - crc.left), (long)(crc.bottom - crc.top));

    g_imguiReady = true;
    return true;
}

// ---------------------------------------------------------------------------
// in-process framebuffer capture (DWM/GDI screen grabs cannot see the GL swap
// chain of a hardware-overlay window, so we read the back buffer ourselves)
// ---------------------------------------------------------------------------
static bool SaveBmpFromGL(int w, int h, const char* path)
{
    if (w <= 0 || h <= 0) return false;
    std::vector<unsigned char> pix((size_t)w * (size_t)h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pix.data());

    int rowBytes = w * 3;
    int pad = (4 - (rowBytes % 4)) % 4;
    int imgSize = (rowBytes + pad) * h;
    int fileSize = 54 + imgSize;

    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(&hdr[2], &fileSize, 4);
    int off = 54;  memcpy(&hdr[10], &off, 4);
    int biSize = 40; memcpy(&hdr[14], &biSize, 4);
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    short planes = 1; memcpy(&hdr[26], &planes, 2);
    short bpp = 24;   memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &imgSize, 4);

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row((size_t)rowBytes + pad, 0);
    for (int y = 0; y < h; ++y)
    {
        const unsigned char* src = &pix[(size_t)y * (size_t)w * 3];
        for (int x = 0; x < w; ++x)
        {
            row[(size_t)x * 3 + 0] = src[(size_t)x * 3 + 2];
            row[(size_t)x * 3 + 1] = src[(size_t)x * 3 + 1];
            row[(size_t)x * 3 + 2] = src[(size_t)x * 3 + 0];
        }
        fwrite(row.data(), 1, (size_t)rowBytes + pad, f);
    }
    fclose(f);
    return true;
}

static void MaybeSelfShot()
{
    char tmp[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, tmp)) return;

    char req[MAX_PATH] = {0};
    snprintf(req, sizeof(req), "%skr4mod_shot.req", tmp);
    if (GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES) return;

    RECT rc = {0, 0, 0, 0};
    if (g_hwnd) GetClientRect(g_hwnd, &rc);
    int w = (int)(rc.right - rc.left), h = (int)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) { Log("selfshot: bad size %dx%d", w, h); return; }

    char out[MAX_PATH] = {0};
    snprintf(out, sizeof(out), "%skr4mod_selfshot.bmp", tmp);
    if (SaveBmpFromGL(w, h, out))
    {
        Log("selfshot saved %dx%d -> %s", w, h, out);
        DeleteFileA(req);
    }
    else
    {
        Log("selfshot failed writing %s", out);
    }
}

// presence of %TEMP%\<name> turns a runtime switch on; lets us toggle the
// overlay without reinstalling anything
static bool FlagFileOn(const char* name)
{
    char tmp[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, tmp)) return false;
    char path[MAX_PATH] = {0};
    snprintf(path, sizeof(path), "%s%s", tmp, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_FRAMEBUFFER       // might already exist via glext-ish headers
#define GL_FRAMEBUFFER 0x8D40
#endif

// Draws one solid red 120x120 square with the plain fixed-function pipeline,
// completely bypassing ImGui. If this does not show up on screen then the
// problem is our present hook / render target, not the ImGui vertex path.
static void __attribute__((unused)) DrawProbe()
{
    static int probeCalls = 0;
    if (probeCalls++ >= 3) return;   // diagnostic only, first 3 presents

    GLint fbo = -1, rfbo = -1;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    GLboolean dbl = glIsEnabled(GL_DOUBLEBUFFER);

    static int probeLogged = 0;
    if (probeLogged < 3)
    {
        ++probeLogged;
        Log("probe: drawFbo=%d readFbo=%d viewport=%d,%d %dx%d dbl=%d err=0x%x",
            fbo, rfbo, vp[0], vp[1], vp[2], vp[3], (int)dbl, glGetError());
    }

    glPushAttrib(0xFFFFFFFF);
    glPushClientAttrib(0xFFFFFFFF);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0.0, 120.0, 120.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0, 0); glVertex2f(120, 0); glVertex2f(120, 120); glVertex2f(0, 120);
    glEnd();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();

    // read the probe pixel straight back out of the back buffer: this tells us
    // whether the geometry landed in the buffer that gets presented at all
    static int readLogged = 0;
    if (readLogged < 3)
    {
        ++readLogged;
        glFinish();
        GLint dbli = -1;
        glGetIntegerv(GL_DOUBLEBUFFER, &dbli);
        GLubyte px[4] = {0, 0, 0, 0};
        glReadBuffer(GL_BACK);
        glReadPixels(60, (vp[3] > 120 ? vp[3] - 60 : 60), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        Log("probe readback: RGBA=%d,%d,%d,%d doubleBuffer=%d vpH=%d err=0x%x",
            px[0], px[1], px[2], px[3], dbli, vp[3], glGetError());

        // second probe: scissored solid clear. Distinguishes "no drawing at all"
        // from "immediate mode (glBegin) is ignored in this context".
        glPushAttrib(0xFFFFFFFF);
        glEnable(GL_SCISSOR_TEST);
        glScissor(200, 200, 200, 200);
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glPopAttrib();
        glFinish();
        GLubyte px2[4] = {0, 0, 0, 0};
        glReadBuffer(GL_BACK);
        glReadPixels(300, 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px2);
        Log("probe clear readback: RGBA=%d,%d,%d,%d err=0x%x",
            px2[0], px2[1], px2[2], px2[3], glGetError());
    }
}

// ---------------------------------------------------------------------------
// in-process memory scanner (step 1) - locates a 4-byte int in our own address
// space, then narrows it down by re-filtering after the value changes.
// Driven from %TEMP%\kr4mod_cmd.txt so it can be scripted without typing:
//   scan <int> | filter <int> | write <int> | clear
// ---------------------------------------------------------------------------
#include <vector>
#include <stdint.h>
#include <string.h>

struct MemRange { uintptr_t base; size_t size; };

static std::vector<MemRange>  g_ranges;
static std::vector<uintptr_t> g_hits;
static size_t     g_rangeIdx = 0;
static size_t     g_rangeOff = 0;
static unsigned char g_target[4] = {0, 0, 0, 0};
static int        g_anchorOff = 0;
static bool       g_scanIsFloat = false;
static bool       g_scanning = false;
static ULONGLONG  g_scanStart = 0;
static int        g_scanMs = 0;
static int        g_writeCount = 0;
static char       g_lastCmd[256] = {0};
static char       g_status[256] = "空闲";

// guard against faulting on a freed region
static bool PageReadable(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// encode the value we are looking for into 4 raw bytes; the anchor byte is
// the least common of the four so memchr skips most of the address space
static void SetTarget(double value, bool isFloat)
{
    g_scanIsFloat = isFloat;
    if (isFloat)
    {
        float f = (float)value;
        memcpy(g_target, &f, 4);
    }
    else
    {
        int32_t i = (int32_t)value;
        memcpy(g_target, &i, 4);
    }
    g_anchorOff = 0;
    for (int k = 0; k < 4; ++k)
    {
        if (g_target[k] != 0 && g_target[k] != 0xFF)
        {
            g_anchorOff = k;
            break;
        }
    }
}

static void ScanCollectRanges()
{
    g_ranges.clear();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t maxAddr = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < maxAddr)
    {
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            DWORD p = mbi.Protect & 0xFF;
            bool writable = (p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                             p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
            if (writable && mbi.RegionSize >= 4)
            {
                MemRange r;
                r.base = base;
                r.size = mbi.RegionSize;
                g_ranges.push_back(r);
            }
        }
        uintptr_t next = base + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
}

static void ScanStart(double value, bool isFloat)
{
    SetTarget(value, isFloat);
    ScanCollectRanges();
    g_hits.clear();
    g_hits.reserve(4096);
    g_rangeIdx = 0;
    g_rangeOff = 0;
    g_scanning = true;
    g_scanStart = GetTickCount64();
    snprintf(g_status, sizeof(g_status), "正在扫描 %.6g（%s），共 %zu 个内存区",
             value, isFloat ? "单精度浮点" : "32 位整数", g_ranges.size());
    Log("scan: start value=%.6g type=%s regions=%zu", value, isFloat ? "单精度浮点" : "32 位整数", g_ranges.size());
}

static void ScanFilter(double value, bool isFloat)
{
    SetTarget(value, isFloat);
    size_t kept = 0;
    for (size_t i = 0; i < g_hits.size(); ++i)
    {
        if (!PageReadable(g_hits[i])) continue;
        unsigned char cur[4];
        memcpy(cur, (const void*)g_hits[i], 4);
        if (memcmp(cur, g_target, 4) == 0) g_hits[kept++] = g_hits[i];
    }
    g_hits.resize(kept);
    snprintf(g_status, sizeof(g_status), "已过滤 %.6g（%s），剩余 %d 个候选",
             value, isFloat ? "单精度浮点" : "32 位整数", (int)kept);
    Log("scan: filter value=%.6g type=%s -> %zu candidates", value, isFloat ? "float" : "int32", kept);
}

static void ScanStep(size_t budget)
{
    if (!g_scanning) return;
    size_t done = 0;
    while (done < budget)
    {
        if (g_rangeIdx >= g_ranges.size())
        {
            g_scanning = false;
            g_scanMs = (int)(GetTickCount64() - g_scanStart);
            snprintf(g_status, sizeof(g_status), "扫描完成: %d 个命中，耗时 %d 毫秒", (int)g_hits.size(), g_scanMs);
            Log("scan: done hits=%zu regions=%zu ms=%d", g_hits.size(), g_ranges.size(), g_scanMs);
            return;
        }
        MemRange& r = g_ranges[g_rangeIdx];
        size_t remain = r.size - g_rangeOff;
        size_t chunk = (remain < (budget - done)) ? remain : (budget - done);
        if (chunk > 4)
        {
            const unsigned char* p = (const unsigned char*)(r.base + g_rangeOff);
            const unsigned char* endAnchor = p + chunk - (4 - g_anchorOff);
            const unsigned char* cur = p + g_anchorOff;
            while (cur < endAnchor)
            {
                const unsigned char* hit = (const unsigned char*)memchr(cur, g_target[g_anchorOff],
                                                                       (size_t)(endAnchor - cur));
                if (!hit) break;
                const unsigned char* rec = hit - g_anchorOff;
                if (g_hits.size() < 4000000 && memcmp(rec, g_target, 4) == 0)
                    g_hits.push_back((uintptr_t)rec);
                cur = hit + 1;
            }
        }
        g_rangeOff += chunk;
        done += chunk;
        if (g_rangeOff >= r.size)
        {
            ++g_rangeIdx;
            g_rangeOff = 0;
        }
    }
}

static void ScanWrite(double value, bool isFloat)
{
    SetTarget(value, isFloat);
    int n = 0;
    for (size_t i = 0; i < g_hits.size(); ++i)
    {
        if (!PageReadable(g_hits[i])) continue;
        memcpy((void*)g_hits[i], g_target, 4);
        ++n;
    }
    g_writeCount = n;
    snprintf(g_status, sizeof(g_status), "已将 %.6g 写入 %d/%d 个地址", value, n, (int)g_hits.size());
    Log("write: value=%.6g type=%s applied=%d total=%zu", value, isFloat ? "float" : "int32", n, g_hits.size());
}

// ---------------------------------------------------------------------------
// money hook (step 3)
// Coins are an int32 pushed through a virtual setter; the HUD only refreshes
// inside that call, which is why writing a scanned address never showed up.
// We inline-hook the prologue right before the vcall and rewrite edx.
//   pattern: 8D 14 07 48 8B CE FF ?? F8 07 00 00 48   (lea edx,[rdi+rax])
// ---------------------------------------------------------------------------
static bool     g_moneyInstalled = false;
static bool     g_moneyOn = false;
static void*    g_moneyStub = nullptr;
static void*    g_moneyTarget = nullptr;
static volatile int32_t* g_moneyFlag = nullptr;   // lives inside the stub page
static volatile int32_t* g_moneyAmt = nullptr;    // lives inside the stub page
static volatile int64_t* g_moneyCount = nullptr;   // lives inside the stub page
static volatile int32_t* g_moneyLastEdx = nullptr; // value the game would have written
static volatile int32_t* g_moneyLastEax = nullptr; // getter() result
static volatile int32_t* g_moneyLastEdi = nullptr; // delta argument
static volatile uint64_t* g_moneyLastRbx = nullptr;// vtable of the coin object
static volatile int32_t* g_moneyMul = nullptr;     // money multiplier slot (stub+0xE8)
static int64_t  g_moneyCountLogged = -1;
static unsigned char g_moneyOrg[6] = {0};
static char     g_moneyStatus[192] = "未安装";

static unsigned char* FindPatternFwd(unsigned char* p, size_t size,
                                     const unsigned char* pat, const char* mask, size_t len)
{
    if (size < len || len == 0) return nullptr;
    size_t end = size - len;
    for (size_t i = 0; i <= end; ++i)
    {
        size_t k = 0;
        for (; k < len; ++k)
        {
            if (mask[k] == 'x' && p[i + k] != pat[k]) break;
        }
        if (k == len) return p + i;
    }
    return nullptr;
}

static unsigned char* ScanGameExec(const unsigned char* pat, const char* mask, size_t len)
{
    HMODULE mod = GetModuleHandleW(L"Kingdom Rush Vengeance.exe");
    if (!mod) return nullptr;
    unsigned char* base = (unsigned char*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (!(sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        unsigned char* start = base + sh[i].VirtualAddress;
        size_t size = sh[i].Misc.VirtualSize;
        if (size == 0) size = sh[i].SizeOfRawData;
        unsigned char* hit = FindPatternFwd(start, size, pat, mask, len);
        if (hit)
        {
            Log("money: pattern hit in section %d at 0x%p", i, (void*)hit);
            return hit;
        }
    }
    return nullptr;
}

// the rel32 jmp needs the stub within +/-2GB of the game's .text
static void* AllocNear(void* target)
{
    uintptr_t t = (uintptr_t)target;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t maxDelta = 0x70000000ULL;
    for (uintptr_t delta = gran; delta < maxDelta; delta += gran)
    {
        uintptr_t cand[2] = { t - delta, t + delta };
        for (int k = 0; k < 2; ++k)
        {
            void* p = VirtualAlloc((LPVOID)(cand[k] & ~(gran - 1)), 0x1000,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

static bool InstallMoneyHook()
{
    if (g_moneyInstalled) return true;

    static const unsigned char p1[] = {0x8D,0x14,0x07,0x48,0x8B,0xCE,0xFF,0x00,0xF8,0x07,0x00,0x00,0x48};
    static const unsigned char p2[] = {0x8D,0x14,0x07,0x48,0x8B,0xCE,0xFF,0x00,0xF0,0x07,0x00,0x00,0x48};
    static const char pm[] = "xxxxxxx?xxxxx";

    unsigned char* hit = ScanGameExec(p1, pm, sizeof(p1));
    if (!hit) hit = ScanGameExec(p2, pm, sizeof(p2));
    if (!hit)
    {
        snprintf(g_moneyStatus, sizeof(g_moneyStatus), "在游戏模块中未找到特征码");
        Log("money: install failed, pattern not found");
        return false;
    }
    if (hit[0] != 0x8D || hit[3] != 0x48 || hit[4] != 0x8B || hit[5] != 0xCE)
    {
        snprintf(g_moneyStatus, sizeof(g_moneyStatus), "目标处原始指令不符合预期: 0x%p", (void*)hit);
        Log("money: prologue mismatch at 0x%p", (void*)hit);
        return false;
    }

    unsigned char* stub = (unsigned char*)AllocNear(hit);
    if (!stub)
    {
        snprintf(g_moneyStatus, sizeof(g_moneyStatus), "目标附近没有可执行内存页");
        Log("money: AllocNear failed");
        return false;
    }

    g_moneyFlag    = (volatile int32_t*)(stub + 0xC0);
    g_moneyAmt     = (volatile int32_t*)(stub + 0xC4);
    g_moneyCount   = (volatile int64_t*)(stub + 0xC8);
    g_moneyLastEdx = (volatile int32_t*)(stub + 0xD0);
    g_moneyLastEax = (volatile int32_t*)(stub + 0xD4);
    g_moneyLastEdi = (volatile int32_t*)(stub + 0xD8);
    g_moneyLastRbx = (volatile uint64_t*)(stub + 0xE0);
    g_moneyMul     = (volatile int32_t*)(stub + 0xE8);
    *g_moneyFlag  = 0;
    *g_moneyAmt   = 999999;
    *g_moneyCount = 0;
    *g_moneyMul   = 1;

    // Layout (every rip-relative disp below is computed from the address of the
    // NEXT instruction, which is where the CPU actually starts adding):
    //   0x00  lea edx,[rdi+rax]     prologue we displaced
    //   0x03  jmp 0x40              rel32 from 0x08
    //   0x40  inc qword [count]
    //   0x47  mov  [lastEdx], edx   value the game intended to write
    //   0x4D  mov  [lastEax], eax   getter() result
    //   0x53  mov  [lastEdi], edi   delta
    //   0x59  mov  [lastRbx], rbx   vtable
    //   0x60  cmp  dword [flag], 1  (7 bytes -> next ip = +7, i.e. o+5 here)
    //   0x67  jne  0x73
    //   0x6D  mov  edx, [amt]       the actual override
    //   0x73  mov  rcx, rsi
    //   0x76  jmp  hit+6
    // BUGFIX: the previous build used (o + 4) as the cmp displacement base,
    // which made it read one byte past g_moneyFlag (0xC1). The compare never
    // matched, jne always taken, so the override never ran and the game wrote
    // its own value while the log still claimed lock=1.
    stub[0] = 0x8D; stub[1] = 0x14; stub[2] = 0x07;
    stub[3] = 0xE9;
    int32_t jt = (int32_t)((intptr_t)(stub + 0x40) - (intptr_t)(stub + 8));
    memcpy(stub + 4, &jt, 4);
    for (int i = 8; i < 0x40; ++i) stub[i] = 0xCC;

    unsigned char* t = stub + 0x40;
    size_t o = 0;
    t[o++] = 0x48; t[o++] = 0xFF; t[o++] = 0x05;                   // inc qword [count]
    { int32_t d = (int32_t)((intptr_t)g_moneyCount - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    // multiplier (integer form of the trainer's money_mul_f): applies to the
    // incoming delta only, and only when the delta is positive
    t[o++] = 0x85; t[o++] = 0xFF;                                  // test edi,edi
    t[o++] = 0x0F; t[o++] = 0x8E;                                  // jle .rec
    size_t jMulA = o; o += 4;
    t[o++] = 0x8B; t[o++] = 0x0D;                                  // mov ecx,[mul]
    { int32_t d = (int32_t)((intptr_t)g_moneyMul - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x83; t[o++] = 0xF9; t[o++] = 0x01;                   // cmp ecx,1
    t[o++] = 0x0F; t[o++] = 0x8E;                                  // jle .rec
    size_t jMulB = o; o += 4;
    t[o++] = 0x0F; t[o++] = 0xAF; t[o++] = 0xF9;                   // imul edi,ecx
    size_t lblRec = o;
    t[o++] = 0x89; t[o++] = 0x15;                                  // mov [lastEdx],edx
    { int32_t d = (int32_t)((intptr_t)g_moneyLastEdx - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x89; t[o++] = 0x05;                                  // mov [lastEax],eax
    { int32_t d = (int32_t)((intptr_t)g_moneyLastEax - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x89; t[o++] = 0x3D;                                  // mov [lastEdi],edi
    { int32_t d = (int32_t)((intptr_t)g_moneyLastEdi - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x48; t[o++] = 0x89; t[o++] = 0x1D;                   // mov [lastRbx],rbx
    { int32_t d = (int32_t)((intptr_t)g_moneyLastRbx - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x83; t[o++] = 0x3D;                                  // cmp dword [flag],1
    { int32_t d = (int32_t)((intptr_t)g_moneyFlag - (intptr_t)(t + o + 5)); memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x01;
    t[o++] = 0x0F; t[o++] = 0x85;                                  // jne skip
    size_t jnePos = o; o += 4;
    t[o++] = 0x8B; t[o++] = 0x15;                                  // mov edx,[amt]
    { int32_t d = (int32_t)((intptr_t)g_moneyAmt - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    { int32_t d = (int32_t)((intptr_t)(t + o) - (intptr_t)(t + jnePos + 4)); memcpy(t + jnePos, &d, 4); }
    t[o++] = 0x48; t[o++] = 0x8B; t[o++] = 0xCE;                   // mov rcx,rsi
    t[o++] = 0xE9;                                                 // jmp hit+6
    { int32_t d = (int32_t)((intptr_t)(hit + 6) - (intptr_t)(t + o + 4)); memcpy(t + o, &d, 4); o += 4; }
    { int32_t d = (int32_t)((intptr_t)(t + lblRec) - (intptr_t)(t + jMulA + 4)); memcpy(t + jMulA, &d, 4); }
    { int32_t d = (int32_t)((intptr_t)(t + lblRec) - (intptr_t)(t + jMulB + 4)); memcpy(t + jMulB, &d, 4); }
    while (o < 0x40) t[o++] = 0xCC;                                // pad: never fall through
    FlushInstructionCache(GetCurrentProcess(), stub, 0x40 + o);
    {
        char hex[3 * 0x80 + 1]; int hp = 0;
        for (int i = 0; i < 0x80; ++i)
        {
            hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", stub[i]);
        }
        Log("money: stub bytes[0..0x7F] = %s", hex);
    }

    memcpy(g_moneyOrg, hit, 6);
    DWORD old = 0;
    if (!VirtualProtect(hit, 6, PAGE_EXECUTE_READWRITE, &old))
    {
        snprintf(g_moneyStatus, sizeof(g_moneyStatus), "VirtualProtect 调用失败 (%lu)", GetLastError());
        Log("money: VirtualProtect failed %lu", GetLastError());
        VirtualFree(stub, 0, MEM_RELEASE);
        g_moneyFlag = nullptr; g_moneyAmt = nullptr; g_moneyStub = nullptr;
        return false;
    }
    unsigned char patch[6];
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((intptr_t)stub - (intptr_t)(hit + 5));
    memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90;
    // Write the tail first and the 0xE9 opcode last: a thread that is about to
    // execute `hit` sees either the untouched prologue or a complete jmp.
    memcpy(hit + 1, patch + 1, 5);
    MemoryBarrier();
    hit[0] = patch[0];
    VirtualProtect(hit, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 6);

    g_moneyStub = stub;
    g_moneyTarget = hit;
    g_moneyInstalled = true;
    snprintf(g_moneyStatus, sizeof(g_moneyStatus), "已挂钩 0x%p（桩代码 0x%p）", (void*)hit, (void*)stub);
    Log("money: hook installed target=0x%p stub=0x%p", (void*)hit, (void*)stub);
    return true;
}

// forward declaration: the feature block below calls it, definition follows
static void SetMoneyLock(bool on);
static void UnhookAll();

// ===========================================================================
// generic inline-hook plumbing (hand-written trampolines, same discipline as
// the money hook: stub page within +/-2GB, rip-relative slots inside the page,
// tail-first patch write, prologue restored on unhook)
// ===========================================================================
struct InlineHk
{
    const char*    name        = "";
    unsigned char* target      = nullptr;
    unsigned char* stub        = nullptr;
    int            cover       = 0;
    unsigned char  org[16]     = {0};
    bool           installed   = false;
    char           status[160] = "未安装";
};

// logic always starts at stub+0x40, data slots live at stub+0x180
#define HK_LOGIC 0x40
#define HK_DATA  0x180

static inline void EmitRip(unsigned char* t, size_t& o, const volatile void* dst)
{
    int32_t d = (int32_t)((intptr_t)(const void*)dst - (intptr_t)(t + o + 4));
    memcpy(t + o, &d, 4);
    o += 4;
}

static inline void EmitJmp(unsigned char* t, size_t& o, const void* dst)
{
    t[o++] = 0xE9;
    int32_t d = (int32_t)((intptr_t)dst - (intptr_t)(t + o + 4));
    memcpy(t + o, &d, 4);
    o += 4;
}

// jcc rel32 was emitted as opcode + 4 zero bytes at dispPos; patch it now
static inline void PatchJcc(unsigned char* t, size_t dispPos, size_t targetOff)
{
    int32_t d = (int32_t)((intptr_t)(t + targetOff) - (intptr_t)(t + dispPos + 4));
    memcpy(t + dispPos, &d, 4);
}

static bool HkBegin(InlineHk& hk, unsigned char* hit, int cover)
{
    if (hk.installed) return true;
    if (!hit)
    {
        snprintf(hk.status, sizeof(hk.status), "%s: 未找到特征码", hk.name);
        Log("hk %s: pattern not found", hk.name);
        return false;
    }
    if (hit[0] == 0xE9)
    {
        snprintf(hk.status, sizeof(hk.status), "%s: 该处已被占用 0x%p", hk.name, (void*)hit);
        Log("hk %s: target already patched at 0x%p", hk.name, (void*)hit);
        return false;
    }
    unsigned char* stub = (unsigned char*)AllocNear(hit);
    if (!stub)
    {
        snprintf(hk.status, sizeof(hk.status), "%s: 近地址分配失败", hk.name);
        Log("hk %s: AllocNear failed", hk.name);
        return false;
    }
    for (int i = 0; i < 0x400; ++i) stub[i] = 0x90;
    stub[0] = 0xE9;
    {
        int32_t d = (int32_t)((intptr_t)(stub + HK_LOGIC) - (intptr_t)(stub + 5));
        memcpy(stub + 1, &d, 4);
    }
    hk.stub    = stub;
    hk.target  = hit;
    hk.cover   = cover;
    memcpy(hk.org, hit, cover);
    return true;
}

static bool HkFinish(InlineHk& hk)
{
    FlushInstructionCache(GetCurrentProcess(), hk.stub, 0x400);
    DWORD old = 0;
    if (!VirtualProtect(hk.target, hk.cover, PAGE_EXECUTE_READWRITE, &old))
    {
        snprintf(hk.status, sizeof(hk.status), "%s: VirtualProtect 失败 %lu", hk.name, GetLastError());
        Log("hk %s: VirtualProtect failed %lu", hk.name, GetLastError());
        VirtualFree(hk.stub, 0, MEM_RELEASE);
        hk.stub = nullptr;
        return false;
    }
    unsigned char patch[16];
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((intptr_t)hk.stub - (intptr_t)(hk.target + 5));
    memcpy(patch + 1, &rel, 4);
    for (int i = 5; i < hk.cover; ++i) patch[i] = 0x90;
    memcpy(hk.target + 1, patch + 1, hk.cover - 1);
    MemoryBarrier();
    hk.target[0] = patch[0];
    VirtualProtect(hk.target, hk.cover, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hk.target, hk.cover);
    hk.installed = true;
    snprintf(hk.status, sizeof(hk.status), "%s: 已开启 0x%p", hk.name, (void*)hk.target);
    Log("hk %s: installed target=0x%p stub=0x%p cover=%d", hk.name, (void*)hk.target, (void*)hk.stub, hk.cover);
    return true;
}

static void HkRestore(InlineHk& hk)
{
    if (!hk.installed || !hk.target) return;
    DWORD old = 0;
    if (VirtualProtect(hk.target, hk.cover, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(hk.target, hk.org, hk.cover);
        VirtualProtect(hk.target, hk.cover, old, &old);
        FlushInstructionCache(GetCurrentProcess(), hk.target, hk.cover);
        Log("hk %s: prologue restored", hk.name);
    }
    if (hk.stub) { VirtualFree(hk.stub, 0, MEM_RELEASE); hk.stub = nullptr; }
    hk.installed = false;
    snprintf(hk.status, sizeof(hk.status), "%s: 已卸载", hk.name);
}

// ---------------------------------------------------------------------------
// HP 9999 (health setter: lea edx,[rdi+rax]; mov rcx,rsi; call [rbx+0x808])
// ---------------------------------------------------------------------------
static InlineHk          g_hkHp;
static volatile int32_t* g_hpFlag  = nullptr;
static volatile int32_t* g_hpAmt   = nullptr;
static volatile int64_t* g_hpCalls = nullptr;
static int64_t           g_hpCallsLogged = -1;
static bool              g_hpOn    = false;

static bool InstallHpHook()
{
    g_hkHp.name = "生命锁定";
    if (g_hkHp.installed) return true;

    static const unsigned char pat[] = {0x8D,0x14,0x07,0x48,0x8B,0xCE,0xFF,0x00,0x08,0x08,0x00,0x00,0x48};
    static const char msk[] = "xxxxxxx?xxxxx";
    unsigned char* hit = ScanGameExec(pat, msk, sizeof(pat));
    if (!HkBegin(g_hkHp, hit, 6)) return false;

    unsigned char* stub = g_hkHp.stub;
    g_hpFlag  = (volatile int32_t*)(stub + HK_DATA + 0x00);
    g_hpAmt   = (volatile int32_t*)(stub + HK_DATA + 0x04);
    g_hpCalls = (volatile int64_t*)(stub + HK_DATA + 0x08);
    *g_hpFlag = 0;
    *g_hpAmt  = 9999;
    *g_hpCalls = 0;

    unsigned char* t = stub + HK_LOGIC;
    size_t o = 0;
    t[o++] = 0x48; t[o++] = 0xFF; t[o++] = 0x05; EmitRip(t, o, g_hpCalls); // inc qword [calls]
    t[o++] = 0x83; t[o++] = 0x3D; EmitRip(t, o, g_hpFlag); t[o++] = 0x01;  // cmp dword [flag],1
    t[o++] = 0x0F; t[o++] = 0x85; size_t jNo = o; o += 4;                  // jne .orig
    t[o++] = 0x8B; t[o++] = 0x15; EmitRip(t, o, g_hpAmt);                  // mov edx,[amt]
    t[o++] = 0xE9; size_t jDone = o; o += 4;                               // jmp .code
    size_t lblOrig = o;
    t[o++] = 0x8D; t[o++] = 0x14; t[o++] = 0x07;                           // lea edx,[rdi+rax]
    size_t lblCode = o;
    t[o++] = 0x48; t[o++] = 0x8B; t[o++] = 0xCE;                           // mov rcx,rsi
    EmitJmp(t, o, g_hkHp.target + 6);
    PatchJcc(t, jNo, lblOrig);
    { int32_t d = (int32_t)((intptr_t)(t + lblCode) - (intptr_t)(t + jDone + 4)); memcpy(t + jDone, &d, 4); }
    return HkFinish(g_hkHp);
}

static void SetHpLock(bool on)
{
    g_hpOn = on;
    if (g_hpFlag) *g_hpFlag = on ? 1 : 0;
    Log("hp: lock=%d amount=%d", on ? 1 : 0, g_hpAmt ? (int)*g_hpAmt : -1);
}

// ---------------------------------------------------------------------------
// unit damage hook:
//   mov r14d,eax ; sub dword [rbx+hpOff],eax ; cmp byte [rbx+typeOff],0 ; je ..
// type byte 0 = player-side unit -> add the damage straight back (invulnerable)
// type 1..2 = hostile            -> integer damage multiplier / one-hit kill
// ---------------------------------------------------------------------------
static InlineHk          g_hkUnit;
static volatile int32_t* g_allyFlag      = nullptr;
static volatile int32_t* g_dmgMul        = nullptr;
static volatile int32_t* g_killFlag      = nullptr;
static volatile int64_t* g_unitCalls     = nullptr;
static volatile int32_t* g_unitLastDmg   = nullptr;
static volatile int32_t* g_unitLastType  = nullptr;
static volatile int32_t* g_unitLastTypeD = nullptr;
static volatile int32_t* g_unitLastHp    = nullptr;
static int64_t           g_unitCallsLogged = -1;
static volatile int32_t  g_unitHpOff     = 0;
static volatile int32_t  g_unitTypeOff   = 0;
static bool              g_allyOn        = false;

static bool InstallUnitHook()
{
    g_hkUnit.name = "友军伤害";
    if (g_hkUnit.installed) return true;

    // 44 8B F0 (mov r14d,eax) ; 29 83 <hpOff> ; 80 BB <typeOff> 00 74
    static const unsigned char pat[] = {0x44,0x8B,0xF0,0x29,0x83,0,0,0,0,0x80,0xBB,0,0,0,0,0x00,0x74};
    static const char msk[] = "xxxxx????xx?????x";
    unsigned char* hit = ScanGameExec(pat, msk, sizeof(pat));
    if (!hit)
    {
        snprintf(g_hkUnit.status, sizeof(g_hkUnit.status), "友军伤害: 未找到特征码");
        Log("unit: pattern not found");
        return false;
    }

    const int32_t hpOff   = *(const int32_t*)(hit + 5);
    const int32_t typeOff = *(const int32_t*)(hit + 11);
    g_unitHpOff = hpOff;
    g_unitTypeOff = typeOff;
    Log("unit: hit 0x%p hpOff=0x%X typeOff=0x%X", (void*)hit, (unsigned)hpOff, (unsigned)typeOff);

    // cover exactly the two original instructions (3 + 6 = 9 bytes); the
    // following cmp/je pair must stay untouched or we would cut it in half
    if (!HkBegin(g_hkUnit, hit, 9)) return false;

    unsigned char* stub = g_hkUnit.stub;
    g_allyFlag      = (volatile int32_t*)(stub + HK_DATA + 0x00);
    g_dmgMul        = (volatile int32_t*)(stub + HK_DATA + 0x04);
    g_killFlag      = (volatile int32_t*)(stub + HK_DATA + 0x08);
    g_unitCalls     = (volatile int64_t*)(stub + HK_DATA + 0x10);
    g_unitLastDmg   = (volatile int32_t*)(stub + HK_DATA + 0x18);
    g_unitLastType  = (volatile int32_t*)(stub + HK_DATA + 0x1C);
    g_unitLastTypeD = (volatile int32_t*)(stub + HK_DATA + 0x20);
    g_unitLastHp    = (volatile int32_t*)(stub + HK_DATA + 0x24);
    *g_allyFlag = 0; *g_dmgMul = 1; *g_killFlag = 0; *g_unitCalls = 0;
    *g_unitLastDmg = 0; *g_unitLastType = 0; *g_unitLastTypeD = 0; *g_unitLastHp = 0;

    unsigned char* t = stub + HK_LOGIC;
    size_t o = 0;
    t[o++] = 0x52;                                                           // push rdx
    t[o++] = 0x51;                                                           // push rcx
    t[o++] = 0x48; t[o++] = 0xFF; t[o++] = 0x05; EmitRip(t, o, g_unitCalls);  // inc qword [calls]
    t[o++] = 0x48; t[o++] = 0x8D; t[o++] = 0x93;                             // lea rdx,[rbx+hpOff]
    { int32_t d = hpOff; memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x8B; t[o++] = 0x8B;                                            // mov ecx,[rbx+typeOff]
    { int32_t d = typeOff; memcpy(t + o, &d, 4); o += 4; }
    t[o++] = 0x89; t[o++] = 0x0D; EmitRip(t, o, g_unitLastTypeD);            // mov [lastTypeD],ecx
    t[o++] = 0x0F; t[o++] = 0xB6; t[o++] = 0xC9;                             // movzx ecx,cl
    t[o++] = 0x89; t[o++] = 0x0D; EmitRip(t, o, g_unitLastType);             // mov [lastType],ecx
    t[o++] = 0x89; t[o++] = 0x05; EmitRip(t, o, g_unitLastDmg);              // mov [lastDmg],eax
    t[o++] = 0x85; t[o++] = 0xC9;                                            // test ecx,ecx
    t[o++] = 0x0F; t[o++] = 0x85; size_t jEnemy = o; o += 4;                 // jne .enemy
    // ---- player side: hp += damage (net zero) ----
    t[o++] = 0x83; t[o++] = 0x3D; EmitRip(t, o, g_allyFlag); t[o++] = 0x01;  // cmp [ally],1
    t[o++] = 0x0F; t[o++] = 0x85; size_t jAllyOut = o; o += 4;               // jne .code
    t[o++] = 0x8B; t[o++] = 0x0A;                                            // mov ecx,[rdx] (hp after the sub)
    t[o++] = 0x03; t[o++] = 0xC8;                                            // add ecx,eax
    t[o++] = 0x89; t[o++] = 0x0A;                                            // mov [rdx],ecx
    t[o++] = 0xE9; size_t jAllyDone = o; o += 4;                             // jmp .code
    // ---- hostile side ----
    size_t lblEnemy = o;
    t[o++] = 0x83; t[o++] = 0xF9; t[o++] = 0x02;                             // cmp ecx,2
    t[o++] = 0x0F; t[o++] = 0x8F; size_t jBigType = o; o += 4;               // jg .code
    t[o++] = 0x85; t[o++] = 0xC0;                                            // test eax,eax
    t[o++] = 0x0F; t[o++] = 0x8E; size_t jNoDmg = o; o += 4;                 // jle .code
    t[o++] = 0x8B; t[o++] = 0x0D; EmitRip(t, o, g_dmgMul);                   // mov ecx,[dmul]
    t[o++] = 0x83; t[o++] = 0xF9; t[o++] = 0x01;                             // cmp ecx,1
    t[o++] = 0x0F; t[o++] = 0x8E; size_t jNoMul = o; o += 4;                 // jle .nomul
    t[o++] = 0x0F; t[o++] = 0xAF; t[o++] = 0xC1;                             // imul eax,ecx
    size_t lblNoMul = o;
    t[o++] = 0x83; t[o++] = 0x3D; EmitRip(t, o, g_killFlag); t[o++] = 0x01;  // cmp [kill],1
    t[o++] = 0x0F; t[o++] = 0x85; size_t jNoKill = o; o += 4;                // jne .code
    t[o++] = 0x03; t[o++] = 0x02;                                            // add eax,[rdx]
    // ---- common tail ----
    size_t lblCode = o;
    t[o++] = 0x8B; t[o++] = 0x0A;                                            // mov ecx,[rdx]
    t[o++] = 0x89; t[o++] = 0x0D; EmitRip(t, o, g_unitLastHp);               // mov [lastHp],ecx
    t[o++] = 0x44; t[o++] = 0x8B; t[o++] = 0xF0;                             // mov r14d,eax
    t[o++] = 0x29; t[o++] = 0x02;                                            // sub [rdx],eax
    t[o++] = 0x59;                                                           // pop rcx
    t[o++] = 0x5A;                                                           // pop rdx
    EmitJmp(t, o, g_hkUnit.target + 9);
    PatchJcc(t, jEnemy, lblEnemy);
    PatchJcc(t, jAllyOut, lblCode);
    PatchJcc(t, jBigType, lblCode);
    PatchJcc(t, jNoDmg, lblCode);
    PatchJcc(t, jNoMul, lblNoMul);
    PatchJcc(t, jNoKill, lblCode);
    { int32_t d = (int32_t)((intptr_t)(t + lblCode) - (intptr_t)(t + jAllyDone + 4)); memcpy(t + jAllyDone, &d, 4); }
    Log("unit: stub logic bytes=%u (data at +0x%X)", (unsigned)o, (unsigned)HK_DATA);
    return HkFinish(g_hkUnit);
}

static void SetAllyInvuln(bool on)
{
    g_allyOn = on;
    if (g_allyFlag) *g_allyFlag = on ? 1 : 0;
    Log("unit: ally_invuln=%d", on ? 1 : 0);
}

static void SetOneKill(bool on)
{
    if (g_killFlag) *g_killFlag = on ? 1 : 0;
    Log("unit: one_kill=%d", on ? 1 : 0);
}

static void SetDmgMul(int v)
{
    if (v < 1) v = 1;
    if (g_dmgMul) *g_dmgMul = v;
    Log("unit: dmul=%d", v);
}

// ---------------------------------------------------------------------------
// hero xp: addss xmm1,xmm0 ; cvttss2si eax,xmm1 ; mov [rdi+0xA44],eax
// ---------------------------------------------------------------------------
static InlineHk          g_hkXp;
static volatile int32_t* g_xpMul     = nullptr;
static volatile int32_t* g_xpLock    = nullptr;
static volatile int64_t* g_xpCalls   = nullptr;
static volatile int32_t* g_xpLastNew = nullptr;
static volatile int32_t* g_xpLastVal = nullptr;
static int64_t           g_xpCallsLogged = -1;
static bool              g_xpLockOn  = false;

static bool InstallXpHook()
{
    g_hkXp.name = "英雄经验";
    if (g_hkXp.installed) return true;

    static const unsigned char pat[] = {0xF3,0x0F,0x58,0xC8,0xF3,0x0F,0x2C,0xC1,0x89,0,0,0,0,0,0x48,0x8B};
    static const char msk[] = "xxxxxxxxx?????xx";
    unsigned char* hit = ScanGameExec(pat, msk, sizeof(pat));
    if (!hit)
    {
        Log("xp: pattern not found");
        snprintf(g_hkXp.status, sizeof(g_hkXp.status), "英雄经验: 未找到特征码");
        return false;
    }
    if (!HkBegin(g_hkXp, hit, 8)) return false;

    unsigned char* stub = g_hkXp.stub;
    g_xpMul     = (volatile int32_t*)(stub + HK_DATA + 0x00);
    g_xpLock    = (volatile int32_t*)(stub + HK_DATA + 0x04);
    g_xpCalls   = (volatile int64_t*)(stub + HK_DATA + 0x08);
    g_xpLastNew = (volatile int32_t*)(stub + HK_DATA + 0x10);
    g_xpLastVal = (volatile int32_t*)(stub + HK_DATA + 0x14);
    *g_xpMul = 1; *g_xpLock = 0; *g_xpCalls = 0; *g_xpLastNew = 0; *g_xpLastVal = 0;

    unsigned char* t = stub + HK_LOGIC;
    size_t o = 0;
    t[o++] = 0x48; t[o++] = 0xFF; t[o++] = 0x05; EmitRip(t, o, g_xpCalls);   // inc qword [calls]
    t[o++] = 0xF3; t[o++] = 0x0F; t[o++] = 0x11; EmitRip(t, o, g_xpLastNew);  // movss [lastNew],xmm0
    t[o++] = 0x8B; t[o++] = 0x05; EmitRip(t, o, g_xpMul);                    // mov eax,[xpmul]
    t[o++] = 0x83; t[o++] = 0xF8; t[o++] = 0x01;                             // cmp eax,1
    t[o++] = 0x0F; t[o++] = 0x8E; size_t jNoMul = o; o += 4;                 // jle .nomul
    t[o++] = 0xF3; t[o++] = 0x0F; t[o++] = 0x2A; t[o++] = 0xD0;              // cvtsi2ss xmm2,eax
    t[o++] = 0xF3; t[o++] = 0x0F; t[o++] = 0x59; t[o++] = 0xC2;              // mulss xmm0,xmm2
    size_t lblNoMul = o;
    t[o++] = 0x83; t[o++] = 0x3D; EmitRip(t, o, g_xpLock); t[o++] = 0x01;    // cmp [lock],1
    t[o++] = 0x0F; t[o++] = 0x85; size_t jNoLock = o; o += 4;                // jne .code
    t[o++] = 0xB8; { int32_t v = 99999999; memcpy(t + o, &v, 4); o += 4; }   // mov eax,99999999
    t[o++] = 0xF3; t[o++] = 0x0F; t[o++] = 0x2A; t[o++] = 0xC8;              // cvtsi2ss xmm1,eax
    size_t lblCode = o;
    t[o++] = 0xF3; t[o++] = 0x0F; t[o++] = 0x58; t[o++] = 0xC8;              // addss xmm1,xmm0
    t[o++] = 0xF3; t[o++] = 0x0F; t[o++] = 0x2C; t[o++] = 0xC1;              // cvttss2si eax,xmm1
    t[o++] = 0x89; t[o++] = 0x05; EmitRip(t, o, g_xpLastVal);                // mov [lastVal],eax
    EmitJmp(t, o, g_hkXp.target + 8);
    PatchJcc(t, jNoMul, lblNoMul);
    PatchJcc(t, jNoLock, lblCode);
    return HkFinish(g_hkXp);
}

static void SetXpLock(bool on)
{
    g_xpLockOn = on;
    if (g_xpLock) *g_xpLock = on ? 1 : 0;
    Log("xp: lock=%d", on ? 1 : 0);
}

static void SetXpMul(int v)
{
    if (v < 1) v = 1;
    if (g_xpMul) *g_xpMul = v;
    Log("xp: mul=%d", v);
}

// ---------------------------------------------------------------------------
// hero skill requirements: cmp eax,[rdi+0x6E0] ; jl ...
// ---------------------------------------------------------------------------
static InlineHk          g_hkSkill;
static volatile int32_t* g_skillFlag  = nullptr;
static volatile int64_t* g_skillCalls = nullptr;
static int64_t           g_skillCallsLogged = -1;
static bool              g_skillOn    = false;

static bool InstallSkillHook()
{
    g_hkSkill.name = "英雄技能";
    if (g_hkSkill.installed) return true;

    // 3B 87 <reqOff> (cmp eax,[rdi+reqOff]) ; 7C <rel8> (jl back) ; 8B 87 <off> ; 39 ...
    // only the unique site has "8B 87" after the jl, the other three use 8B CE/D6/F5
    static const unsigned char pat[] = {0x3B,0x87,0,0,0,0,0x7C,0x00,0x8B,0x87,0,0,0,0,0x39};
    static const char msk[] = "xx????x?xx????x";
    unsigned char* hit = ScanGameExec(pat, msk, sizeof(pat));
    if (!hit)
    {
        Log("skill: pattern not found");
        snprintf(g_hkSkill.status, sizeof(g_hkSkill.status), "英雄技能: 未找到特征码");
        return false;
    }
    if (!HkBegin(g_hkSkill, hit, 6)) return false;

    unsigned char* stub = g_hkSkill.stub;
    g_skillFlag  = (volatile int32_t*)(stub + HK_DATA + 0x00);
    g_skillCalls = (volatile int64_t*)(stub + HK_DATA + 0x08);
    *g_skillFlag = 0; *g_skillCalls = 0;

    const int32_t reqOff = *(const int32_t*)(hit + 2);
    Log("skill: hit 0x%p reqOff=0x%X", (void*)hit, (unsigned)reqOff);

    unsigned char* t = stub + HK_LOGIC;
    size_t o = 0;
    t[o++] = 0x48; t[o++] = 0xFF; t[o++] = 0x05; EmitRip(t, o, g_skillCalls); // inc qword [calls]
    t[o++] = 0x83; t[o++] = 0x3D; EmitRip(t, o, g_skillFlag); t[o++] = 0x01;  // cmp [flag],1
    t[o++] = 0x0F; t[o++] = 0x85; size_t jNo = o; o += 4;                     // jne .orig
    t[o++] = 0xB8; { int32_t v = 9999; memcpy(t + o, &v, 4); o += 4; }        // mov eax,9999
    size_t lblOrig = o;
    t[o++] = 0x3B; t[o++] = 0x87;                                             // cmp eax,[rdi+reqOff]
    { int32_t d = reqOff; memcpy(t + o, &d, 4); o += 4; }
    EmitJmp(t, o, g_hkSkill.target + 6);
    PatchJcc(t, jNo, lblOrig);
    return HkFinish(g_hkSkill);
}

static void SetSkillFree(bool on)
{
    g_skillOn = on;
    if (g_skillFlag) *g_skillFlag = on ? 1 : 0;
    Log("skill: free=%d", on ? 1 : 0);
}

// ---------------------------------------------------------------------------
// upgrade points: mov ecx,[rbx+0x44] ; sub ecx,eax ; mov eax,ecx ; ret
// ---------------------------------------------------------------------------
static InlineHk          g_hkUpg;
static volatile int32_t* g_upgFlag  = nullptr;
static volatile int32_t* g_upgAmt   = nullptr;
static volatile int64_t* g_upgCalls = nullptr;
static int64_t           g_upgCallsLogged = -1;
static bool              g_upgOn    = false;

static bool InstallUpgHook()
{
    g_hkUpg.name = "升级点";
    if (g_hkUpg.installed) return true;

    static const unsigned char pat[] = {0x8B,0x4B,0x44,0x2B,0xC8,0x8B,0xC1,0x48,0x83,0xC4,0,0,0xC3};
    static const char msk[] = "xxxxxxxxxx??x";
    unsigned char* hit = ScanGameExec(pat, msk, sizeof(pat));
    if (!hit)
    {
        Log("upg: pattern not found");
        snprintf(g_hkUpg.status, sizeof(g_hkUpg.status), "升级点: 未找到特征码");
        return false;
    }
    if (!HkBegin(g_hkUpg, hit, 5)) return false;

    unsigned char* stub = g_hkUpg.stub;
    g_upgFlag  = (volatile int32_t*)(stub + HK_DATA + 0x00);
    g_upgAmt   = (volatile int32_t*)(stub + HK_DATA + 0x04);
    g_upgCalls = (volatile int64_t*)(stub + HK_DATA + 0x08);
    *g_upgFlag = 0; *g_upgAmt = 99; *g_upgCalls = 0;

    unsigned char* t = stub + HK_LOGIC;
    size_t o = 0;
    t[o++] = 0x48; t[o++] = 0xFF; t[o++] = 0x05; EmitRip(t, o, g_upgCalls);  // inc qword [calls]
    t[o++] = 0x83; t[o++] = 0x3D; EmitRip(t, o, g_upgFlag); t[o++] = 0x01;   // cmp [flag],1
    t[o++] = 0x0F; t[o++] = 0x85; size_t jNo = o; o += 4;                    // jne .orig
    t[o++] = 0x8B; t[o++] = 0x0D; EmitRip(t, o, g_upgAmt);                   // mov ecx,[amt]
    t[o++] = 0x03; t[o++] = 0xC8;                                            // add ecx,eax
    t[o++] = 0x89; t[o++] = 0x4B; t[o++] = 0x44;                             // mov [rbx+0x44],ecx
    t[o++] = 0x2B; t[o++] = 0xC8;                                            // sub ecx,eax
    t[o++] = 0xE9; size_t jDone = o; o += 4;                                 // jmp .done
    size_t lblOrig = o;
    t[o++] = 0x8B; t[o++] = 0x4B; t[o++] = 0x44;                             // mov ecx,[rbx+0x44]
    t[o++] = 0x2B; t[o++] = 0xC8;                                            // sub ecx,eax
    size_t lblDone = o;
    EmitJmp(t, o, g_hkUpg.target + 5);
    PatchJcc(t, jNo, lblOrig);
    { int32_t d = (int32_t)((intptr_t)(t + lblDone) - (intptr_t)(t + jDone + 4)); memcpy(t + jDone, &d, 4); }
    return HkFinish(g_hkUpg);
}

static void SetUpgPoints(bool on)
{
    g_upgOn = on;
    if (g_upgFlag) *g_upgFlag = on ? 1 : 0;
    Log("upg: points=%d", on ? 1 : 0);
}

// ---------------------------------------------------------------------------
// aggregate
// ---------------------------------------------------------------------------
static bool g_patchesDone = false;

static void InstallAllHooks()
{
    if (g_patchesDone) return;
    g_patchesDone = true;
    InstallMoneyHook();
    InstallHpHook();
    InstallUnitHook();
    InstallXpHook();
    InstallSkillHook();
    InstallUpgHook();
    Log("patches: money=%d hp=%d unit=%d xp=%d skill=%d upg=%d",
        (int)g_moneyInstalled, (int)g_hkHp.installed, (int)g_hkUnit.installed,
        (int)g_hkXp.installed, (int)g_hkSkill.installed, (int)g_hkUpg.installed);
}

static void SetAllFeatures(bool on)
{
    SetMoneyLock(on);
    SetHpLock(on);
    SetAllyInvuln(on);
    SetOneKill(on);
    SetXpLock(on);
    SetSkillFree(on);
    SetUpgPoints(on);
    Log("patches: all=%d", on ? 1 : 0);
}


static void SetMoneyLock(bool on)
{
    g_moneyOn = on;
    if (g_moneyFlag) *g_moneyFlag = on ? 1 : 0;
    if (on)
        snprintf(g_moneyStatus, sizeof(g_moneyStatus), "金币锁定已开启（数值 %d）",
                 g_moneyAmt ? *g_moneyAmt : 0);
    else
        snprintf(g_moneyStatus, sizeof(g_moneyStatus), "金币锁定已关闭");
    Log("money: lock=%d amount=%d", on ? 1 : 0, g_moneyAmt ? *g_moneyAmt : 0);
}

// poll the script file; runs at most once per distinct content
static void SetGameSpeed(float mul);   // 定义在 DrawScannerUI 之前

static void PollScanCommand()
{
    char tmp[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, tmp)) return;
    char path[MAX_PATH] = {0};
    snprintf(path, sizeof(path), "%skr4mod_cmd.txt", tmp);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[256] = {0};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    if (strcmp(buf, g_lastCmd) == 0) return;

    // first poll after injection: swallow whatever is still sitting in the
    // command file so a leftover "mon"/"write ..." from a previous session is
    // never replayed behind the user's back
    static bool s_cmdPrimed = false;
    if (!s_cmdPrimed)
    {
        s_cmdPrimed = true;
        strncpy(g_lastCmd, buf, sizeof(g_lastCmd) - 1);
        g_lastCmd[sizeof(g_lastCmd) - 1] = 0;
        Log("cmd channel primed (stale command ignored): %.32s", buf);
        return;
    }

    strncpy(g_lastCmd, buf, sizeof(g_lastCmd) - 1);

    char op[32] = {0};
    double val = 0.0;
    if (sscanf(buf, "%31s %lf", op, &val) >= 1)
    {
        if (strcmp(op, "scan") == 0)         ScanStart(val, false);
        else if (strcmp(op, "scanf") == 0)   ScanStart(val, true);
        else if (strcmp(op, "filter") == 0)  ScanFilter(val, false);
        else if (strcmp(op, "filterf") == 0) ScanFilter(val, true);
        else if (strcmp(op, "write") == 0)   ScanWrite(val, false);
        else if (strcmp(op, "writef") == 0)  ScanWrite(val, true);
        else if (strcmp(op, "clear") == 0)
        {
            g_hits.clear();
            g_scanning = false;
            snprintf(g_status, sizeof(g_status), "已清空");
            Log("scan: cleared");
        }
        else if (strcmp(op, "mhook") == 0) InstallMoneyHook();
        else if (strcmp(op, "mon") == 0)   SetMoneyLock(true);
        else if (strcmp(op, "moff") == 0)  SetMoneyLock(false);
        else if (strcmp(op, "mset") == 0)
        {
            if (g_moneyAmt) *g_moneyAmt = (int32_t)val;
            Log("money: amount=%d", g_moneyAmt ? (int)*g_moneyAmt : -1);
        }
        else if (strcmp(op, "mmul") == 0)
        {
            if (g_moneyMul)
            {
                *g_moneyMul = (val < 1.0) ? 1 : (int32_t)val;
                Log("money: mul=%d", (int)*g_moneyMul);
            }
        }
        else if (strcmp(op, "speed") == 0)   SetGameSpeed((float)val);
        else if (strcmp(op, "hookall") == 0)  InstallAllHooks();
        else if (strcmp(op, "allon") == 0)    SetAllFeatures(true);
        else if (strcmp(op, "alloff") == 0)   SetAllFeatures(false);
        else if (strcmp(op, "hmon") == 0)     SetHpLock(true);
        else if (strcmp(op, "hmoff") == 0)    SetHpLock(false);
        else if (strcmp(op, "hset") == 0)
        {
            if (g_hpAmt) *g_hpAmt = (int32_t)val;
            Log("hp: amount=%d", g_hpAmt ? (int)*g_hpAmt : -1);
        }
        else if (strcmp(op, "allyon") == 0)   SetAllyInvuln(true);
        else if (strcmp(op, "allyoff") == 0)  SetAllyInvuln(false);
        else if (strcmp(op, "killon") == 0)   SetOneKill(true);
        else if (strcmp(op, "killoff") == 0)  SetOneKill(false);
        else if (strcmp(op, "dmul") == 0)     SetDmgMul((int)val);
        else if (strcmp(op, "xmon") == 0)     SetXpLock(true);
        else if (strcmp(op, "xmoff") == 0)    SetXpLock(false);
        else if (strcmp(op, "xmul") == 0)     SetXpMul((int)val);
        else if (strcmp(op, "skon") == 0)     SetSkillFree(true);
        else if (strcmp(op, "skoff") == 0)    SetSkillFree(false);
        else if (strcmp(op, "upon") == 0)     SetUpgPoints(true);
        else if (strcmp(op, "upoff") == 0)    SetUpgPoints(false);
        else if (strcmp(op, "upamt") == 0)
        {
            if (g_upgAmt) *g_upgAmt = (val < 1.0) ? 1 : (int32_t)val;
            Log("upg: amount=%d", g_upgAmt ? (int)*g_upgAmt : -1);
        }
        else if (strcmp(op, "status") == 0)
        {
            Log("hooks: money=%d hp=%d unit=%d xp=%d skill=%d upg=%d",
                (int)g_moneyInstalled, (int)g_hkHp.installed, (int)g_hkUnit.installed,
                (int)g_hkXp.installed, (int)g_hkSkill.installed, (int)g_hkUpg.installed);
            Log("calls: money=%lld hp=%lld unit=%lld xp=%lld skill=%lld upg=%lld",
                (long long)(g_moneyCount ? *g_moneyCount : 0),
                (long long)(g_hpCalls ? *g_hpCalls : 0),
                (long long)(g_unitCalls ? *g_unitCalls : 0),
                (long long)(g_xpCalls ? *g_xpCalls : 0),
                (long long)(g_skillCalls ? *g_skillCalls : 0),
                (long long)(g_upgCalls ? *g_upgCalls : 0));
            Log("money: locked=%d amt=%d mul=%d",
                g_moneyFlag ? *g_moneyFlag : -1, g_moneyAmt ? (int)*g_moneyAmt : -1,
                g_moneyMul ? (int)*g_moneyMul : -1);
            Log("targets: money=0x%llX hp=0x%llX unit=0x%llX xp=0x%llX skill=0x%llX upg=0x%llX",
                (unsigned long long)g_moneyTarget,
                (unsigned long long)g_hkHp.target, (unsigned long long)g_hkUnit.target,
                (unsigned long long)g_hkXp.target, (unsigned long long)g_hkSkill.target,
                (unsigned long long)g_hkUpg.target);
        }
        else if (strcmp(op, "unhook") == 0)
        {
            g_pendingUnhook = true;
            Log("cmd: unhook queued");
        }
    }
}

// ---------------------------------------------------------------------------
// 游戏速率：不碰系统时钟，只缩放游戏"感知到的时间增量"。
//
// v22 的教训：只把 QueryPerformanceCounter 换掉，游戏拿它算 dt 变快了，但
// Sleep / GetTickCount 仍然是真实速度，等待目标与时钟不同步，主线程自锁卡死。
//
// v23 改成 CE 式多点时间源改写，让所有时间源走同一条虚拟时间线：
//   QueryPerformanceCounter / GetTickCount64 / GetTickCount / timeGetTime
//   → 返回按倍率缩放的累积时间（每线程独立增量基准，倍率改了也不跳变）
//   Sleep / SleepEx → 毫秒数除以倍率，节流等待跟着一起变快
//
// 全程无锁：倍率是 LONG 定点（×1000），每线程只有自己的 thread_local 基准。
// 倍率 1.00x 时所有 detour 走快速返回，链路上只剩一次转发调用。
// ---------------------------------------------------------------------------
#define SPEED_MUL_SCALE 1000

typedef BOOL      (WINAPI *PFN_QPC)(LARGE_INTEGER*);
typedef ULONGLONG (WINAPI *PFN_GTC64)(void);
typedef DWORD     (WINAPI *PFN_GTC)(void);
typedef DWORD     (WINAPI *PFN_TGT)(void);
typedef void      (WINAPI *PFN_SLEEP)(DWORD);
typedef DWORD     (WINAPI *PFN_SLEEPEX)(DWORD, BOOL);

static PFN_QPC     g_qpcCall     = nullptr;   // IAT 槽原值（Steam 重建过的真实实现）
static PFN_GTC64   g_gtc64Call   = nullptr;
static PFN_GTC     g_gtcCall     = nullptr;
static PFN_TGT     g_tgtCall     = nullptr;
static PFN_SLEEP   g_sleepCall   = nullptr;
static PFN_SLEEPEX g_sleepExCall = nullptr;
static PFN_GTC64   g_gtc64Real   = nullptr;   // 兜底用的系统实现

static volatile LONG   g_mulQ1000   = SPEED_MUL_SCALE;
// 一旦接管就永久走虚拟时间线：倍率回到 1.00x 也只改增量比例，
// 不能直接返回真实时间，否则时间源发生跳变（v23 切回 1.00x 卡死的原因）
static volatile LONG   g_speedActive = 0;
static volatile LONG64 g_speedCalls = 0;
static float  g_speedMul    = 1.0f;           // 面板显示用
static bool   g_speedReady  = false;
static int    g_speedSlots  = 0;
static uintptr_t g_mainBase = 0;
static size_t    g_mainSize = 0;
static char   g_speedStatus[160] = "游戏速率: 1.00x（未安装）";

struct SpeedTls { long long lastReal; long long virt; long long rem; };
static thread_local SpeedTls t_qpc  = {0, 0, 0};
static thread_local SpeedTls t_tick = {0, 0, 0};

static void InitMainModuleRange()
{
    HMODULE base = GetModuleHandleA(nullptr);
    if (!base) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_mainBase = (uintptr_t)base;
    g_mainSize = nt->OptionalHeader.SizeOfImage;
}

// 只缩放"游戏主模块代码"发起的调用：本 DLL 自己的 ImGui 计时、窗口 proc、
// Steam 客户端线程都保持真实时间，面板 FPS 才不会被一起乘倍率。
static inline bool SpeedInGameCode(void* ret)
{
    const uintptr_t r = (uintptr_t)ret;
    return g_mainBase && r >= g_mainBase && r < g_mainBase + g_mainSize;
}

// 把真实增量按倍率折算成虚拟增量，余数累积避免长期漂移
static inline long long SpeedScaleDelta(SpeedTls& t, long long real, LONG num)
{
    if (t.lastReal == 0)          // 首次调用：从真实值起步，不制造时间跳变
    {
        t.lastReal = real;
        t.virt     = real;
        return real;
    }
    long long d = real - t.lastReal;
    if (d < 0) d = 0;
    t.lastReal = real;
    long long scaled = d * (long long)num + t.rem;
    t.virt += scaled / SPEED_MUL_SCALE;
    t.rem   = scaled % SPEED_MUL_SCALE;
    return t.virt;
}

static BOOL WINAPI QpcDetour(LARGE_INTEGER* li)
{
    if (!g_qpcCall) return FALSE;
    BOOL ok = g_qpcCall(li);
    if (!ok || !li) return ok;
    if (!g_speedActive) return ok;
    if (!SpeedInGameCode(__builtin_return_address(0))) return ok;
    const LONG num = g_mulQ1000;
    li->QuadPart = SpeedScaleDelta(t_qpc, li->QuadPart, num);
    ++g_speedCalls;
    return ok;
}

static ULONGLONG WINAPI Gtc64Detour(void)
{
    ULONGLONG real = g_gtc64Call ? g_gtc64Call()
                                 : (g_gtc64Real ? g_gtc64Real() : ::GetTickCount64());
    if (!g_speedActive) return real;
    if (!SpeedInGameCode(__builtin_return_address(0))) return real;
    const LONG num = g_mulQ1000;
    ++g_speedCalls;
    return (ULONGLONG)SpeedScaleDelta(t_tick, (long long)real, num);
}

static DWORD WINAPI GtcDetour(void)
{
    ULONGLONG real = g_gtc64Call ? g_gtc64Call()
                                 : (g_gtc64Real ? g_gtc64Real() : ::GetTickCount64());
    if (!g_speedActive) return (DWORD)real;
    if (!SpeedInGameCode(__builtin_return_address(0))) return (DWORD)real;
    const LONG num = g_mulQ1000;
    ++g_speedCalls;
    return (DWORD)(ULONGLONG)SpeedScaleDelta(t_tick, (long long)real, num);
}

static DWORD WINAPI TgtDetour(void)
{
    ULONGLONG real = g_gtc64Call ? g_gtc64Call()
                                 : (g_gtc64Real ? g_gtc64Real() : ::GetTickCount64());
    if (!g_speedActive) return (DWORD)real;
    if (!SpeedInGameCode(__builtin_return_address(0))) return (DWORD)real;
    const LONG num = g_mulQ1000;
    ++g_speedCalls;
    return (DWORD)(ULONGLONG)SpeedScaleDelta(t_tick, (long long)real, num);
}

static inline DWORD SpeedSleepMs(DWORD ms, LONG num)
{
    unsigned long long d = (unsigned long long)ms * SPEED_MUL_SCALE / (unsigned long long)num;
    if (d == 0) d = 1;                    // 至少让出 1ms，避免变成忙等
    return (DWORD)d;
}

static void WINAPI SleepDetour(DWORD ms)
{
    if (ms && SpeedInGameCode(__builtin_return_address(0)))
    {
        const LONG num = g_mulQ1000;
        if (num != SPEED_MUL_SCALE)
        {
            DWORD d = SpeedSleepMs(ms, num);
            if (g_sleepCall) g_sleepCall(d); else ::Sleep(d);
            return;
        }
    }
    if (g_sleepCall) g_sleepCall(ms); else ::Sleep(ms);
}

static DWORD WINAPI SleepExDetour(DWORD ms, BOOL alertable)
{
    if (ms && SpeedInGameCode(__builtin_return_address(0)))
    {
        const LONG num = g_mulQ1000;
        if (num != SPEED_MUL_SCALE)
        {
            DWORD d = SpeedSleepMs(ms, num);
            if (g_sleepExCall) return g_sleepExCall(d, alertable);
            return ::SleepEx(d, alertable);
        }
    }
    if (g_sleepExCall) return g_sleepExCall(ms, alertable);
    return ::SleepEx(ms, alertable);
}

// 在游戏主模块导入表里按函数名换掉全部时间源，命中数写入日志
static int PatchGameTimeIat()
{
    HMODULE base = GetModuleHandleA(nullptr);
    if (!base) return -1;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;

    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return -1;

    struct Spec { const char* name; void* det; void** callOut; };
    const Spec specs[] = {
        { "QueryPerformanceCounter", (void*)&QpcDetour,     (void**)&g_qpcCall     },
        { "GetTickCount64",          (void*)&Gtc64Detour,   (void**)&g_gtc64Call   },
        { "GetTickCount",            (void*)&GtcDetour,     (void**)&g_gtcCall     },
        { "timeGetTime",             (void*)&TgtDetour,     (void**)&g_tgtCall     },
        { "Sleep",                   (void*)&SleepDetour,   (void**)&g_sleepCall   },
        { "SleepEx",                 (void*)&SleepExDetour, (void**)&g_sleepExCall },
    };
    const int nspecs = (int)(sizeof(specs) / sizeof(specs[0]));

    int patched = 0;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + dir.VirtualAddress);
    for (; imp->Name; ++imp)
    {
        const char* dllName = (const char*)((BYTE*)base + imp->Name);
        IMAGE_THUNK_DATA64* thunk = (IMAGE_THUNK_DATA64*)((BYTE*)base + imp->FirstThunk);
        IMAGE_THUNK_DATA64* onames = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA64*)((BYTE*)base + imp->OriginalFirstThunk) : nullptr;
        if (!thunk) continue;

        for (size_t idx = 0; thunk[idx].u1.Function; ++idx)
        {
            const char* fname = nullptr;
            if (onames && onames[idx].u1.AddressOfData &&
                !(onames[idx].u1.Ordinal & IMAGE_ORDINAL_FLAG64))
            {
                fname = (const char*)((BYTE*)base + onames[idx].u1.AddressOfData + sizeof(WORD));
            }
            if (!fname) continue;

            for (int k = 0; k < nspecs; ++k)
            {
                if (strcmp(fname, specs[k].name) != 0) continue;
                FARPROC* slot = (FARPROC*)&thunk[idx].u1.Function;
                if (*slot == (FARPROC)specs[k].det) break;      // 已经换过

                if (specs[k].callOut && !*specs[k].callOut)
                    *specs[k].callOut = (void*)*slot;           // Steam 重建过的真实实现

                DWORD oldProt = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) break;
                *slot = (FARPROC)specs[k].det;
                VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
                ++patched;
                Log("speed: iat %s!%s slot=%p orig=%p", dllName, fname,
                    (void*)slot, (void*)*specs[k].callOut);
                break;
            }
        }
    }
    return patched;
}

static bool InstallGameSpeed()
{
    if (g_speedReady) return true;
    InitMainModuleRange();

    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    if (!kb) kb = GetModuleHandleA("kernel32.dll");
    g_gtc64Real = kb ? (PFN_GTC64)GetProcAddress(kb, "GetTickCount64") : nullptr;
    if (!g_gtc64Real) g_gtc64Real = (PFN_GTC64)&::GetTickCount64;

    g_speedSlots = PatchGameTimeIat();
    if (g_speedSlots <= 0 || !g_qpcCall)
    {
        snprintf(g_speedStatus, sizeof(g_speedStatus), "游戏速率不可用：导入表未命中(%d)", g_speedSlots);
        Log("speed: install failed, slots=%d qpc=%p", g_speedSlots, (void*)g_qpcCall);
        return false;
    }

    g_speedReady = true;
    InterlockedExchange(&g_speedActive, 1);
    snprintf(g_speedStatus, sizeof(g_speedStatus), "游戏速率: %.2fx（%d 个时间源已接管）",
             g_speedMul, g_speedSlots);
    Log("speed: ready, %d slots patched (qpc=%p tick=%p tick64=%p tgt=%p sleep=%p ex=%p)",
        g_speedSlots, (void*)g_qpcCall, (void*)g_gtcCall, (void*)g_gtc64Call,
        (void*)g_tgtCall, (void*)g_sleepCall, (void*)g_sleepExCall);
    return true;
}

static void SetGameSpeed(float mul)
{
    if (mul < 0.10f) mul = 0.10f;
    if (mul > 4.00f) mul = 4.00f;
    if (!InstallGameSpeed())
    {
        Log("speed: not available");
        return;
    }
    g_speedMul = mul;
    LONG q = (LONG)(mul * SPEED_MUL_SCALE + 0.5f);
    if (q < (SPEED_MUL_SCALE / 10)) q = SPEED_MUL_SCALE / 10;
    InterlockedExchange(&g_mulQ1000, q);
    snprintf(g_speedStatus, sizeof(g_speedStatus), "游戏速率: %.2fx（%d 个时间源已接管）",
             mul, g_speedSlots);
    Log("speed: mul=%.2f q=%ld slots=%d calls=%lld", mul, (long)q, g_speedSlots,
        (long long)g_speedCalls);
}

static void DrawScannerUI()
{
    ImGui::Separator();
    ImGui::Text("内存扫描 (%s)", g_scanIsFloat ? "单精度浮点" : "32 位整数");
    if (g_scanning)
        ImGui::Text("扫描中... %zu/%zu 内存区", g_rangeIdx, g_ranges.size());
    else
        ImGui::Text("候选数量: %d", (int)g_hits.size());
    ImGui::TextWrapped("%s", g_status);

    int shown = (int)g_hits.size();
    if (shown > 12) shown = 12;
    for (int i = 0; i < shown; ++i)
    {
        if (!PageReadable(g_hits[i]))
        {
            ImGui::Text("  0x%p = <已失效>", (void*)g_hits[i]);
            continue;
        }
        if (g_scanIsFloat)
        {
            float f = 0.0f;
            memcpy(&f, (const void*)g_hits[i], 4);
            ImGui::Text("  0x%p = %g", (void*)g_hits[i], f);
        }
        else
        {
            int32_t v = 0;
            memcpy(&v, (const void*)g_hits[i], 4);
            ImGui::Text("  0x%p = %d", (void*)g_hits[i], v);
        }
    }

    ImGui::SeparatorText("金币相关");
    ImGui::TextWrapped("%s", g_moneyStatus);
    if (g_moneyInstalled)
    {
        if (g_moneyOn)
        {
            if (ImGui::Button("关闭金币锁定"))
                SetMoneyLock(false);
        }
        else
        {
            if (ImGui::Button("开启金币锁定"))
                SetMoneyLock(true);
        }
        ImGui::SameLine();
        int amt = g_moneyAmt ? (int)*g_moneyAmt : 0;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputInt("##amt", &amt, 100000, 1000000))
        {
            if (g_moneyAmt) *g_moneyAmt = (int32_t)amt;
        }

        // 金币倍率：只放大"正的金币增量"（stub 里 test edi,edi 之后走的分支），
        // 花的钱不会被放大，和 trainer 里 money_mul 的语义一致。
        int mul = g_moneyMul ? (int)*g_moneyMul : 1;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderInt("金币倍率", &mul, 1, 100))
        {
            if (g_moneyMul) *g_moneyMul = (mul < 1) ? 1 : mul;
        }
    }
    else
    {
        if (ImGui::Button("安装金币 Hook"))
            InstallMoneyHook();
    }

    ImGui::SeparatorText("英雄相关");
    ImGui::TextWrapped("%s", g_hkXp.status);
    ImGui::TextWrapped("%s", g_hkSkill.status);
    ImGui::TextWrapped("%s", g_hkUpg.status);
    {
        bool on = g_xpLockOn;
        if (ImGui::Checkbox("英雄经验锁定", &on)) SetXpLock(on);
    }
    {
        int v = g_xpMul ? (int)*g_xpMul : 1;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("经验倍率", &v, 1, 10)) SetXpMul(v);
    }
    {
        bool on = g_skillOn;
        if (ImGui::Checkbox("技能点免消耗", &on)) SetSkillFree(on);
    }
    {
        bool up = g_upgOn;
        if (ImGui::Checkbox("升级点锁定", &up)) SetUpgPoints(up);
    }

    ImGui::SeparatorText("基础功能");
    ImGui::TextWrapped("%s", g_hkHp.status);
    ImGui::TextWrapped("%s", g_hkUnit.status);
    if (ImGui::Button("一键安装全部"))
        InstallAllHooks();
    ImGui::SameLine();
    if (ImGui::Button("全部开启"))
        SetAllFeatures(true);
    ImGui::SameLine();
    if (ImGui::Button("全部关闭"))
        SetAllFeatures(false);

    {
        bool on = g_hpOn;
        if (ImGui::Checkbox("生命锁定", &on)) SetHpLock(on);
        ImGui::SameLine();
        int v = g_hpAmt ? (int)*g_hpAmt : 9999;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("##hpamt", &v, 1000, 10000)) { if (g_hpAmt) *g_hpAmt = v; }
    }
    {
        bool on = g_allyOn;
        if (ImGui::Checkbox("友军无敌", &on)) SetAllyInvuln(on);
        ImGui::SameLine();
        bool ok = (g_killFlag && *g_killFlag != 0);
        if (ImGui::Checkbox("一击必杀", &ok)) SetOneKill(ok);
    }
    {
        int v = g_dmgMul ? (int)*g_dmgMul : 1;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("伤害倍率", &v, 1, 10)) SetDmgMul(v);
    }
    {
        ImGui::TextWrapped("%s", g_speedStatus);
        float sp = g_speedMul;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderFloat("游戏速率", &sp, 0.10f, 4.00f, "%.2fx"))
            SetGameSpeed(sp);
        ImGui::SameLine();
        if (ImGui::Button("1.00x##spd"))
            SetGameSpeed(1.0f);
    }
}

// ---------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------
static void RenderOverlay()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 940.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("KR4 修改器  v0.6  (全部功能)", &g_overlayShown,
                     ImGuiWindowFlags_AlwaysVerticalScrollbar))
    {
        ImGui::Text("注入成功，覆盖层工作正常");
        ImGui::Text("进程 PID: %lu", (unsigned long)GetCurrentProcessId());
        ImGui::Text("构建时间: %s %s", __DATE__, __TIME__);
        // 一次呈现会喂两帧（wglSwapBuffers 与 gdi32!SwapBuffers 各一次），
        // ImGui 的 Framerate 因此翻倍，折半还原真实呈现帧率。
        float dispFps = ImGui::GetIO().Framerate * 0.5f;
        ImGui::Text("FPS: %.1f / %.2f ms", dispFps,
                    1000.0f / (dispFps > 0.0f ? dispFps : 1.0f));
        ImGui::Separator();
        ImGui::Text("GL_VERSION: %s", (const char*)glGetString(GL_VERSION));
        ImGui::Separator();
        ImGui::Text("INSERT 显示/隐藏面板    END 卸载全部 Hook");
        ImGui::Text("中文字体: 系统黑体/雅黑/宋体 18px");
        ImGui::Text("扫描候选: %d", (int)g_hits.size());
        DrawScannerUI();
    }
    ImGui::End();

    ImGui::Render();

    // Snapshot the full fixed-function / client state: the GL2 backend relies
    // on its own push/pop, but the game renders with legacy state we must not
    // leak into (this is what turned the whole scene magenta in v0.2).
    glPushAttrib(0xFFFFFFFF);        // GL_ALL_ATTRIB_BITS
    glPushClientAttrib(0xFFFFFFFF);  // GL_CLIENT_ALL_ATTRIB_BITS
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();

    // 面板必须铺满整窗，且不能被游戏遗留的深度/裁剪/模板状态裁掉一片像素，
    // 否则观感就是"面板某些帧缺一块"的闪烁。glPushAttrib 只做快照不改变状态。
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (GLsizei)io.DisplaySize.x, (GLsizei)io.DisplaySize.y);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glFinish();   // 面板必须在本次 SwapBuffers 前真正落进后备缓冲，否则会隔帧丢失

    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();

    ++g_frameCount;
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
        ++g_glErrorCount;

    ULONGLONG tick = GetTickCount64();
    if (tick - g_lastStatsTick > 3000)
    {
        g_lastStatsTick = tick;
        Log("stats: frames=%d glErrors=%d fps=%.1f vtx=%d idx=%d wgl=%d gdi=%d spd=%.2f qpc=%lld", g_frameCount, g_glErrorCount,
            ImGui::GetIO().Framerate, ImGui::GetDrawData()->TotalVtxCount,
            ImGui::GetDrawData()->TotalIdxCount, g_wglCalls, g_gdiCalls,
            g_speedMul, (long long)g_speedCalls);
        if (g_moneyCount && *g_moneyCount != g_moneyCountLogged)
        {
            g_moneyCountLogged = *g_moneyCount;
            int32_t gv    = g_moneyLastEax ? *g_moneyLastEax : 0;
            int32_t del   = g_moneyLastEdi ? *g_moneyLastEdi : 0;
            int32_t would = gv + del;
            int32_t lockf = g_moneyFlag ? *g_moneyFlag : -1;
            int32_t amt   = g_moneyAmt ? *g_moneyAmt : -1;
            int32_t mul   = g_moneyMul ? *g_moneyMul : -1;
            int32_t applied = (lockf == 1) ? amt : (would * (mul > 0 ? mul : 1));
            Log("money: calls=%lld getter=%d delta=%d wouldWrite=%d applied=%d locked=%d amt=%d mul=%d vtable=0x%llX",
                (long long)*g_moneyCount, gv, del, would, applied, lockf, amt, mul,
                (unsigned long long)(g_moneyLastRbx ? *g_moneyLastRbx : 0));
        }
        if (g_hpCalls && *g_hpCalls != g_hpCallsLogged)
        {
            g_hpCallsLogged = *g_hpCalls;
            Log("hp: calls=%lld flag=%d amt=%d",
                (long long)*g_hpCalls, g_hpFlag ? *g_hpFlag : -1, g_hpAmt ? (int)*g_hpAmt : -1);
        }
        if (g_unitCalls && *g_unitCalls != g_unitCallsLogged)
        {
            g_unitCallsLogged = *g_unitCalls;
            Log("unit: calls=%lld dmg=%d type=%d typeD=%d hp=%d ally=%d dmul=%d kill=%d hpOff=0x%X typeOff=0x%X",
                (long long)*g_unitCalls, g_unitLastDmg ? *g_unitLastDmg : 0,
                g_unitLastType ? *g_unitLastType : -1, g_unitLastTypeD ? *g_unitLastTypeD : -1,
                g_unitLastHp ? *g_unitLastHp : -1, g_allyFlag ? *g_allyFlag : -1,
                g_dmgMul ? *g_dmgMul : -1, g_killFlag ? *g_killFlag : -1,
                (unsigned)g_unitHpOff, (unsigned)g_unitTypeOff);
        }
        if (g_xpCalls && *g_xpCalls != g_xpCallsLogged)
        {
            g_xpCallsLogged = *g_xpCalls;
            Log("xp: calls=%lld new=%d total=%d mul=%d lock=%d",
                (long long)*g_xpCalls, g_xpLastNew ? *g_xpLastNew : 0,
                g_xpLastVal ? *g_xpLastVal : 0, g_xpMul ? *g_xpMul : -1,
                g_xpLock ? *g_xpLock : -1);
        }
        if (g_skillCalls && *g_skillCalls != g_skillCallsLogged)
        {
            g_skillCallsLogged = *g_skillCalls;
            Log("skill: calls=%lld flag=%d", (long long)*g_skillCalls, g_skillFlag ? *g_skillFlag : -1);
        }
        if (g_upgCalls && *g_upgCalls != g_upgCallsLogged)
        {
            g_upgCallsLogged = *g_upgCalls;
            Log("upg: calls=%lld flag=%d amt=%d",
                (long long)*g_upgCalls, g_upgFlag ? *g_upgFlag : -1, g_upgAmt ? (int)*g_upgAmt : -1);
        }
    }

    MaybeSelfShot();
}

static void UnhookAll()
{
    if (g_unhooked) return;
    g_unhooked = true;

    MH_DisableHook(MH_ALL_HOOKS);
    if (g_origWndProc && g_hwnd && IsWindow(g_hwnd))
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);

    // every feature hook is a hand-written inline patch, MH_DisableHook knows
    // nothing about them: restore each original prologue so a fresh DLL can be
    // injected into the same process without re-launching the game
    if (g_moneyInstalled && g_moneyTarget)
    {
        DWORD old = 0;
        if (VirtualProtect(g_moneyTarget, 6, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(g_moneyTarget, g_moneyOrg, 6);
            VirtualProtect(g_moneyTarget, 6, old, &old);
            FlushInstructionCache(GetCurrentProcess(), g_moneyTarget, 6);
            Log("money: hook removed (original prologue restored)");
        }
        g_moneyInstalled = false;
        if (g_moneyStub) { VirtualFree(g_moneyStub, 0, MEM_RELEASE); g_moneyStub = nullptr; }
        g_moneyFlag = nullptr; g_moneyAmt = nullptr; g_moneyCount = nullptr;
        g_moneyLastEdx = nullptr; g_moneyLastEax = nullptr;
        g_moneyLastEdi = nullptr; g_moneyLastRbx = nullptr; g_moneyMul = nullptr;
    }
    HkRestore(g_hkHp);
    HkRestore(g_hkUnit);
    HkRestore(g_hkXp);
    HkRestore(g_hkSkill);
    HkRestore(g_hkUpg);

    g_imguiReady = false;
    Log("unhooked (overlay disabled)");
}

static void OnSwap(HDC dc)
{
    if (g_unhooked) return;

    // install the money hook once, as soon as the first frame is presented
    static bool s_autoHooked = false;
    if (!s_autoHooked)
    {
        s_autoHooked = true;
        InstallAllHooks();
    }

    // scanner work runs regardless of overlay visibility, in small slices so
    // a full address-space sweep never stalls the game's frame pacing
    PollScanCommand();
    // 每 4 次呈现只推进一片 16MB：每帧都扫会让帧时间忽长忽短，观感同样像闪
    if ((++g_scanTick & 3) == 0)
        ScanStep(16u * 1024u * 1024u);

    if (g_pendingUnhook)
    {
        g_pendingUnhook = false;
        UnhookAll();
        return;
    }

    // hotkeys (polled; works even when the overlay is hidden)
    bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !g_prevInsert)
    {
        g_overlayShown = !g_overlayShown;
        Log("overlay toggled -> %d", g_overlayShown ? 1 : 0);
    }
    g_prevInsert = insertDown;

    if (GetAsyncKeyState(VK_END) & 0x8000)
    {
        UnhookAll();
        return;
    }

    if (g_imguiReady)
    {
        ++g_presentCount;

        // pass-through diagnostics mode: create %TEMP%\kr4mod_render.off to make
        // the DLL stop touching the GL pipeline entirely (hook-damage triage).
        if (FlagFileOn("kr4mod_render.off"))
        {
            ULONGLONG now2 = GetTickCount64();
            if (g_lastStatsTick == 0) g_lastStatsTick = now2;
            if (now2 - g_lastStatsTick > 3000)
            {
                g_lastStatsTick = now2;
                Log("passthrough: presents=%d (overlay render disabled)", g_presentCount);
            }
            return;
        }

        if (!g_overlayShown) return;

        // 每次进入 SwapHook 都完整重画一遍。游戏是双缓冲 + 一次呈现流程里
        // wglSwapBuffers 与 gdi32!SwapBuffers 会先后各进一次，任何一次呈现之前
        // 都必须把面板写进当前后备缓冲；按时间去重会让其中一帧的缓冲没有面板，
        // 屏幕上就是"面板时有时无"的闪烁。
        g_lastRender = GetTickCount64();

        if (InterlockedExchange(&g_inRender, 1) != 0)
            return;
        RenderOverlay();
        InterlockedExchange(&g_inRender, 0);
    }
    else if (!g_initTried)
    {
        // retry until a GL context exists
        g_initTried = true;
        if (!InitImGui(dc))
            g_initTried = false;
    }
}

static BOOL WINAPI Hooked_wglSwapBuffers(HDC dc)
{
    InterlockedIncrement(&g_wglCalls);
    OnSwap(dc);
    return g_origWglSwap ? g_origWglSwap(dc) : FALSE;
}

static BOOL WINAPI Hooked_gdiSwapBuffers(HDC dc)
{
    InterlockedIncrement(&g_gdiCalls);
    OnSwap(dc);
    return g_origGdiSwap ? g_origGdiSwap(dc) : FALSE;
}

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------
static DWORD WINAPI InstallThread(LPVOID)
{
    Log("=== kr4mod attached, pid=%lu ===", (unsigned long)GetCurrentProcessId());

    if (MH_Initialize() != MH_OK)
    {
        Log("MH_Initialize failed");
        return 1;
    }

    bool anyHook = false;

    HMODULE hGl = GetModuleHandleA("opengl32.dll");
    if (!hGl) hGl = LoadLibraryA("opengl32.dll");
    void* pWglSwap = hGl ? (void*)GetProcAddress(hGl, "wglSwapBuffers") : nullptr;
    if (pWglSwap)
    {
        MH_STATUS st = MH_CreateHook(pWglSwap, (LPVOID)&Hooked_wglSwapBuffers, (LPVOID*)&g_origWglSwap);
        if (st == MH_OK && MH_EnableHook(pWglSwap) == MH_OK)
        {
            anyHook = true;
            Log("hooked opengl32!wglSwapBuffers @ %p", pWglSwap);
        }
        else
            Log("hook opengl32!wglSwapBuffers failed, status=%d", (int)st);
    }
    else
        Log("opengl32!wglSwapBuffers not found");

    HMODULE hGdi = GetModuleHandleA("gdi32.dll");
    if (!hGdi) hGdi = LoadLibraryA("gdi32.dll");
    void* pSwap = hGdi ? (void*)GetProcAddress(hGdi, "SwapBuffers") : nullptr;
    if (pSwap)
    {
        MH_STATUS st = MH_CreateHook(pSwap, (LPVOID)&Hooked_gdiSwapBuffers, (LPVOID*)&g_origGdiSwap);
        if (st == MH_OK && MH_EnableHook(pSwap) == MH_OK)
        {
            anyHook = true;
            Log("hooked gdi32!SwapBuffers @ %p", pSwap);
        }
        else
            Log("hook gdi32!SwapBuffers failed, status=%d", (int)st);
    }
    else
        Log("gdi32!SwapBuffers not found");

    if (!anyHook)
    {
        Log("FATAL: no swap hook installed");
        return 1;
    }

    Log("swap hook(s) installed; waiting for first present to init ImGui");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        LogInit();
        DisableThreadLibraryCalls(hInst);
        HANDLE t = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
