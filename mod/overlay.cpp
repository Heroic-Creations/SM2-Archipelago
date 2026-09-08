// ----------------------------------------------------------------- overlay.cpp
//
// See overlay.h. A layered top-most window drawing a neon feed of Archipelago
// events over the game -- the in-suit AI look (KAREN / FRIDAY): condensed
// uppercase type, an accent rail, and a real bloom.
//
// Rendering, and why it is done this way:
//
//   GDI cannot draw with an alpha channel -- TextOut writes alpha 0, so a
//   plain layered window has to fall back to a colour key, which gives hard
//   edges and no glow. Instead each line is drawn WHITE ON BLACK into a DIB
//   and that becomes a coverage mask: luminance IS the alpha. The mask is box
//   blurred twice for the bloom, then composited in the line's colour with
//   premultiplied alpha and handed to UpdateLayeredWindow.
//
//   The result is genuinely translucent -- the scene shows through the glow --
//   with no renderer hook. Still pure Win32 on its own thread: it never
//   touches D3D12 or the game thread, so the worst case remains "no window".

#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "logging.h"

namespace overlay {
namespace {

constexpr int   kMaxLines   = 6;
constexpr DWORD kHoldMs     = 9000;
constexpr DWORD kFadeMs     = 1800;
constexpr int   kLineHeight = 30;
constexpr int   kPadX       = 20;
constexpr int   kPadY       = 16;
constexpr int   kRailW      = 3;     // the accent bar down the left of a line
constexpr int   kRailGap    = 12;
constexpr int   kWidth      = 660;
constexpr int   kGlow       = 3;     // box-blur radius for the neon halo
constexpr int   kShadow     = 6;     // wider, softer radius for the dark scrim

struct Line {
    std::string text;
    int         kind;
    DWORD       born;
};

struct Rgb { int r, g, b; };

std::mutex        g_lock;
std::vector<Line> g_lines;
HWND              g_hwnd   = nullptr;
HANDLE            g_thread = nullptr;
volatile bool     g_run    = false;
int               g_off_x  = 40;    // from the game window left edge
int               g_off_y  = 140;   // from the game window BOTTOM edge
std::atomic<int>  g_energy{-1};      // Longest Night meter; -1 = hidden

constexpr int kMeterH = 26;
int Height() { return kLineHeight * kMaxLines + kPadY * 2 + 24 + (g_energy.load() >= 0 ? kMeterH : 0); }

// Suit-AI palette: electric cyan for checks, warm gold for items, pale ice for
// notices. Saturated enough to bloom without turning into a white smear.
Rgb KindColour(int kind) {
    switch (kind) {
        case 0:  return {  90, 226, 255 };
        case 1:  return { 255, 189,  76 };
        case 3:  return { 226, 222, 210 };   // UPDATE: warm off-white, no alarm in it
        default: return { 196, 224, 240 };
    }
}

const char* KindPrefix(int kind) {
    switch (kind) {
        case 0:  return "CHECK";
        case 1:  return "ITEM";
        case 3:  return "UPDATE";
        default: return "LINK";
    }
}

BOOL CALLBACK PickWindow(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    // Never pick our own window. Once the ENERGY meter pushed the feed past
    // 240 px tall it matched this filter, and being topmost it came first in
    // the enumeration: every tick anchored the feed 40 px right of itself and it
    // walked off the screen (seen live 2026-09-07, Night's Grip in Astoria).
    if (hwnd == g_hwnd) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    RECT r{};
    GetWindowRect(hwnd, &r);
    if ((r.right - r.left) < 320 || (r.bottom - r.top) < 240) return TRUE;
    *reinterpret_cast<HWND*>(param) = hwnd;
    return FALSE;
}

HWND GameWindow() {
    HWND found = nullptr;
    EnumWindows(PickWindow, reinterpret_cast<LPARAM>(&found));
    return found;
}


// --- the connect panel --------------------------------------------------------
//
// Three fields typed in game. Submitting writes them where the Python client
// watches; the mod never speaks the Archipelago protocol itself.

bool         g_connected = false;
std::string  g_status_detail = "not connected";

constexpr UINT kMsgRedraw = WM_APP + 12;

// A horizontal then vertical box blur -- separable, so it stays cheap even
// though this runs a few times a second.
void Blur(std::vector<unsigned char>& m, int w, int h, int radius) {
    std::vector<unsigned char> tmp(m.size(), 0);
    const int span = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        for (int x = -radius; x <= radius; ++x)
            if (x >= 0 && x < w) sum += m[y * w + x];
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = static_cast<unsigned char>(sum / span);
            const int add = x + radius + 1, drop = x - radius;
            if (add < w)   sum += m[y * w + add];
            if (drop >= 0) sum -= m[y * w + drop];
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -radius; y <= radius; ++y)
            if (y >= 0 && y < h) sum += tmp[y * w + x];
        for (int y = 0; y < h; ++y) {
            m[y * w + x] = static_cast<unsigned char>(sum / span);
            const int add = y + radius + 1, drop = y - radius;
            if (add < h)   sum += tmp[(add) * w + x];
            if (drop >= 0) sum -= tmp[(drop) * w + x];
        }
    }
}

