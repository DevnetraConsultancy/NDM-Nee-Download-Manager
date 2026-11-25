// ndm_theme.h -- shared premium dark + gold drawing helpers for the NDM UI
// Copyright (c) 2026 Devnetra Consultancy. MIT License.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include "../common/ndm_common.h"

// ---------------------------------------------------------------------------
// Vector glyphs (drawn with GDI+, no external assets)
// ---------------------------------------------------------------------------
enum NdmGlyph
{
    GLY_NONE = 0,
    GLY_ADD, GLY_RESUME, GLY_PAUSE, GLY_STOPALL, GLY_DELETE, GLY_FOLDER,
    GLY_OPTIONS, GLY_ALL, GLY_ZIP, GLY_DOC, GLY_MUSIC, GLY_PROGRAM,
    GLY_VIDEO, GLY_UNFINISHED, GLY_FINISHED, GLY_CHECK, GLY_DOWNARROW,
    GLY_FILE, GLY_GLOBE, GLY_LINK, GLY_INFO, GLY_OPEN, GLY_CLOSE,
    GLY_MINUS, GLY_PLUS, GLY_REFRESH, GLY_BROWSER, GLY_MAGNET, GLY_PICTURE,
    GLY_CLOCK, GLY_SEARCH
};

// ---------------------------------------------------------------------------
// File categories (sidebar filters + category save folders)
// ---------------------------------------------------------------------------
enum NdmCategory
{
    CAT_ALL = 0,
    CAT_COMPRESSED,
    CAT_DOCUMENTS,
    CAT_MUSIC,
    CAT_PROGRAMS,
    CAT_VIDEO,
    CAT_PICTURES,
    CAT_TORRENT,                // NDM_KIND_TORRENT items
    CAT_UNFINISHED,
    CAT_FINISHED,
    CAT_COUNT,
    CAT_GENERAL = 100           // file type that has no dedicated category
};

// Button visual kinds for owner-drawn BUTTON controls
enum NdmBtnKind
{
    BTN_PRIMARY = 0,            // gold gradient, dark text
    BTN_SECONDARY,              // dark surface + border
    BTN_GHOST,                  // flat, gold text
    BTN_TOGGLE,                 // switch + label (uses BM_GETCHECK)
    BTN_STEPPER,                // small square -/+ (glyph)
    BTN_DANGER                  // red outline
};

// Sent by NdmTheme::PreTranslate to a modeless window for Ctrl+Tab.
#define WM_NDM_CTRLTAB  (WM_APP + 60)

namespace NdmTheme
{
    // ---- lifetime -------------------------------------------------------
    void   Init();                       // GDI+ startup, dark-mode opt-in
    void   Shutdown();

    // ---- DPI ------------------------------------------------------------
    UINT   GetDpi(HWND hwnd);
    inline int Scale(int v, UINT dpi) { return MulDiv(v, (int)dpi, 96); }
    BOOL   AdjustRectForDpi(RECT* rc, DWORD style, DWORD exStyle, UINT dpi);
    int    SysMetric(int idx, UINT dpi);

    // ---- window chrome ---------------------------------------------------
    void   DarkTitleBar(HWND hwnd);
    void   DarkScrollbars(HWND hwnd);     // DarkMode_Explorer theme + allow
    void   DarkTooltip(HWND hTip);
    void   ForceForeground(HWND hwnd);    // reliably bring to front
    // App icon from the resource at exactly px pixels (cached, never stretched)
    HICON  AppIcon(int px);
    // Set title-bar (small) + taskbar/Alt-Tab (big) icons at the window's
    // current DPI. Call on WM_CREATE and again on WM_DPICHANGED.
    void   SetWindowIcons(HWND hwnd, UINT dpi = 0);
    // Centre a top-level window with the given 96-dpi client size.
    void   PlaceWindow(HWND hwnd, int cw96, int ch96, HWND owner);
    // Origin point used when creating a dialog so it opens on the right
    // monitor (centre of owner, or the monitor under the cursor).
    POINT  CreationPoint(HWND owner);

    // ---- modeless window registry (for IsDialogMessage in msg loops) -----
    // PreTranslate first offers Ctrl+Tab / Ctrl+Shift+Tab to the window as
    // WM_NDM_CTRLTAB (wParam 1 = backwards); a nonzero result consumes it.
    void   AddModeless(HWND hwnd);
    void   RemoveModeless(HWND hwnd);
    bool   PreTranslate(MSG* msg);
    void   CloseAllModeless();

    // ---- fonts ----------------------------------------------------------
    HFONT  MakeFont(int px96, int weight, UINT dpi, const wchar_t* face = L"Segoe UI");

    // ---- GDI primitives (all leak-free) ---------------------------------
    void   Fill(HDC hdc, const RECT& rc, COLORREF c);
    void   GradientV(HDC hdc, const RECT& rc, COLORREF top, COLORREF bot);
    void   HLine(HDC hdc, int x1, int x2, int y, COLORREF c);
    void   VLine(HDC hdc, int x, int y1, int y2, COLORREF c);
    void   Text(HDC hdc, const std::wstring& s, RECT rc, COLORREF c,
                HFONT f, UINT fmt);
    SIZE   MeasureText(HDC hdc, const std::wstring& s, HFONT f);
    COLORREF Blend(COLORREF a, COLORREF b, int alpha255);  // a*(1-t)+b*t

