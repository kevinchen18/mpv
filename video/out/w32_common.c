/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <limits.h>
#include <pthread.h>
#include <assert.h>
#include <windows.h>
#include <windowsx.h>
#include <ole2.h>
#include <shobjidl.h>
#include <avrt.h>

#include "options/options.h"
#include "input/keycodes.h"
#include "input/input.h"
#include "input/event.h"
#include "stream/stream.h"
#include "common/msg.h"
#include "common/common.h"
#include "vo.h"
#include "win_state.h"
#include "w32_common.h"
#include "win32/displayconfig.h"
#include "osdep/io.h"
#include "osdep/threads.h"
#include "osdep/w32_keyboard.h"
#include "osdep/atomic.h"
#include "misc/dispatch.h"
#include "misc/rendezvous.h"
#include "mpv_talloc.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)

static __thread struct vo_w32_state *w32_thread_context;

struct vo_w32_state {
    struct mp_log *log;
    struct vo *vo;
    struct mp_vo_opts *opts;
    struct input_ctx *input_ctx;

    pthread_t thread;
    bool terminate;
    struct mp_dispatch_queue *dispatch; // used to run stuff on the GUI thread

    HWND window;
    HWND parent; // 0 normally, set in embedding mode
    HHOOK parent_win_hook;
    HWINEVENTHOOK parent_evt_hook;

    HMONITOR monitor; // Handle of the current screen
    struct mp_rect screenrc; // Size and virtual position of the current screen
    char *color_profile; // Path of the current screen's color profile

    // last non-fullscreen extends (updated only on fullscreen or on initialization)
    int prev_width;
    int prev_height;
    int prev_x;
    int prev_y;

    // Has the window seen a WM_DESTROY? If so, don't call DestroyWindow again.
    bool destroyed;

    // whether the window position and size were intialized
    bool window_bounds_initialized;

    bool current_fs;

    // currently known window state
    int window_x;
    int window_y;
    int dw;
    int dh;

    // video size
    uint32_t o_dwidth;
    uint32_t o_dheight;

    bool disable_screensaver;
    bool cursor_visible;
    atomic_uint event_flags;

    BOOL tracking;
    TRACKMOUSEEVENT trackEvent;

    int mouse_x;
    int mouse_y;

    // Should SetCursor be called when handling VOCTRL_SET_CURSOR_VISIBILITY?
    bool can_set_cursor;

    // UTF-16 decoding state for WM_CHAR and VK_PACKET
    int high_surrogate;

    ITaskbarList2 *taskbar_list;
    ITaskbarList3 *taskbar_list3;
    UINT tbtnCreatedMsg;
    bool tbtnCreated;

    struct voctrl_playback_state current_pstate;

    // updates on move/resize/displaychange
    double display_fps;

    HANDLE avrt_handle;
};

typedef struct tagDropTarget {
    IDropTarget iface;
    atomic_int refCnt;
    DWORD lastEffect;
    IDataObject* dataObj;
    struct vo_w32_state *w32;
} DropTarget;