// Draw one string white-on-black and return its coverage mask.
void RenderMask(HDC dc, HBITMAP dib, unsigned char* bits, int w, int h,
                const char* text, HFONT font, int x, int y,
                std::vector<unsigned char>& mask) {
    RECT all{0, 0, w, h};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &all, black);
    DeleteObject(black);

    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, x, y, text, static_cast<int>(strlen(text)));
    SelectObject(dc, old);
    GdiFlush();

    mask.assign(static_cast<size_t>(w) * h, 0);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char* p = bits + static_cast<size_t>(i) * 4;
        mask[i] = std::max(p[0], std::max(p[1], p[2]));
    }
}

// The neon halo adds light but no contrast, so bright scenes wash the text
// out. This lays a soft BLACK halo underneath first: premultiplied additive
// with a black colour raises alpha while leaving RGB at zero, which darkens
// whatever is behind the glyph without tinting the neon on top of it.
void ComposeShadow(std::vector<unsigned char>& rgba, int w, int h,
                   const std::vector<unsigned char>& shadow, float fade) {
    for (int i = 0; i < w * h; ++i) {
        const float a = (shadow[i] / 255.0f) * 0.85f * fade;
        if (a <= 0.004f) continue;
        unsigned char* p = &rgba[static_cast<size_t>(i) * 4];
        const int na = static_cast<int>((a > 1.0f ? 1.0f : a) * 255.0f);
        p[3] = static_cast<unsigned char>(std::min(255, p[3] + na));
    }
}

void Compose(std::vector<unsigned char>& rgba, int w, int h,
             const std::vector<unsigned char>& core,
             const std::vector<unsigned char>& glow, Rgb c, float fade) {
    for (int i = 0; i < w * h; ++i) {
        const float k = core[i] / 255.0f;
        const float g = glow[i] / 255.0f;
        // Alpha: the glyph itself, plus a softer halo around it.
        float a = (k + g * 0.85f) * fade;
        if (a <= 0.004f) continue;
        if (a > 1.0f) a = 1.0f;
        // The core reads brighter than the halo, which keeps the text legible
        // instead of dissolving into its own bloom.
        const float lift = k * 0.45f;
        float r = (c.r / 255.0f) * (0.55f + lift);
        float gg = (c.g / 255.0f) * (0.55f + lift);
        float b = (c.b / 255.0f) * (0.55f + lift);
        if (r > 1.0f) r = 1.0f;
        if (gg > 1.0f) gg = 1.0f;
        if (b > 1.0f) b = 1.0f;

        unsigned char* p = &rgba[static_cast<size_t>(i) * 4];
        const int na = static_cast<int>(a * 255.0f);
        // Premultiplied, and additive against anything already drawn so
        // overlapping glows accumulate rather than clip.
        const int nr = static_cast<int>(r * a * 255.0f);
        const int ng = static_cast<int>(gg * a * 255.0f);
        const int nb = static_cast<int>(b * a * 255.0f);
        p[0] = static_cast<unsigned char>(std::min(255, p[0] + nb));
        p[1] = static_cast<unsigned char>(std::min(255, p[1] + ng));
        p[2] = static_cast<unsigned char>(std::min(255, p[2] + nr));
        p[3] = static_cast<unsigned char>(std::min(255, p[3] + na));
    }
}

