#include "ui/dpi_panel.h"

#include "ui/theme.h"

#include <windowsx.h>

#include <algorithm>

namespace ui {
namespace {

struct SliderState {
    int min_value = 0;
    int max_value = 100;
    int step = 1;
    int value = 0;
    bool dragging = false;
    bool focused = false;
    const Theme* theme = nullptr;
};

SliderState* state_of(HWND hwnd) {
    return reinterpret_cast<SliderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int thumb_radius(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return std::min(9L, std::max(6L, (rc.bottom - rc.top) / 3));
}

int snap(const SliderState& s, int v) {
    v = std::clamp(v, s.min_value, s.max_value);
    if (s.step <= 1) return v;
    const int steps = (v - s.min_value + s.step / 2) / s.step;
    return std::min(s.min_value + steps * s.step, s.max_value);
}

int value_to_x(HWND hwnd, const SliderState& s) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int r = thumb_radius(hwnd);
    const int span = std::max(1, static_cast<int>(rc.right) - 2 * r);
    const int range = std::max(1, s.max_value - s.min_value);
    return r + MulDiv(s.value - s.min_value, span, range);
}

int x_to_value(HWND hwnd, const SliderState& s, int x) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int r = thumb_radius(hwnd);
    const int span = std::max(1, static_cast<int>(rc.right) - 2 * r);
    const int range = s.max_value - s.min_value;
    const int raw = s.min_value + MulDiv(std::clamp(x - r, 0, span), range, span);
    return snap(s, raw);
}

void notify_parent(HWND hwnd, const SliderState& s, bool final_value) {
    HWND parent = GetParent(hwnd);
    if (!parent) return;
    const auto id = static_cast<WORD>(GetWindowLongPtrW(hwnd, GWLP_ID));
    SendMessageW(parent, WM_SLIDER_CHANGED, MAKEWPARAM(id, final_value ? 1 : 0), s.value);
}

void paint(HWND hwnd, const SliderState& s) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);

    // Doppelpufferung: ohne die flackert der Griff beim Ziehen sichtbar.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    const Palette pal = s.theme ? s.theme->colors()
                                : Palette{RGB(243, 243, 243), RGB(255, 255, 255), RGB(0, 0, 0),
                                          RGB(100, 100, 100), RGB(0, 95, 184), RGB(200, 200, 200),
                                          RGB(200, 200, 200), RGB(150, 150, 150)};
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const COLORREF accent = enabled ? pal.accent : pal.disabled;

    HBRUSH bg = CreateSolidBrush(pal.bg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    const int r = thumb_radius(hwnd);
    const int cy = rc.bottom / 2;
    const int track_h = std::max(3, r / 3);
    const int thumb_x = value_to_x(hwnd, s);

    // Rinne rechts vom Griff
    RECT track{r, cy - track_h / 2, rc.right - r, cy - track_h / 2 + track_h};
    HBRUSH tb = CreateSolidBrush(pal.track);
    FillRect(mem, &track, tb);
    DeleteObject(tb);

    // gefuellter Teil links vom Griff
    RECT filled{r, track.top, thumb_x, track.bottom};
    HBRUSH fb = CreateSolidBrush(accent);
    FillRect(mem, &filled, fb);
    DeleteObject(fb);

    // Griff
    HBRUSH thumb_brush = CreateSolidBrush(accent);
    HPEN thumb_pen = CreatePen(PS_SOLID, 1, pal.bg);
    HGDIOBJ ob = SelectObject(mem, thumb_brush);
    HGDIOBJ op = SelectObject(mem, thumb_pen);
    Ellipse(mem, thumb_x - r, cy - r, thumb_x + r, cy + r);
    SelectObject(mem, ob);
    SelectObject(mem, op);
    DeleteObject(thumb_brush);
    DeleteObject(thumb_pen);

    if (s.focused) {
        RECT focus{thumb_x - r - 2, cy - r - 2, thumb_x + r + 2, cy + r + 2};
        SetTextColor(mem, pal.text);
        SetBkColor(mem, pal.bg);
        DrawFocusRect(mem, &focus);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK slider_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SliderState* s = state_of(hwnd);

    switch (msg) {
        case WM_NCCREATE: {
            auto* fresh = new SliderState();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(fresh));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_NCDESTROY:
            delete s;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, msg, wp, lp);

        case WM_ERASEBKGND:
            return 1;   // alles laeuft ueber WM_PAINT in den Puffer

        case WM_PAINT:
            if (s) paint(hwnd, *s);
            return 0;

        case SLM_SETRANGE:
            if (s) {
                s->min_value = static_cast<int>(wp);
                s->max_value = std::max(static_cast<int>(lp), s->min_value + 1);
                s->value = snap(*s, s->value);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case SLM_SETSTEP:
            if (s) { s->step = std::max(1, static_cast<int>(wp)); s->value = snap(*s, s->value); }
            return 0;

        case SLM_SETVALUE:
            if (s) {
                const int v = snap(*s, static_cast<int>(wp));
                if (v != s->value) { s->value = v; InvalidateRect(hwnd, nullptr, FALSE); }
            }
            return 0;

        case SLM_GETVALUE:
            return s ? s->value : 0;

        case SLM_SETTHEME:
            if (s) { s->theme = reinterpret_cast<const Theme*>(lp); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;

        case WM_LBUTTONDOWN:
            if (s && IsWindowEnabled(hwnd)) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                s->dragging = true;
                const int v = x_to_value(hwnd, *s, GET_X_LPARAM(lp));
                if (v != s->value) { s->value = v; InvalidateRect(hwnd, nullptr, FALSE); }
                notify_parent(hwnd, *s, false);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (s && s->dragging) {
                const int v = x_to_value(hwnd, *s, GET_X_LPARAM(lp));
                if (v != s->value) {
                    s->value = v;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    notify_parent(hwnd, *s, false);
                }
            }
            return 0;

        case WM_LBUTTONUP:
            if (s && s->dragging) {
                s->dragging = false;
                ReleaseCapture();
                notify_parent(hwnd, *s, true);
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (s) s->dragging = false;
            return 0;

        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;

        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            if (s) { s->focused = (msg == WM_SETFOCUS); InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;

        case WM_KEYDOWN: {
            if (!s || !IsWindowEnabled(hwnd)) return 0;
            int v = s->value;
            switch (wp) {
                case VK_LEFT:  case VK_DOWN: v -= s->step; break;
                case VK_RIGHT: case VK_UP:   v += s->step; break;
                case VK_PRIOR: v += s->step * 10; break;
                case VK_NEXT:  v -= s->step * 10; break;
                case VK_HOME:  v = s->min_value; break;
                case VK_END:   v = s->max_value; break;
                default: return DefWindowProcW(hwnd, msg, wp, lp);
            }
            v = snap(*s, v);
            if (v != s->value) {
                s->value = v;
                InvalidateRect(hwnd, nullptr, FALSE);
                notify_parent(hwnd, *s, true);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!s || !IsWindowEnabled(hwnd)) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? s->step : -s->step;
            const int v = snap(*s, s->value + delta);
            if (v != s->value) {
                s->value = v;
                InvalidateRect(hwnd, nullptr, FALSE);
                notify_parent(hwnd, *s, true);
            }
            return 0;
        }

        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool register_slider_class(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = slider_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kSliderClass;
    return RegisterClassExW(&wc) != 0;
}

} // namespace ui