static FORMATETC fmtetc_file = { CF_HDROP, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
static FORMATETC fmtetc_url = { 0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

static void DropTarget_Destroy(DropTarget* This)
{
    if (This->dataObj != NULL) {
        This->dataObj->lpVtbl->Release(This->dataObj);
        This->dataObj->lpVtbl = NULL;
    }

    talloc_free(This);
}

static HRESULT STDMETHODCALLTYPE DropTarget_QueryInterface(IDropTarget* This,
                                                           REFIID riid,
                                                           void** ppvObject)
{
    if (!IsEqualGUID(riid, &IID_IUnknown) ||
        !IsEqualGUID(riid, &IID_IDataObject)) {
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    *ppvObject = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE DropTarget_AddRef(IDropTarget* This)
{
    DropTarget* t = (DropTarget*)This;
    return atomic_fetch_add(&t->refCnt, 1) + 1;
}

static ULONG STDMETHODCALLTYPE DropTarget_Release(IDropTarget* This)
{
    DropTarget* t = (DropTarget*)This;
    ULONG cRef = atomic_fetch_add(&t->refCnt, -1) - 1;

    if (cRef == 0) {
        DropTarget_Destroy(t);
    }

    return cRef;
}

static HRESULT STDMETHODCALLTYPE DropTarget_DragEnter(IDropTarget* This,
                                                      IDataObject* pDataObj,
                                                      DWORD grfKeyState,
                                                      POINTL pt,
                                                      DWORD* pdwEffect)
{
    DropTarget* t = (DropTarget*)This;

    pDataObj->lpVtbl->AddRef(pDataObj);
    if (pDataObj->lpVtbl->QueryGetData(pDataObj, &fmtetc_file) != S_OK &&
        pDataObj->lpVtbl->QueryGetData(pDataObj, &fmtetc_url) != S_OK) {

        *pdwEffect = DROPEFFECT_NONE;
    }

    if (t->dataObj != NULL) {
        t->dataObj->lpVtbl->Release(t->dataObj);
    }

    t->dataObj = pDataObj;
    t->lastEffect = *pdwEffect;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DropTarget_DragOver(IDropTarget* This,
                                                     DWORD grfKeyState,
                                                     POINTL pt,
                                                     DWORD* pdwEffect)
{
    DropTarget* t = (DropTarget*)This;

    *pdwEffect = t->lastEffect;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DropTarget_DragLeave(IDropTarget* This)
{
    DropTarget* t = (DropTarget*)This;

    if (t->dataObj != NULL) {
        t->dataObj->lpVtbl->Release(t->dataObj);
        t->dataObj = NULL;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DropTarget_Drop(IDropTarget* This,
                                                 IDataObject* pDataObj,
                                                 DWORD grfKeyState, POINTL pt,
                                                 DWORD* pdwEffect)
{
    DropTarget* t = (DropTarget*)This;

    STGMEDIUM medium;

    if (t->dataObj != NULL) {
        t->dataObj->lpVtbl->Release(t->dataObj);
        t->dataObj = NULL;
    }

    enum mp_dnd_action action = (grfKeyState & MK_SHIFT) ? DND_APPEND : DND_REPLACE;

    pDataObj->lpVtbl->AddRef(pDataObj);

    if (pDataObj->lpVtbl->GetData(pDataObj, &fmtetc_file, &medium) == S_OK) {
        if (GlobalLock(medium.hGlobal) != NULL) {
            HDROP hDrop = (HDROP)medium.hGlobal;

            UINT numFiles = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            char** files = talloc_zero_array(NULL, char*, numFiles);

            UINT nrecvd_files = 0;
            for (UINT i = 0; i < numFiles; i++) {
                UINT len = DragQueryFileW(hDrop, i, NULL, 0);
                wchar_t* buf = talloc_array(NULL, wchar_t, len + 1);

                if (DragQueryFileW(hDrop, i, buf, len + 1) == len) {
                    char* fname = mp_to_utf8(files, buf);
                    files[nrecvd_files++] = fname;

                    MP_VERBOSE(t->w32, "received dropped file: %s\n",
                               fname);
                } else {
                    MP_ERR(t->w32, "error getting dropped file name\n");
                }

                talloc_free(buf);
            }

            GlobalUnlock(medium.hGlobal);
            mp_event_drop_files(t->w32->input_ctx, nrecvd_files, files,
                                action);

            talloc_free(files);
        }

        ReleaseStgMedium(&medium);
    } else if (pDataObj->lpVtbl->GetData(pDataObj,
                                         &fmtetc_url, &medium) == S_OK) {
        // get the URL encoded in US-ASCII
        wchar_t* wurl = GlobalLock(medium.hGlobal);
        if (wurl != NULL) {
            char *url = mp_to_utf8(NULL, wurl);
            if (mp_event_drop_mime_data(t->w32->input_ctx, "text/uri-list",
                                        bstr0(url), action) > 0) {
                MP_VERBOSE(t->w32, "received dropped URL: %s\n", url);
            } else {
                MP_ERR(t->w32, "error getting dropped URL\n");
            }

            talloc_free(url);
            GlobalUnlock(medium.hGlobal);
        }

        ReleaseStgMedium(&medium);
    }
    else {
        t->lastEffect = DROPEFFECT_NONE;
    }

    pDataObj->lpVtbl->Release(pDataObj);
    *pdwEffect = t->lastEffect;
    return S_OK;
}


static void DropTarget_Init(DropTarget* This, struct vo_w32_state *w32)
{
    IDropTargetVtbl* vtbl = talloc(This, IDropTargetVtbl);
    *vtbl = (IDropTargetVtbl){
        DropTarget_QueryInterface, DropTarget_AddRef, DropTarget_Release,
        DropTarget_DragEnter, DropTarget_DragOver, DropTarget_DragLeave,
        DropTarget_Drop
    };

    This->iface.lpVtbl = vtbl;
    atomic_store(&This->refCnt, 0);
    This->lastEffect = 0;
    This->dataObj = NULL;
    This->w32 = w32;
}

static void add_window_borders(HWND hwnd, RECT *rc)
{
    AdjustWindowRect(rc, GetWindowLong(hwnd, GWL_STYLE), 0);
}

// basically a reverse AdjustWindowRect (win32 doesn't appear to have this)
static void subtract_window_borders(HWND hwnd, RECT *rc)
{
    RECT b = { 0, 0, 0, 0 };
    add_window_borders(hwnd, &b);
    rc->left -= b.left;
    rc->top -= b.top;
    rc->right -= b.right;
    rc->bottom -= b.bottom;
}

static LRESULT borderless_nchittest(struct vo_w32_state *w32, int x, int y)
{
    if (IsMaximized(w32->window))
        return HTCLIENT;

    POINT mouse = { x, y };
    ScreenToClient(w32->window, &mouse);

    // The horizontal frame should be the same size as the vertical frame,
    // since the NONCLIENTMETRICS structure does not distinguish between them
    int frame_size = GetSystemMetrics(SM_CXFRAME) +
                     GetSystemMetrics(SM_CXPADDEDBORDER);
    // The diagonal size handles are slightly wider than the side borders
    int diagonal_width = frame_size * 2 + GetSystemMetrics(SM_CXBORDER);

    // Hit-test top border
    if (mouse.y < frame_size) {
        if (mouse.x < diagonal_width)
            return HTTOPLEFT;
        if (mouse.x >= w32->dw - diagonal_width)
            return HTTOPRIGHT;
        return HTTOP;
    }

    // Hit-test bottom border
    if (mouse.y >= w32->dh - frame_size) {
        if (mouse.x < diagonal_width)
            return HTBOTTOMLEFT;
        if (mouse.x >= w32->dw - diagonal_width)
            return HTBOTTOMRIGHT;
        return HTBOTTOM;
    }

    // Hit-test side borders
    if (mouse.x < frame_size)
        return HTLEFT;
    if (mouse.x >= w32->dw - frame_size)
        return HTRIGHT;
    return HTCLIENT;
}

// turn a WMSZ_* input value in v into the border that should be resized
// returns: 0=left, 1=top, 2=right, 3=bottom, -1=undefined
static int get_resize_border(int v)
{
    switch (v) {
    case WMSZ_LEFT: return 3;
    case WMSZ_TOP: return 2;
    case WMSZ_RIGHT: return 3;
    case WMSZ_BOTTOM: return 2;
    case WMSZ_TOPLEFT: return 1;
    case WMSZ_TOPRIGHT: return 1;
    case WMSZ_BOTTOMLEFT: return 3;
    case WMSZ_BOTTOMRIGHT: return 3;
    default: return -1;
    }
}

static bool key_state(int vk)
{
    return GetKeyState(vk) & 0x8000;
}

static int mod_state(struct vo_w32_state *w32)
{
    int res = 0;

    // AltGr is represented as LCONTROL+RMENU on Windows
    bool alt_gr = mp_input_use_alt_gr(w32->input_ctx) &&
        key_state(VK_RMENU) && key_state(VK_LCONTROL);

    if (key_state(VK_RCONTROL) || (key_state(VK_LCONTROL) && !alt_gr))
        res |= MP_KEY_MODIFIER_CTRL;
    if (key_state(VK_SHIFT))
        res |= MP_KEY_MODIFIER_SHIFT;
    if (key_state(VK_LMENU) || (key_state(VK_RMENU) && !alt_gr))
        res |= MP_KEY_MODIFIER_ALT;
    return res;
}

static int decode_surrogate_pair(wchar_t lead, wchar_t trail)
{
    return 0x10000 + (((lead & 0x3ff) << 10) | (trail & 0x3ff));
}

static int decode_utf16(struct vo_w32_state *w32, wchar_t c)
{
    // Decode UTF-16, keeping state in w32->high_surrogate
    if (IS_HIGH_SURROGATE(c)) {
        w32->high_surrogate = c;
        return 0;
    }
    if (IS_LOW_SURROGATE(c)) {
        if (!w32->high_surrogate) {
            MP_ERR(w32, "Invalid UTF-16 input\n");
            return 0;
        }
        int codepoint = decode_surrogate_pair(w32->high_surrogate, c);
        w32->high_surrogate = 0;
        return codepoint;
    }
    if (w32->high_surrogate != 0) {
        w32->high_surrogate = 0;
        MP_ERR(w32, "Invalid UTF-16 input\n");
        return 0;
    }

    return c;
}

static void clear_keyboard_buffer(void)
{
    static const UINT vkey = VK_DECIMAL;
    static const BYTE keys[256] = { 0 };
    UINT scancode = MapVirtualKey(vkey, MAPVK_VK_TO_VSC);
    wchar_t buf[10];
    int ret = 0;

    // Use the method suggested by Michael Kaplan to clear any pending dead
    // keys from the current keyboard layout. See:
    // https://web.archive.org/web/20101004154432/http://blogs.msdn.com/b/michkap/archive/2006/04/06/569632.aspx
    // https://web.archive.org/web/20100820152419/http://blogs.msdn.com/b/michkap/archive/2007/10/27/5717859.aspx
    do {
        ret = ToUnicode(vkey, scancode, keys, buf, MP_ARRAY_SIZE(buf), 0);
    } while (ret < 0);
}

static int to_unicode(UINT vkey, UINT scancode, const BYTE keys[256])
{
    // This wraps ToUnicode to be stateless and to return only one character

    // Make the buffer 10 code units long to be safe, same as here:
    // https://web.archive.org/web/20101013215215/http://blogs.msdn.com/b/michkap/archive/2006/03/24/559169.aspx
    wchar_t buf[10] = { 0 };

    // Dead keys aren't useful for key shortcuts, so clear the keyboard state
    clear_keyboard_buffer();

    int len = ToUnicode(vkey, scancode, keys, buf, MP_ARRAY_SIZE(buf), 0);

    // Return the last complete UTF-16 code point. A negative return value
    // indicates a dead key, however there should still be a non-combining
    // version of the key in the buffer.
    if (len < 0)
        len = -len;
    if (len >= 2 && IS_SURROGATE_PAIR(buf[len - 2], buf[len - 1]))
        return decode_surrogate_pair(buf[len - 2], buf[len - 1]);
    if (len >= 1)
        return buf[len - 1];

    return 0;
}

static int decode_key(struct vo_w32_state *w32, UINT vkey, UINT scancode)
{
    BYTE keys[256];
    GetKeyboardState(keys);

    // If mp_input_use_alt_gr is false, detect and remove AltGr so normal
    // characters are generated. Note that AltGr is represented as
    // LCONTROL+RMENU on Windows.
    if ((keys[VK_RMENU] & 0x80) && (keys[VK_LCONTROL] & 0x80) &&
        !mp_input_use_alt_gr(w32->input_ctx))
    {
        keys[VK_RMENU] = keys[VK_LCONTROL] = 0;
        keys[VK_MENU] = keys[VK_LMENU];
        keys[VK_CONTROL] = keys[VK_RCONTROL];
    }

    int c = to_unicode(vkey, scancode, keys);

    // Some shift states prevent ToUnicode from working or cause it to produce
    // control characters. If this is detected, remove modifiers until it
    // starts producing normal characters.
    if (c < 0x20 && (keys[VK_MENU] & 0x80)) {
        keys[VK_LMENU] = keys[VK_RMENU] = keys[VK_MENU] = 0;
        c = to_unicode(vkey, scancode, keys);
    }
    if (c < 0x20 && (keys[VK_CONTROL] & 0x80)) {
        keys[VK_LCONTROL] = keys[VK_RCONTROL] = keys[VK_CONTROL] = 0;
        c = to_unicode(vkey, scancode, keys);
    }
    if (c < 0x20)
        return 0;

    // Decode lone UTF-16 surrogates (VK_PACKET can generate these)
    if (c < 0x10000)
        return decode_utf16(w32, c);
    return c;
}

static void handle_key_down(struct vo_w32_state *w32, UINT vkey, UINT scancode)
{
    // Ignore key repeat
    if (scancode & KF_REPEAT)
        return;

    int mpkey = mp_w32_vkey_to_mpkey(vkey, scancode & KF_EXTENDED);
    if (!mpkey) {
        mpkey = decode_key(w32, vkey, scancode & (0xff | KF_EXTENDED));
        if (!mpkey)
            return;
    }

    mp_input_put_key(w32->input_ctx, mpkey | mod_state(w32) | MP_KEY_STATE_DOWN);
}

static void handle_key_up(struct vo_w32_state *w32, UINT vkey, UINT scancode)
{
    switch (vkey) {
    case VK_MENU:
    case VK_CONTROL:
    case VK_SHIFT:
        break;
    default:
        // Releasing all keys on key-up is simpler and ensures no keys can be
        // get "stuck." This matches the behaviour of other VOs.
        mp_input_put_key(w32->input_ctx, MP_INPUT_RELEASE_ALL);
    }
}

static bool handle_char(struct vo_w32_state *w32, wchar_t wc)
{
    int c = decode_utf16(w32, wc);

    if (c == 0)
        return true;
    if (c < 0x20)
        return false;

    mp_input_put_key(w32->input_ctx, c | mod_state(w32));
    return true;
}

static void signal_events(struct vo_w32_state *w32, int events)
{
    atomic_fetch_or(&w32->event_flags, events);
    vo_wakeup(w32->vo);
}

static void wakeup_gui_thread(void *ctx)
{
    struct vo_w32_state *w32 = ctx;
    PostMessage(w32->window, WM_USER, 0, 0);
}

static double get_refresh_rate_from_gdi(const wchar_t *device)
{
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &dm))
        return 0.0;

    // May return 0 or 1 which "represent the display hardware's default refresh rate"
    // https://msdn.microsoft.com/en-us/library/windows/desktop/dd183565%28v=vs.85%29.aspx
    // mpv validates this value with a threshold of 1, so don't return exactly 1
    if (dm.dmDisplayFrequency == 1)
        return 0.0;

    // dm.dmDisplayFrequency is an integer which is rounded down, so it's
    // highly likely that 23 represents 24/1.001, 59 represents 60/1.001, etc.
    // A caller can always reproduce the original value by using floor.
    double rv = dm.dmDisplayFrequency;
    switch (dm.dmDisplayFrequency) {
        case  23:
        case  29:
        case  47:
        case  59:
        case  71:
        case  89:
        case  95:
        case 119:
        case 143:
            rv = (rv + 1) / 1.001;
    }

    return rv;
}

static char *get_color_profile(void *ctx, const wchar_t *device)
{
    char *name = NULL;

    HDC ic = CreateICW(device, NULL, NULL, NULL);
    if (!ic)
        goto done;
    wchar_t wname[MAX_PATH + 1];
    if (!GetICMProfileW(ic, &(DWORD){ MAX_PATH }, wname))
        goto done;

    name = mp_to_utf8(ctx, wname);
done:
    if (ic)
        DeleteDC(ic);
    return name;
}

static void update_display_info(struct vo_w32_state *w32)
{
    HMONITOR monitor = MonitorFromWindow(w32->window, MONITOR_DEFAULTTOPRIMARY);
    if (w32->monitor == monitor)
        return;
    w32->monitor = monitor;

    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(monitor, (MONITORINFO*)&mi);

    // Try to get the monitor refresh rate.
    double freq = 0.0;

    if (freq == 0.0)
        freq = mp_w32_displayconfig_get_refresh_rate(mi.szDevice);
    if (freq == 0.0)
        freq = get_refresh_rate_from_gdi(mi.szDevice);

    if (freq != w32->display_fps) {
        MP_VERBOSE(w32, "display-fps: %f\n", freq);
        if (freq == 0.0)
            MP_WARN(w32, "Couldn't determine monitor refresh rate\n");
        w32->display_fps = freq;
        signal_events(w32, VO_EVENT_WIN_STATE);
    }

    char *color_profile = get_color_profile(w32, mi.szDevice);
    if ((color_profile == NULL) != (w32->color_profile == NULL) ||
        (color_profile && strcmp(color_profile, w32->color_profile)))
    {
        if (color_profile)
            MP_VERBOSE(w32, "color-profile: %s\n", color_profile);
        talloc_free(w32->color_profile);
        w32->color_profile = color_profile;
        color_profile = NULL;
        signal_events(w32, VO_EVENT_ICC_PROFILE_CHANGED);
    }

    talloc_free(color_profile);
}

static void force_update_display_info(struct vo_w32_state *w32)
{
    w32->monitor = 0;
    update_display_info(w32);
}

static void update_playback_state(struct vo_w32_state *w32)
{
    struct voctrl_playback_state *pstate = &w32->current_pstate;

    if (!w32->taskbar_list3 || !w32->tbtnCreated)
        return;

    if (!pstate->playing || !pstate->taskbar_progress) {
        ITaskbarList3_SetProgressState(w32->taskbar_list3, w32->window,
                                       TBPF_NOPROGRESS);
        return;
    }

    ITaskbarList3_SetProgressValue(w32->taskbar_list3, w32->window,
                                   pstate->percent_pos, 100);
    ITaskbarList3_SetProgressState(w32->taskbar_list3, w32->window,
                                   pstate->paused ? TBPF_PAUSED :
                                                    TBPF_NORMAL);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    assert(w32_thread_context);
    struct vo_w32_state *w32 = w32_thread_context;
    if (!w32->window)
        w32->window = hWnd; // can happen during CreateWindow*!
    assert(w32->window == hWnd);
    int mouse_button = 0;

    switch (message) {
    case WM_USER:
        // This message is used to wakeup the GUI thread, see wakeup_gui_thread.
        mp_dispatch_queue_process(w32->dispatch, 0);
        break;
    case WM_ERASEBKGND: // no need to erase background separately
        return 1;
    case WM_PAINT:
        signal_events(w32, VO_EVENT_EXPOSE);
        break;
    case WM_MOVE: {
        POINT p = {0};
        ClientToScreen(w32->window, &p);
        w32->window_x = p.x;
        w32->window_y = p.y;

        // Window may intersect with new monitors (see VOCTRL_GET_DISPLAY_NAMES)
        signal_events(w32, VO_EVENT_WIN_STATE);

        update_display_info(w32);  // if we moved between monitors
        MP_VERBOSE(w32, "move window: %d:%d\n", w32->window_x, w32->window_y);
        break;
    }
    case WM_SIZE: {
        RECT r;
        if (GetClientRect(w32->window, &r) && r.right > 0 && r.bottom > 0) {
            w32->dw = r.right;
            w32->dh = r.bottom;
            signal_events(w32, VO_EVENT_RESIZE);
            MP_VERBOSE(w32, "resize window: %d:%d\n", w32->dw, w32->dh);
        }

        // Window may have been minimized or restored
        signal_events(w32, VO_EVENT_WIN_STATE);

        update_display_info(w32);
        break;
    }
    case WM_SIZING:
        if (w32->opts->keepaspect && w32->opts->keepaspect_window &&
            !w32->current_fs && !w32->parent)
        {
            RECT *rc = (RECT*)lParam;
            // get client area of the windows if it had the rect rc
            // (subtracting the window borders)
            RECT r = *rc;
            subtract_window_borders(w32->window, &r);
            int c_w = r.right - r.left, c_h = r.bottom - r.top;
            float aspect = w32->o_dwidth / (float) MPMAX(w32->o_dheight, 1);
            int d_w = c_h * aspect - c_w;
            int d_h = c_w / aspect - c_h;
            int d_corners[4] = { d_w, d_h, -d_w, -d_h };
            int corners[4] = { rc->left, rc->top, rc->right, rc->bottom };
            int corner = get_resize_border(wParam);
            if (corner >= 0)
                corners[corner] -= d_corners[corner];
            *rc = (RECT) { corners[0], corners[1], corners[2], corners[3] };
            return TRUE;
        }
        break;
    case WM_CLOSE:
        // Don't actually allow it to destroy the window, or whatever else it
        // is that will make us lose WM_USER wakeups.
        mp_input_put_key(w32->input_ctx, MP_KEY_CLOSE_WIN);
        return 0;
    case WM_NCDESTROY: // Sometimes only WM_NCDESTROY is received in --wid mode
    case WM_DESTROY:
        if (w32->destroyed)
            break;
        // If terminate is not set, something else destroyed the window. This
        // can also happen in --wid mode when the parent window is destroyed.
        if (!w32->terminate)
            mp_input_put_key(w32->input_ctx, MP_KEY_CLOSE_WIN);
        RevokeDragDrop(w32->window);
        w32->destroyed = true;
        w32->window = NULL;
        PostQuitMessage(0);
        break;
    case WM_SYSCOMMAND:
        switch (wParam) {
        case SC_SCREENSAVE:
        case SC_MONITORPOWER:
            if (w32->disable_screensaver) {
                MP_VERBOSE(w32, "killing screensaver\n");
                return 0;
            }
            break;
        }
        break;
    case WM_NCHITTEST:
        // Provide sizing handles for borderless windows
        if (!w32->opts->border && !w32->current_fs) {
            return borderless_nchittest(w32, GET_X_LPARAM(lParam),
                                        GET_Y_LPARAM(lParam));
        }
        break;
    case WM_SYSKEYDOWN:
        // Open the window menu on Alt+Space. Normally DefWindowProc opens the
        // window menu in response to WM_SYSCHAR, but since mpv translates its
        // own keyboard input, WM_SYSCHAR isn't generated, so the window menu
        // must be opened manually.
        if (wParam == VK_SPACE) {
            SendMessage(w32->window, WM_SYSCOMMAND, SC_KEYMENU, ' ');
            return 0;
        }

        // Handle all other WM_SYSKEYDOWN messages as WM_KEYDOWN
    case WM_KEYDOWN:
        handle_key_down(w32, wParam, HIWORD(lParam));
        if (wParam == VK_F10)
            return 0;
        break;
    case WM_SYSKEYUP:
    case WM_KEYUP:
        handle_key_up(w32, wParam, HIWORD(lParam));
        if (wParam == VK_F10)
            return 0;
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (handle_char(w32, wParam))
            return 0;
        break;
    case WM_KILLFOCUS:
        mp_input_put_key(w32->input_ctx, MP_INPUT_RELEASE_ALL);
        break;
    case WM_SETCURSOR:
        // The cursor should only be hidden if the mouse is in the client area
        // and if the window isn't in menu mode (HIWORD(lParam) is non-zero)
        w32->can_set_cursor = LOWORD(lParam) == HTCLIENT && HIWORD(lParam);
        if (w32->can_set_cursor && !w32->cursor_visible) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        w32->tracking = FALSE;
        mp_input_put_key(w32->input_ctx, MP_KEY_MOUSE_LEAVE);
        break;
    case WM_MOUSEMOVE: {
        if (!w32->tracking) {
            w32->tracking = TrackMouseEvent(&w32->trackEvent);
            mp_input_put_key(w32->input_ctx, MP_KEY_MOUSE_ENTER);
        }
        // Windows can send spurious mouse events, which would make the mpv
        // core unhide the mouse cursor on completely unrelated events. See:
        //  https://blogs.msdn.com/b/oldnewthing/archive/2003/10/01/55108.aspx
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (x != w32->mouse_x || y != w32->mouse_y) {
            w32->mouse_x = x;
            w32->mouse_y = y;
            mp_input_set_mouse_pos(w32->input_ctx, x, y);
        }
        break;
    }
    case WM_LBUTTONDOWN:
        mouse_button = MP_MOUSE_BTN0 | MP_KEY_STATE_DOWN;
        break;
    case WM_LBUTTONUP:
        mouse_button = MP_MOUSE_BTN0 | MP_KEY_STATE_UP;
        break;
    case WM_MBUTTONDOWN:
        mouse_button = MP_MOUSE_BTN1 | MP_KEY_STATE_DOWN;
        break;
    case WM_MBUTTONUP:
        mouse_button = MP_MOUSE_BTN1 | MP_KEY_STATE_UP;
        break;
    case WM_RBUTTONDOWN:
        mouse_button = MP_MOUSE_BTN2 | MP_KEY_STATE_DOWN;
        break;
    case WM_RBUTTONUP:
        mouse_button = MP_MOUSE_BTN2 | MP_KEY_STATE_UP;
        break;
    case WM_MOUSEWHEEL: {
        int x = GET_WHEEL_DELTA_WPARAM(wParam);
        mouse_button = x > 0 ? MP_MOUSE_BTN3 : MP_MOUSE_BTN4;
        break;
    }
    case WM_XBUTTONDOWN:
        mouse_button = HIWORD(wParam) == 1 ? MP_MOUSE_BTN5 : MP_MOUSE_BTN6;
        mouse_button |= MP_KEY_STATE_DOWN;
        break;
    case WM_XBUTTONUP:
        mouse_button = HIWORD(wParam) == 1 ? MP_MOUSE_BTN5 : MP_MOUSE_BTN6;
        mouse_button |= MP_KEY_STATE_UP;
        break;
    case WM_DISPLAYCHANGE:
        force_update_display_info(w32);
        break;
    }

    if (message == w32->tbtnCreatedMsg) {
        w32->tbtnCreated = true;
        update_playback_state(w32);
        return 0;
    }

    if (mouse_button) {
        mouse_button |= mod_state(w32);
        mp_input_put_key(w32->input_ctx, mouse_button);

        if (mp_input_mouse_enabled(w32->input_ctx)) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);

            if (mouse_button == (MP_MOUSE_BTN0 | MP_KEY_STATE_DOWN) &&
                !w32->current_fs &&
                !mp_input_test_dragging(w32->input_ctx, x, y))
            {
                // Window dragging hack
                ReleaseCapture();
                SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                mp_input_put_key(w32->input_ctx, MP_MOUSE_BTN0 |
                                                 MP_KEY_STATE_UP);
                return 0;
            }
        }

        if (mouse_button & MP_KEY_STATE_DOWN)
            SetCapture(w32->window);
        else
            ReleaseCapture();
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

static pthread_once_t window_class_init_once = PTHREAD_ONCE_INIT;
static ATOM window_class;
static void register_window_class(void)
{
    window_class = RegisterClassExW(&(WNDCLASSEXW) {
        .cbSize = sizeof(WNDCLASSEXW),
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = WndProc,
        .hInstance = HINST_THISCOMPONENT,
        .hIcon = LoadIconW(HINST_THISCOMPONENT, L"IDI_ICON1"),
        .hCursor = LoadCursor(NULL, IDC_ARROW),
        .lpszClassName = L"mpv",
    });
}

static ATOM get_window_class(void)
{
    pthread_once(&window_class_init_once, register_window_class);
    return window_class;
}

static void resize_child_win(HWND parent)
{
    // Check if an mpv window is a child of this window. This will not
    // necessarily be the case because the hook functions will run for all
    // windows on the parent window's thread.
    ATOM cls = get_window_class();
    HWND child = FindWindowExW(parent, NULL, (LPWSTR)MAKEINTATOM(cls), NULL);
    if (!child)
        return;
    // Make sure the window was created by this instance
    if (GetWindowLongPtrW(child, GWLP_HINSTANCE) != (LONG_PTR)HINST_THISCOMPONENT)
        return;

    // Resize the mpv window to match its parent window's size
    RECT rm, rp;
    if (!GetClientRect(child, &rm))
        return;
    if (!GetClientRect(parent, &rp))
        return;
    if (EqualRect(&rm, &rp))
        return;
    SetWindowPos(child, NULL, 0, 0, rp.right, rp.bottom, SWP_ASYNCWINDOWPOS |
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

static LRESULT CALLBACK parent_win_hook(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode != HC_ACTION)
        goto done;
    CWPSTRUCT *cwp = (CWPSTRUCT*)lParam;
    if (cwp->message != WM_WINDOWPOSCHANGED)
        goto done;
    resize_child_win(cwp->hwnd);
done:
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static void CALLBACK parent_evt_hook(HWINEVENTHOOK hWinEventHook, DWORD event,
    HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread,
    DWORD dwmsEventTime)
{
    if (event != EVENT_OBJECT_LOCATIONCHANGE)
        return;
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    resize_child_win(hwnd);
}

static void install_parent_hook(struct vo_w32_state *w32)
{
    DWORD pid;
    DWORD tid = GetWindowThreadProcessId(w32->parent, &pid);

    // If the parent lives inside the current process, install a Windows hook
    if (pid == GetCurrentProcessId()) {
        w32->parent_win_hook = SetWindowsHookExW(WH_CALLWNDPROC,
            parent_win_hook, NULL, tid);
    } else {
        // Otherwise, use a WinEvent hook. These don't seem to be as smooth as
        // Windows hooks, but they can be delivered across process boundaries.
        w32->parent_evt_hook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            NULL, parent_evt_hook, pid, tid, WINEVENT_OUTOFCONTEXT);
    }
}

static void remove_parent_hook(struct vo_w32_state *w32)
{
    if (w32->parent_win_hook)
        UnhookWindowsHookEx(w32->parent_win_hook);
    if (w32->parent_evt_hook)
        UnhookWinEvent(w32->parent_evt_hook);
}

// Dispatch incoming window events and handle them.
// This returns only when the thread is asked to terminate.
static void run_message_loop(struct vo_w32_state *w32)
{
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0)
        DispatchMessageW(&msg);

    // Even if the message loop somehow exits, we still have to respond to
    // external requests until termination is requested.
    while (!w32->terminate)
        mp_dispatch_queue_process(w32->dispatch, 1000);
}

struct get_monitor_data {
    int i;
    int target;
    HMONITOR mon;
};

static BOOL CALLBACK get_monitor_proc(HMONITOR mon, HDC dc, LPRECT r, LPARAM p)
{
    struct get_monitor_data *data = (struct get_monitor_data*)p;

    if (data->i == data->target) {
        data->mon = mon;
        return FALSE;
    }
    data->i++;
    return TRUE;
}

static HMONITOR get_monitor(int id)
{
    struct get_monitor_data data = { .target = id };
    EnumDisplayMonitors(NULL, NULL, get_monitor_proc, (LPARAM)&data);
    return data.mon;
}

static void update_screen_rect(struct vo_w32_state *w32)
{
    struct mp_vo_opts *opts = w32->opts;
    int screen = w32->current_fs ? opts->fsscreen_id : opts->screen_id;

    // Handle --fs-screen=all
    if (w32->current_fs && screen == -2) {
        struct mp_rect rc = {
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN),
        };
        rc.x1 += rc.x0;
        rc.y1 += rc.y0;
        w32->screenrc = rc;
        return;
    }

    // When not using --fs-screen=all, mpv belongs to a specific HMONITOR
    HMONITOR mon;
    if (screen == -1) {
        // Handle --fs-screen=current and --screen=default
        mon = MonitorFromWindow(w32->window, MONITOR_DEFAULTTOPRIMARY);
    } else {
        mon = get_monitor(screen);
        if (!mon) {
            MP_INFO(w32, "Screen %d does not exist, falling back to primary\n",
                    screen);
            mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
    }

    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    w32->screenrc = (struct mp_rect){
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right, mi.rcMonitor.bottom,
    };
}

static DWORD update_style(struct vo_w32_state *w32, DWORD style)
{
    const DWORD NO_FRAME = WS_OVERLAPPED;
    const DWORD FRAME = WS_OVERLAPPEDWINDOW | WS_SIZEBOX;
    style &= ~(NO_FRAME | FRAME);
    style |= (w32->opts->border && !w32->current_fs) ? FRAME : NO_FRAME;
    return style;
}

// Update the window title, position, size, and border style.
static void reinit_window_state(struct vo_w32_state *w32)
{
    HWND layer = HWND_NOTOPMOST;
    RECT r;

    if (w32->parent)
        return;

    bool new_fs = w32->opts->fullscreen;
    bool toggle_fs = w32->current_fs != new_fs;
    w32->current_fs = new_fs;

    if (w32->taskbar_list) {
        ITaskbarList2_MarkFullscreenWindow(w32->taskbar_list,
                                           w32->window, w32->current_fs);
    }

    DWORD style = update_style(w32, GetWindowLong(w32->window, GWL_STYLE));

    if (w32->opts->ontop)
        layer = HWND_TOPMOST;

    // xxx not sure if this can trigger any unwanted messages (WM_MOVE/WM_SIZE)
    update_screen_rect(w32);

    int screen_w = w32->screenrc.x1 - w32->screenrc.x0;
    int screen_h = w32->screenrc.y1 - w32->screenrc.y0;

    if (w32->current_fs) {
        // Save window position and size when switching to fullscreen.
        if (toggle_fs) {
            w32->prev_width = w32->dw;
            w32->prev_height = w32->dh;
            w32->prev_x = w32->window_x;
            w32->prev_y = w32->window_y;
            MP_VERBOSE(w32, "save window bounds: %d:%d:%d:%d\n",
                   w32->prev_x, w32->prev_y, w32->prev_width, w32->prev_height);
        }

        w32->window_x = w32->screenrc.x0;
        w32->window_y = w32->screenrc.y0;
        w32->dw = screen_w;
        w32->dh = screen_h;
    } else {
        if (toggle_fs) {
            // Restore window position and size when switching from fullscreen.
            MP_VERBOSE(w32, "restore window bounds: %d:%d:%d:%d\n",
                   w32->prev_x, w32->prev_y, w32->prev_width, w32->prev_height);
            w32->dw = w32->prev_width;
            w32->dh = w32->prev_height;
            w32->window_x = w32->prev_x;
            w32->window_y = w32->prev_y;
        }
    }

    r.left = w32->window_x;
    r.right = r.left + w32->dw;
    r.top = w32->window_y;
    r.bottom = r.top + w32->dh;

    SetWindowLong(w32->window, GWL_STYLE, style);

    RECT cr = r;
    add_window_borders(w32->window, &r);
    // Check on client area size instead of window size on --fit-border=no
    long o_w;
    long o_h;
    if( w32->opts->fit_border ) {
        o_w = r.right - r.left;
        o_h = r.bottom - r.top;
    } else {
        o_w = cr.right - cr.left;
        o_h = cr.bottom - cr.top;
    }

    if ( !w32->current_fs && ( o_w > screen_w || o_h > screen_h ) )
    {
        MP_VERBOSE(w32, "requested window size larger than the screen\n");
        // Use the aspect of the client area, not the full window size.
        // Basically, try to compute the maximum window size.
        long n_w;
        long n_h;
        if( w32->opts->fit_border ) {
            n_w = screen_w - (r.right - cr.right) - (cr.left - r.left);
            n_h = screen_h - (r.bottom - cr.bottom) - (cr.top - r.top);
        } else {
            n_w = screen_w;
            n_h = screen_h;
        }
        // Letterbox
        double asp = (cr.right - cr.left) / (double)(cr.bottom - cr.top);
        double s_asp = n_w / (double)n_h;
        if (asp > s_asp) {
            n_h = n_w / asp;
        } else {
            n_w = n_h * asp;
        }
        // Save new size
        w32->dw = n_w;
        w32->dh = n_h;
        // Get old window center
        long o_cx = r.left + (r.right - r.left) / 2;
        long o_cy = r.top + (r.bottom - r.top) / 2;
        // Add window borders to the new window size
        r = (RECT){.right = n_w, .bottom = n_h};
        add_window_borders(w32->window, &r);
        // Get top and left border size for client area position calculation
        long b_top = -r.top;
        long b_left = -r.left;
        // Center the final window around the old window center
        n_w = r.right - r.left;
        n_h = r.bottom - r.top;
        r.left = o_cx - n_w / 2;
        r.top = o_cy - n_h / 2;
        r.right = r.left + n_w;
        r.bottom = r.top + n_h;
        // Save new client area position
        w32->window_x = r.left + b_left;
        w32->window_y = r.top + b_top;
    }

    MP_VERBOSE(w32, "reset window bounds: %d:%d:%d:%d\n",
               (int) r.left, (int) r.top, (int)(r.right - r.left),
               (int)(r.bottom - r.top));

    SetWindowPos(w32->window, layer, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    signal_events(w32, VO_EVENT_RESIZE);
}

static void gui_thread_reconfig(void *ptr)
{
    struct vo_w32_state *w32 = ptr;

    struct vo *vo = w32->vo;

    struct vo_win_geometry geo;
    vo_calc_window_geometry(vo, &w32->screenrc, &geo);
    vo_apply_window_geometry(vo, &geo);

    bool reset_size = w32->o_dwidth != vo->dwidth || w32->o_dheight != vo->dheight;
    bool pos_init = false;

    w32->o_dwidth = vo->dwidth;
    w32->o_dheight = vo->dheight;

    // the desired size is ignored in wid mode, it always matches the window size.
    if (!w32->parent) {
        if (w32->window_bounds_initialized) {
            // restore vo_dwidth/vo_dheight, which are reset against our will
            // in vo_config()
            RECT r;
            GetClientRect(w32->window, &r);
            vo->dwidth = r.right;
            vo->dheight = r.bottom;
        } else {
            w32->window_bounds_initialized = true;
            reset_size = true;
            pos_init = true;
            w32->window_x = w32->prev_x = geo.win.x0;
            w32->window_y = w32->prev_y = geo.win.y0;
        }

        if (reset_size) {
            w32->prev_width = vo->dwidth = w32->o_dwidth;
            w32->prev_height = vo->dheight = w32->o_dheight;
        }
    } else {
        RECT r;
        GetClientRect(w32->window, &r);
        vo->dwidth = r.right;
        vo->dheight = r.bottom;
    }

    // Recenter window around old position on new video size
    // excluding the case when initial position handled by win_state.
    if (!pos_init) {
        w32->window_x += w32->dw / 2 - vo->dwidth / 2;
        w32->window_y += w32->dh / 2 - vo->dheight / 2;
    }
    w32->dw = vo->dwidth;
    w32->dh = vo->dheight;

    reinit_window_state(w32);
}

// Resize the window. On the first call, it's also made visible.
void vo_w32_config(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    mp_dispatch_run(w32->dispatch, gui_thread_reconfig, w32);
}

static void thread_disable_ime(void)
{
    // Disables the IME for windows on this thread. imm32.dll must be loaded
    // dynamically to account for machines without East Asian language support.
    HMODULE imm32 = LoadLibraryW(L"imm32.dll");
    if (!imm32)
        return;
    BOOL (WINAPI *pImmDisableIME)(DWORD) = (BOOL (WINAPI*)(DWORD))
        GetProcAddress(imm32, "ImmDisableIME");
    if (pImmDisableIME)
        pImmDisableIME(0);
    FreeLibrary(imm32);
}

static void *gui_thread(void *ptr)
{
    struct vo_w32_state *w32 = ptr;
    bool ole_ok = false;
    int res = 0;

    mpthread_set_name("win32 window");

    thread_disable_ime();

    w32_thread_context = w32;

    if (w32->opts->WinID >= 0)
        w32->parent = (HWND)(intptr_t)(w32->opts->WinID);

    ATOM cls = get_window_class();
    if (w32->parent) {
        RECT r;
        GetClientRect(w32->parent, &r);
        w32->window = CreateWindowExW(WS_EX_NOPARENTNOTIFY,
                                      (LPWSTR)MAKEINTATOM(cls), L"mpv",
                                      WS_CHILD | WS_VISIBLE,
                                      0, 0, r.right, r.bottom,
                                      w32->parent, 0, HINST_THISCOMPONENT, NULL);

        // Install a hook to get notifications when the parent changes size
        if (w32->window)
            install_parent_hook(w32);
    } else {
        w32->window = CreateWindowExW(0, (LPWSTR)MAKEINTATOM(cls), L"mpv",
                                      update_style(w32, 0),
                                      CW_USEDEFAULT, SW_HIDE, 100, 100,
                                      0, 0, HINST_THISCOMPONENT, NULL);
    }

    if (!w32->window) {
        MP_ERR(w32, "unable to create window!\n");
        goto done;
    }

    if (SUCCEEDED(OleInitialize(NULL))) {
        ole_ok = true;

        fmtetc_url.cfFormat = (CLIPFORMAT)RegisterClipboardFormat(TEXT("UniformResourceLocatorW"));
        DropTarget* dropTarget = talloc(NULL, DropTarget);
        DropTarget_Init(dropTarget, w32);
        RegisterDragDrop(w32->window, &dropTarget->iface);

        // ITaskbarList2 has the MarkFullscreenWindow method, which is used to
        // make sure the taskbar is hidden when mpv goes fullscreen
        if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL,
                                       CLSCTX_INPROC_SERVER, &IID_ITaskbarList2,
                                       (void**)&w32->taskbar_list)))
        {
            if (FAILED(ITaskbarList2_HrInit(w32->taskbar_list))) {
                ITaskbarList2_Release(w32->taskbar_list);
                w32->taskbar_list = NULL;
            }
        }

        // ITaskbarList3 has methods for status indication on taskbar buttons,
        // however that interface is only available on Win7/2008 R2 or newer
        if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL,
                                       CLSCTX_INPROC_SERVER, &IID_ITaskbarList3,
                                       (void**)&w32->taskbar_list3)))
        {
            if (FAILED(ITaskbarList3_HrInit(w32->taskbar_list3))) {
                ITaskbarList3_Release(w32->taskbar_list3);
                w32->taskbar_list3 = NULL;
            } else {
                w32->tbtnCreatedMsg = RegisterWindowMessage(L"TaskbarButtonCreated");
            }
        }
    } else {
        MP_ERR(w32, "Failed to initialize OLE/COM\n");
    }

    w32->tracking   = FALSE;
    w32->trackEvent = (TRACKMOUSEEVENT){
        .cbSize    = sizeof(TRACKMOUSEEVENT),
        .dwFlags   = TME_LEAVE,
        .hwndTrack = w32->window,
    };

    if (w32->parent)
        EnableWindow(w32->window, 0);

    w32->cursor_visible = true;

    update_screen_rect(w32);

    mp_dispatch_set_wakeup_fn(w32->dispatch, wakeup_gui_thread, w32);

    res = 1;
done:

    mp_rendezvous(w32, res); // init barrier

    // This blocks until the GUI thread is to be exited.
    if (res)
        run_message_loop(w32);

    MP_VERBOSE(w32, "uninit\n");

    remove_parent_hook(w32);
    if (w32->window && !w32->destroyed)
        DestroyWindow(w32->window);
    if (w32->taskbar_list)
        ITaskbarList2_Release(w32->taskbar_list);
    if (w32->taskbar_list3)
        ITaskbarList3_Release(w32->taskbar_list3);
    if (ole_ok)
        OleUninitialize();
    SetThreadExecutionState(ES_CONTINUOUS);

    w32_thread_context = NULL;
    return NULL;
}

// Returns: 1 = Success, 0 = Failure
int vo_w32_init(struct vo *vo)
{
    assert(!vo->w32);

    struct vo_w32_state *w32 = talloc_ptrtype(vo, w32);
    *w32 = (struct vo_w32_state){
        .log = mp_log_new(w32, vo->log, "win32"),
        .vo = vo,
        .opts = vo->opts,
        .input_ctx = vo->input_ctx,
        .dispatch = mp_dispatch_create(w32),
    };
    vo->w32 = w32;

    if (pthread_create(&w32->thread, NULL, gui_thread, w32))
        goto fail;

    if (!mp_rendezvous(w32, 0)) { // init barrier
        pthread_join(w32->thread, NULL);
        goto fail;
    }

    // While the UI runs in its own thread, the thread in which this function
    // runs in will be the renderer thread. Apply magic MMCSS cargo-cult,
    // which might stop Windows from throttling clock rate and so on.
    if (vo->opts->mmcss_profile[0]) {
        wchar_t *profile = mp_from_utf8(NULL, vo->opts->mmcss_profile);
        w32->avrt_handle = AvSetMmThreadCharacteristicsW(profile, &(DWORD){0});
        talloc_free(profile);
    }

    return 1;
fail:
    talloc_free(w32);
    vo->w32 = NULL;
    return 0;
}

struct disp_names_data {
    HMONITOR assoc;
    int count;
    char **names;
};

static BOOL CALLBACK disp_names_proc(HMONITOR mon, HDC dc, LPRECT r, LPARAM p)
{
    struct disp_names_data *data = (struct disp_names_data*)p;

    // get_disp_names() adds data->assoc to the list, so skip it here
    if (mon == data->assoc)
        return TRUE;

    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    if (GetMonitorInfoW(mon, (MONITORINFO*)&mi)) {
        MP_TARRAY_APPEND(NULL, data->names, data->count,
                         mp_to_utf8(NULL, mi.szDevice));
    }
    return TRUE;
}

static char **get_disp_names(struct vo_w32_state *w32)
{
    // Get the client area of the window in screen space
    RECT rect = { 0 };
    GetClientRect(w32->window, &rect);
    MapWindowPoints(w32->window, NULL, (POINT*)&rect, 2);

    struct disp_names_data data = { .assoc = w32->monitor };

    // Make sure the monitor that Windows considers to be associated with the
    // window is first in the list
    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    if (GetMonitorInfoW(data.assoc, (MONITORINFO*)&mi)) {
        MP_TARRAY_APPEND(NULL, data.names, data.count,
                         mp_to_utf8(NULL, mi.szDevice));
    }

    // Get the names of the other monitors that intersect the client rect
    EnumDisplayMonitors(NULL, &rect, disp_names_proc, (LPARAM)&data);
    MP_TARRAY_APPEND(NULL, data.names, data.count, NULL);
    return data.names;
}

static int gui_thread_control(struct vo_w32_state *w32, int request, void *arg)
{
    switch (request) {
    case VOCTRL_FULLSCREEN:
        if (w32->opts->fullscreen != w32->current_fs)
            reinit_window_state(w32);
        return VO_TRUE;
    case VOCTRL_ONTOP:
        reinit_window_state(w32);
        return VO_TRUE;
    case VOCTRL_BORDER:
        reinit_window_state(w32);
        return VO_TRUE;
    case VOCTRL_GET_UNFS_WINDOW_SIZE: {
        int *s = arg;

        if (!w32->window_bounds_initialized)
            return VO_FALSE;

        s[0] = w32->current_fs ? w32->prev_width : w32->dw;
        s[1] = w32->current_fs ? w32->prev_height : w32->dh;
        return VO_TRUE;
    }
    case VOCTRL_SET_UNFS_WINDOW_SIZE: {
        int *s = arg;

        if (!w32->window_bounds_initialized)
            return VO_FALSE;
        if (w32->current_fs) {
            w32->prev_x += w32->prev_width / 2 - s[0] / 2;
            w32->prev_y += w32->prev_height / 2 - s[1] / 2;
            w32->prev_width = s[0];
            w32->prev_height = s[1];
        } else {
            w32->window_x += w32->dw / 2 - s[0] / 2;
            w32->window_y += w32->dh / 2 - s[1] / 2;
            w32->dw = s[0];
            w32->dh = s[1];
        }

        reinit_window_state(w32);
        return VO_TRUE;
    }
    case VOCTRL_GET_WIN_STATE:
        *(int *)arg = IsIconic(w32->window) ? VO_WIN_STATE_MINIMIZED : 0;
        return VO_TRUE;
    case VOCTRL_SET_CURSOR_VISIBILITY:
        w32->cursor_visible = *(bool *)arg;

        if (w32->can_set_cursor && w32->tracking) {
            if (w32->cursor_visible)
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            else
                SetCursor(NULL);
        }
        return VO_TRUE;
    case VOCTRL_KILL_SCREENSAVER:
        w32->disable_screensaver = true;
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                ES_DISPLAY_REQUIRED);
        return VO_TRUE;
    case VOCTRL_RESTORE_SCREENSAVER:
        w32->disable_screensaver = false;
        SetThreadExecutionState(ES_CONTINUOUS);
        return VO_TRUE;
    case VOCTRL_UPDATE_WINDOW_TITLE: {
        wchar_t *title = mp_from_utf8(NULL, (char *)arg);
        SetWindowTextW(w32->window, title);
        talloc_free(title);
        return VO_TRUE;
    }
    case VOCTRL_UPDATE_PLAYBACK_STATE: {
        w32->current_pstate = *(struct voctrl_playback_state *)arg;

        update_playback_state(w32);
        return VO_TRUE;
    }
    case VOCTRL_GET_DISPLAY_FPS:
        update_display_info(w32);
        *(double*) arg = w32->display_fps;
        return VO_TRUE;
    case VOCTRL_GET_DISPLAY_NAMES:
        *(char ***)arg = get_disp_names(w32);
        return VO_TRUE;
    case VOCTRL_GET_ICC_PROFILE:
        update_display_info(w32);
        if (w32->color_profile) {
            bstr *p = arg;
            *p = stream_read_file(w32->color_profile, NULL,
                w32->vo->global, 100000000); // 100 MB
            return p->len ? VO_TRUE : VO_FALSE;
        }
        return VO_FALSE;
    }
    return VO_NOTIMPL;
}