    // ---- GDI+ shapes (anti-aliased) -------------------------------------
    void   RoundFill(HDC hdc, const RECT& rc, int radius, COLORREF c,
                     BYTE alpha = 255);
    void   RoundFillV(HDC hdc, const RECT& rc, int radius, COLORREF top,
                      COLORREF bot);
    void   RoundFrame(HDC hdc, const RECT& rc, int radius, COLORREF c,
                      float width = 1.0f, BYTE alpha = 255);
    void   Circle(HDC hdc, int cx, int cy, int r, COLORREF c);
    // Soft elliptical glow (centre colour at alpha fading to transparent)
    void   Glow(HDC hdc, const RECT& rc, COLORREF c, BYTE alpha);
    // Gold progress bar with rounded track; frac in [0,1]; indeterminate
    // when frac < 0 (phase animates a moving highlight).
    void   ProgressBar(HDC hdc, const RECT& rc, double frac, COLORREF fill,
                       COLORREF fillLight, int phase = 0);
    void   Glyph(HDC hdc, NdmGlyph g, const RECT& rc, COLORREF c,
                 float strokeScale = 1.0f);
    // NDM brand mark -- the one logo used everywhere. Drawn from vector
    // geometry snapped to whole pixels at the actual size (the same geometry
    // tools/make_icons.ps1 renders into ndm.ico and the browser icons).
    void   Logo(HDC hdc, const RECT& rc);
    void   GoldRule(HDC hdc, int x1, int x2, int y);      // fading gold line
    // Area sparkline of the most recent samples (gold line + soft fill)
    void   Sparkline(HDC hdc, const RECT& rc, const DWORD* samples, int count);
    // Rounded tile with a category glyph (dialog headers / progress)
    void   GlyphTile(HDC hdc, const RECT& rc, NdmGlyph g, UINT dpi);

    // ---- controls -------------------------------------------------------
    // Owner-draw push buttons: subclass for hover + hand cursor and
    // double-click -> click conversion. Toggles also keep their check state
    // (BM_GETCHECK / BM_SETCHECK) and flip it on click / space.
    void   MakeButton(HWND hBtn);
    void   MakeToggle(HWND hBtn);
    bool   IsHot(HWND hBtn);
    void   SetBtnGlyph(HWND hBtn, NdmGlyph g);
    void   DrawButton(const DRAWITEMSTRUCT* di, NdmBtnKind kind,
                      COLORREF parentBg, HFONT font, UINT dpi);
    // Rounded field frame drawn behind a border-less EDIT control
    void   DrawField(HDC hdc, const RECT& rc, bool focused, bool readOnly,
                     UINT dpi);

    // ---- accessibility (MSAA dynamic annotation via oleacc) ---------------
    // Toggles (MakeToggle) report themselves as check boxes with their
    // checked / focused / unavailable state; their name is the window text.
    // SetAccName gives a control whose caption is painted by its parent (a
    // border-less edit) an accessible name; "" / nullptr removes it. Both are
    // no-ops when COM is not initialised on the thread.
    void   SetAccName(HWND hCtl, const wchar_t* name);

    // ---- categories / formatting -----------------------------------------
    int          CategoryOfFile(const std::wstring& filename);
    const wchar_t* CategoryName(int cat);
    NdmGlyph     CategoryGlyph(int cat);
    std::wstring CategoryFolder(const std::wstring& base, int cat);
    std::wstring JoinPath(const std::wstring& dir, const std::wstring& file);
    std::wstring HostOf(const std::wstring& url);
    std::wstring SanitizeFilename(const std::wstring& name);
    bool         EnsureFolder(const std::wstring& dir);
    std::wstring StatusText(NdmStatus st);
    COLORREF     StatusColor(NdmStatus st);
    std::wstring BrowseFolder(HWND owner, const std::wstring& initial,
                              const wchar_t* title);
    void         OpenFolderSelect(const std::wstring& dir,
                                  const std::wstring& file);
}

// ---------------------------------------------------------------------------
// Per-dialog GDI resources (fonts / brushes) for a given DPI
// ---------------------------------------------------------------------------
struct NdmDlgRes
{
    HFONT  ui = nullptr, bold = nullptr, title = nullptr, sm = nullptr,
           caption = nullptr, big = nullptr;
    HBRUSH edit = nullptr, read = nullptr;
    UINT   dpi = 96;
    void   Create(UINT d);
    void   Free();
    int    S(int v) const { return MulDiv(v, (int)dpi, 96); }
};

namespace NdmTheme
{
    // Border-less EDIT for use inside a DrawField frame
    HWND   MakeEdit(HWND parent, int id, DWORD extraStyle = 0);
    // Owner-draw BUTTON (hover-tracked); toggle = switch with check state
    HWND   MakeBtn(HWND parent, int id, const wchar_t* text,
                   NdmGlyph glyph = GLY_NONE, bool toggle = false);
    // Position an edit vertically centred inside a field rectangle
    void   PlaceEdit(HWND edit, const RECT& field, HFONT font, int padX);
    // WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC for our edits
    HBRUSH EditColors(HDC hdc, HWND ctl, const NdmDlgRes& r);
    // Small upper-case caption with letter spacing
    void   Caption(HDC hdc, const wchar_t* text, RECT rc, const NdmDlgRes& r,
                   COLORREF c = NDM_CLR_FG_DIM);
    std::wstring GetText(HWND h);
}

// ---------------------------------------------------------------------------
// MemDC -- RAII double buffer. Draw into .dc with the same coordinates as
// the target; the destructor blits to the target and frees everything.
// ---------------------------------------------------------------------------
class NdmMemDC
{
public:
    NdmMemDC(HDC target, const RECT& rc);
    ~NdmMemDC();
    HDC  dc = nullptr;
    bool ok() const { return dc != nullptr; }
private:
    HDC     m_target = nullptr;
    HBITMAP m_bmp    = nullptr;
    HGDIOBJ m_old    = nullptr;
    RECT    m_rc{};
};