void Redraw() {
    if (!g_hwnd) return;
    const int w = kWidth, h = Height();

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* raw = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!dib) { DeleteDC(dc); ReleaseDC(nullptr, screen); return; }
    HGDIOBJ oldbmp = SelectObject(dc, dib);
    unsigned char* bits = static_cast<unsigned char*>(raw);

    // Condensed, techy, and on every Windows 10/11 box; Consolas is the
    // fallback if a machine somehow lacks it.
    HFONT font = CreateFontA(21, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FF_DONTCARE, "Bahnschrift SemiCondensed");
    if (!font) {
        font = CreateFontA(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, FF_DONTCARE, "Consolas");
    }

    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4, 0);
    std::vector<unsigned char> core, glow, shadow;

    std::vector<Line> copy;
    {
        std::lock_guard<std::mutex> g(g_lock);
        copy = g_lines;
    }

    const DWORD now = GetTickCount();

    // Bottom-align the feed inside its block. Drawing from the top left a wide
    // empty gap between the last message and the status line whenever fewer
    // than kMaxLines were showing, which is most of the time.
    int visible = 0;
    for (const Line& l : copy) {
        const DWORD a = now - l.born;
        if (a <= kHoldMs + kFadeMs) ++visible;
    }
    int y = kPadY + (kMaxLines - (visible < kMaxLines ? visible : kMaxLines)) * kLineHeight;

    for (const Line& l : copy) {
        const DWORD age = now - l.born;
        if (age > kHoldMs + kFadeMs) continue;
        float fade = 1.0f;
        if (age > kHoldMs) fade = 1.0f - float(age - kHoldMs) / float(kFadeMs);
        if (fade <= 0.0f) continue;

        const Rgb c = KindColour(l.kind);
        char buf[420];
        sprintf_s(buf, "%-6s %s", KindPrefix(l.kind), l.text.c_str());

        RenderMask(dc, dib, bits, w, h, buf, font, kPadX + kRailW + kRailGap, y, core);

        // Dark scrim first (wider and softer), then the neon over it.
        shadow = core;
        Blur(shadow, w, h, kShadow);
        ComposeShadow(rgba, w, h, shadow, fade);

        glow = core;
        Blur(glow, w, h, kGlow);
        Compose(rgba, w, h, core, glow, c, fade);

        // The accent rail: a solid bar in the line's colour, drawn straight
        // into the buffer rather than through GDI so it keeps its alpha.
        for (int ry = y + 3; ry < y + kLineHeight - 6 && ry < h; ++ry) {
            for (int rx = kPadX; rx < kPadX + kRailW && rx < w; ++rx) {
                unsigned char* p = &rgba[(static_cast<size_t>(ry) * w + rx) * 4];
                const float a = 0.92f * fade;
                p[0] = static_cast<unsigned char>(std::min(255, p[0] + int(c.b * a)));
                p[1] = static_cast<unsigned char>(std::min(255, p[1] + int(c.g * a)));
                p[2] = static_cast<unsigned char>(std::min(255, p[2] + int(c.r * a)));
                p[3] = static_cast<unsigned char>(std::min(255, p[3] + int(255 * a)));
            }
        }
        y += kLineHeight;
    }

    // --- status dot ------------------------------------------------------------
    {
        // Just under the last message, not under the whole block.
        const int dy = kLineHeight * kMaxLines + kPadY - 8;
        const Rgb dot = g_connected ? Rgb{90, 235, 130} : Rgb{235, 90, 90};
        for (int y2 = dy + 6; y2 < dy + 16 && y2 < h; ++y2) {
            for (int x2 = kPadX; x2 < kPadX + 10 && x2 < w; ++x2) {
                // round-ish: skip the corners
                const int cx = x2 - (kPadX + 5), cy = y2 - (dy + 11);
                if (cx * cx + cy * cy > 25) continue;
                unsigned char* q = &rgba[(static_cast<size_t>(y2) * w + x2) * 4];
                q[0] = static_cast<unsigned char>(std::min(255, q[0] + dot.b));
                q[1] = static_cast<unsigned char>(std::min(255, q[1] + dot.g));
                q[2] = static_cast<unsigned char>(std::min(255, q[2] + dot.r));
                q[3] = 255;
            }
        }
        char line[160];
        {
            std::lock_guard<std::mutex> g(g_lock);
            sprintf_s(line, "%s", g_status_detail.c_str());
        }
        RenderMask(dc, dib, bits, w, h, line, font, kPadX + 20, dy + 1, core);
        shadow = core; Blur(shadow, w, h, kShadow); ComposeShadow(rgba, w, h, shadow, 1.0f);
        glow = core;   Blur(glow, w, h, kGlow);
        Compose(rgba, w, h, core, glow, g_connected ? Rgb{150, 235, 170} : Rgb{235, 150, 150}, 1.0f);

        // --- energy meter (Longest Night, while bleeding) --------------------
        // The game's HUD bar will not wake for our drain, so the cost is shown
        // here: a label, ten cells, a percentage. Off-white while you have
        // plenty, amber under half, red under a quarter.
        const int e = g_energy.load();
        if (e >= 0) {
            const int my = dy + 30;
            const Rgb ec = (e < 25) ? Rgb{235, 90, 90} : (e < 50 ? Rgb{255, 189, 76} : Rgb{226, 222, 210});
            char lab[64]; sprintf_s(lab, "ENERGY  %3d%%", e);
            RenderMask(dc, dib, bits, w, h, lab, font, kPadX, my, core);
            shadow = core; Blur(shadow, w, h, kShadow); ComposeShadow(rgba, w, h, shadow, 1.0f);
            glow = core;   Blur(glow, w, h, kGlow);
            Compose(rgba, w, h, core, glow, ec, 1.0f);
            // ten cells, drawn straight into the buffer
            const int cx0 = kPadX + 150, cw = 22, cg = 4, ch = 12, cy0 = my + 7;
            const int lit = (e + 9) / 10;
            for (int c = 0; c < 10; ++c) {
                const bool on = c < lit;
                for (int yy = cy0; yy < cy0 + ch && yy < h; ++yy) {
                    for (int xx = cx0 + c * (cw + cg); xx < cx0 + c * (cw + cg) + cw && xx < w; ++xx) {
                        unsigned char* q = &rgba[(static_cast<size_t>(yy) * w + xx) * 4];
                        const float a = on ? 0.95f : 0.22f;
                        q[0] = static_cast<unsigned char>(std::min(255, q[0] + int(ec.b * a)));
                        q[1] = static_cast<unsigned char>(std::min(255, q[1] + int(ec.g * a)));
                        q[2] = static_cast<unsigned char>(std::min(255, q[2] + int(ec.r * a)));
                        q[3] = static_cast<unsigned char>(std::min(255, q[3] + int(255 * a)));
                    }
                }
            }
        }

    }

    memcpy(bits, rgba.data(), rgba.size());

    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    POINT pos{wr.left, wr.top};
    SIZE  size{w, h};
    POINT src{0, 0};
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, screen, &pos, &size, dc, &src, 0, &bf, ULW_ALPHA);

    SelectObject(dc, oldbmp);
    DeleteObject(dib);
    if (font) DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void Reposition() {
    // Lower left. The game puts its own collectible pop-up in the top-left
    // corner, so the feed sits well below it; the right-hand side was tried
    // first and read badly against the rest of the HUD.
    RECT r{};
    HWND game = GameWindow();
    int x = 40, y = 120;
    if (game && GetWindowRect(game, &r) && (r.right - r.left) > 320) {
        x = r.left + g_off_x;
        y = r.bottom - Height() - g_off_y;
        if (y < r.top) y = r.top;            // window shorter than the feed
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, kWidth, Height(), SWP_NOACTIVATE);
}


LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:   Reposition(); Redraw(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;

        case kMsgRedraw:
            Redraw();
            return 0;

        default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

DWORD WINAPI ThreadMain(LPVOID) {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = Proc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "SM2ApOverlay";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "SM2ApOverlay", "SM2 Archipelago", WS_POPUP,
        40, 120, kWidth, Height(), nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        WARN("overlay: CreateWindowEx failed (%lu)", GetLastError());
        return 0;
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    Reposition();
    Redraw();
    // 10 Hz: fast enough for a smooth fade, far too slow to matter for perf.
    SetTimer(g_hwnd, 1, 100, nullptr);
    INFO("overlay window created (%p)", g_hwnd);

    MSG msg;
    while (g_run && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    KillTimer(g_hwnd, 1);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return 0;
}

}  // namespace

bool Running() { return g_run && g_hwnd != nullptr; }

bool Start() {
    if (g_run) return true;
    g_run = true;
    g_thread = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
    if (!g_thread) { g_run = false; return false; }
    for (int i = 0; i < 40 && !g_hwnd; ++i) Sleep(25);
    return g_hwnd != nullptr;
}

void Stop() {
    if (!g_run) return;
    g_run = false;
    if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = nullptr; }
}


void SetOffsets(int x, int y_from_bottom) {
    g_off_x = x;
    g_off_y = y_from_bottom;
    if (g_hwnd) Reposition();
}