static void do_control(void *ptr)
{
    void **p = ptr;
    struct vo_w32_state *w32 = p[0];
    int *events = p[1];
    int request = *(int *)p[2];
    void *arg = p[3];
    int *ret = p[4];
    *ret = gui_thread_control(w32, request, arg);
    *events |= atomic_fetch_and(&w32->event_flags, 0);
    // Safe access, since caller (owner of vo) is blocked.
    if (*events & VO_EVENT_RESIZE) {
        w32->vo->dwidth = w32->dw;
        w32->vo->dheight = w32->dh;
    }
}

int vo_w32_control(struct vo *vo, int *events, int request, void *arg)
{
    struct vo_w32_state *w32 = vo->w32;
    if (request == VOCTRL_CHECK_EVENTS) {
        *events |= atomic_fetch_and(&w32->event_flags, 0);
        if (*events & VO_EVENT_RESIZE) {
            mp_dispatch_lock(w32->dispatch);
            vo->dwidth = w32->dw;
            vo->dheight = w32->dh;
            mp_dispatch_unlock(w32->dispatch);
        }
        return VO_TRUE;
    } else {
        int r;
        void *p[] = {w32, events, &request, arg, &r};
        mp_dispatch_run(w32->dispatch, do_control, p);
        return r;
    }
}

static void do_terminate(void *ptr)
{
    struct vo_w32_state *w32 = ptr;
    w32->terminate = true;

    if (!w32->destroyed)
        DestroyWindow(w32->window);

    mp_dispatch_interrupt(w32->dispatch);
}

void vo_w32_uninit(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    if (!w32)
        return;

    mp_dispatch_run(w32->dispatch, do_terminate, w32);
    pthread_join(w32->thread, NULL);

    AvRevertMmThreadCharacteristics(w32->avrt_handle);

    talloc_free(w32);
    vo->w32 = NULL;
}

HWND vo_w32_hwnd(struct vo *vo)
{
    struct vo_w32_state *w32 = vo->w32;
    return w32->window; // immutable, so no synchronization needed
}

void vo_w32_run_on_thread(struct vo *vo, void (*cb)(void *ctx), void *ctx)
{
    struct vo_w32_state *w32 = vo->w32;
    mp_dispatch_run(w32->dispatch, cb, ctx);
}