void SetEnergy(int percent) {
    const int before = g_energy.exchange(percent < 0 ? -1 : (percent > 100 ? 100 : percent));
    if (!g_hwnd) return;
    if ((before < 0) != (percent < 0)) Reposition();   // the meter adds a row
    Redraw();
}

void SetConnected(bool on, const char* detail) {
    {
        // Written here on the poll thread, read in Redraw on the overlay
        // thread: a std::string needs the lock, a bool does not.
        std::lock_guard<std::mutex> g(g_lock);
        g_connected = on;
        g_status_detail = detail ? detail : (on ? "connected" : "not connected");
    }
    if (g_hwnd) Redraw();
}

// Lines longer than the feed is wide were being clipped mid-word ("it is best
// not to wast"). Wrap at a space instead; the continuation carries the same
// kind so it keeps the colour, and an indent so it reads as one message.
constexpr size_t kWrapAt = 60;   // 60 + the 7-char label still clears the edge (clipping began near 72)

void PushOne(int kind, const std::string& text) {
    g_lines.push_back({text, kind, GetTickCount()});
}

void Push(int kind, const char* text) {
    if (!text) return;
    std::lock_guard<std::mutex> g(g_lock);
    std::string rest = text;
    while (rest.size() > kWrapAt) {
        size_t cut = rest.rfind(' ', kWrapAt);
        if (cut == std::string::npos || cut < kWrapAt / 2) cut = kWrapAt;
        PushOne(kind, rest.substr(0, cut));
        rest = "  " + rest.substr(cut + (rest[cut] == ' ' ? 1 : 0));
    }
    PushOne(kind, rest);
    const DWORD now = GetTickCount();
    g_lines.erase(std::remove_if(g_lines.begin(), g_lines.end(),
                                 [now](const Line& l) { return now - l.born > kHoldMs + kFadeMs; }),
                  g_lines.end());
    while (static_cast<int>(g_lines.size()) > kMaxLines) g_lines.erase(g_lines.begin());
}

}  // namespace overlay
