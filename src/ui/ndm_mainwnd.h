// ndm_mainwnd.h -- NDM main window (premium dark + gold)
// Invented with Care By Devnetra Consultancy
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include "../common/ndm_common.h"
#include "ndm_theme.h"
#include "ndm_uilogic.h"

struct IDropTarget;

// ---------------------------------------------------------------------------
// Control / command IDs
// ---------------------------------------------------------------------------
#define IDC_LISTVIEW        1002

#define ID_CMD_ADD          2001
#define ID_CMD_RESUME       2002
#define ID_CMD_PAUSE        2003
#define ID_CMD_DELETE       2004
#define ID_CMD_OPENFOLDER   2005
#define ID_CMD_OPTIONS      2006
#define ID_CMD_STOPALL      2007
#define ID_CMD_OPEN         2008
#define ID_CMD_REDOWNLOAD   2009
#define ID_CMD_COPYURL      2010
#define ID_CMD_PROPERTIES   2011
#define ID_CMD_SELECTALL    2012
#define ID_CMD_RESUMEALL    2013
#define ID_CMD_DEFAULT      2014   // Enter / double-click
#define ID_CMD_RENAME       2015   // F2
#define ID_CMD_COPYNAME     2016
#define ID_CMD_DELETEFILE   2017   // Shift+Del: remove + file to the Recycle Bin
#define ID_CMD_REFRESHADDR  2018
#define ID_CMD_SCHEDULED    2019   // "Add to scheduler" toggle
#define ID_CMD_PRIO_LOW     2020   // + NDM_PRIO_*
#define ID_CMD_PRIO_NORMAL  2021
#define ID_CMD_PRIO_HIGH    2022
#define ID_CMD_SPEED_OFF    2023
#define ID_CMD_SPEED_256K   2024
#define ID_CMD_SPEED_1M     2025
#define ID_CMD_SPEED_5M     2026
#define ID_CMD_SPEED_CUSTOM 2027
#define ID_CMD_MOVE_TOP     2028
#define ID_CMD_MOVE_UP      2029
#define ID_CMD_MOVE_DOWN    2030
#define ID_CMD_MOVE_BOTTOM  2031
#define ID_CMD_SCHEDULER    2032   // toolbar: Scheduler window
#define ID_CMD_FIND         2033   // Ctrl+F: focus the search box
#define IDC_SEARCH          1003

#define ID_TRAY_SHOW        3001
#define ID_TRAY_ADD         3002
#define ID_TRAY_EXIT        3003
#define ID_TRAY_OPTIONS     3004
#define ID_TRAY_PAUSEALL    3005
#define ID_TRAY_RESUMEALL   3006
#define ID_TRAY_SPEED_OFF   3010   // global speed limit presets
#define ID_TRAY_SPEED_256K  3011
#define ID_TRAY_SPEED_1M    3012
#define ID_TRAY_SPEED_5M    3013
#define ID_TRAY_SPEED_CUSTOM 3014

// Private messages
#define WM_NDM_URL_REQUEST  (WM_APP + 20)   // retired: ignored (requests go through NdmPostBrowserRequest)
#define WM_NDM_SHOW_MAIN    (WM_APP + 21)
#define WM_NDM_DONE_ACTION  (WM_APP + 22)   // wParam NDM_DONE_*, lParam id (NdmPower::OnQueueAction)
#define WM_NDM_DROP         (WM_APP + 23)   // something was dropped on the window (see m_drops)
#define WM_NDM_COLWIDTH     (WM_APP + 24)   // wParam column: the user resized it (header drag / double-click)

// COPYDATA codes
#define NDM_COPYDATA_SHOW    0x4E444D53     // 'NDMS' : just show the window (second instance)
#define NDM_COPYDATA_CMDLINE 0x4E444D43     // 'NDMC' : second instance forwards its command-line
                                            //          argument (URL or local .torrent path)
// WM_NDM_ADD_DOWNLOAD (shell extension): http/https/magnet URLs only.

// ListView columns
enum
{
    COL_FILENAME = 0,
    COL_DATE,                   // completed (finished) / added (others), local time
    COL_SIZE,
    COL_STATUS,
    COL_PROGRESS,
    COL_SPEED,
    COL_TIMELEFT,
    COL_URL,
    COL_COUNT
};

// Timer
#define NDM_TIMER_ID    100
#define NDM_TIMER_MS    500
#define NDM_ANIM_TIMER  101
#define NDM_CLIP_TIMER  102     // clipboard was busy: read it again shortly

// ---------------------------------------------------------------------------
// NdmMainWindow
// ---------------------------------------------------------------------------
class NdmMainWindow
{
public:
    static NdmMainWindow& GetInstance();

    NdmMainWindow(const NdmMainWindow&)            = delete;
    NdmMainWindow& operator=(const NdmMainWindow&) = delete;

    HWND  Create(HINSTANCE hInst, bool startHidden);
    HWND  GetHwnd() const { return m_hWnd; }
    HWND  GetSearchHwnd() const { return m_hSearch; }
    HACCEL GetAccel() const { return m_hAccel; }
    void  RefreshListView() { Refresh(true); }
    void  ShowMain();                       // restore from tray / bring to front

    // Shown by the browser bridge / shell ext / Add URL
    void  HandleBrowserRequest(NdmBrowserRequest* req);   // takes ownership
    static bool AcceptableSource(const std::wstring& url, int origin);   // NDM_ORIGIN_*

    // Select a download in the list (clearing a filter that hides it),
    // bring the window up and open its download window.
    void  ShowExisting(DWORD id);
    // Clipboard text NDM itself just wrote (Copy address / file name): the
    // clipboard watcher ignores this sequence number.
    void  NoteOwnClipboard(DWORD seq) { m_ownClipSeq = seq; }

    // Called by the drop target (OLE thread = UI thread): queue + post.
    void  QueueDrop(const std::vector<std::wstring>& items, bool files);

private:
    NdmMainWindow() = default;

    // One row of the list -- a copy taken on the UI thread each tick
    struct Row
    {
        DWORD        id = 0;
        std::wstring name, url, savePath, error;
        UINT64       total = 0, done = 0;
        DWORD        speed = 0;
        NdmStatus    status = NDM_STATUS_QUEUED;
        int          conns = 0;
        int          cat = 0;
        int          kind = 0;          // NDM_KIND_*
        UINT64       dateFt = 0;        // NdmUi::ListDateFt (UTC FILETIME, 0 = unknown)
        int          priority = 1;
        DWORD        speedLimit = 0;
        bool         scheduled = false;
        INT64        queueOrder = 0;
        bool operator==(const Row& o) const
        {
            return id == o.id && total == o.total && done == o.done &&
                   speed == o.speed && status == o.status && name == o.name &&
                   url == o.url && error == o.error && savePath == o.savePath &&
                   dateFt == o.dateFt && priority == o.priority && speedLimit == o.speedLimit &&
                   scheduled == o.scheduled;
        }
    };

    struct ToolBtn
    {
        int          cmd;
        int          glyph;
        const wchar_t* label;
        const wchar_t* tip;
        RECT         rc{};
        bool         enabled = true;
        bool         groupEnd = false;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK HeaderSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK SearchSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // setup / layout
    void  OnCreate();
    void  CreateFonts();
    void  FreeFonts();
    void  CreateList();
    void  ApplyListMetrics();
    void  Layout();
    void  FitLastColumn();
    void  LayoutSidebar();
    void  ScrollSidebar(int pos);
    void  SaveUiState();
    bool  PointerOverList() const;
    void  UpdateTooltips();

    // painting
    void  OnPaint();
    void  PaintSpeed(HDC dc, const RECT& rc);
    void  PaintToolbar(HDC dc, const RECT& rc);
    void  PaintSidebar(HDC dc, const RECT& rc);
    void  PaintStatus(HDC dc, const RECT& rc);
    void  PaintColumnHeader(HWND hHdr, HDC dc);
    LRESULT OnListCustomDraw(NMLVCUSTOMDRAW* cd);
    void  DrawRow(HDC dc, int index, const RECT& rowRc);
    void  DrawEmptyState(HDC dc);

    // data
    void  Refresh(bool force);
    void  RebuildView(bool keepSelection);
    bool  RowMatches(const Row& r, int cat) const;
    void  SortView();
    std::vector<DWORD> SelectedIds() const;
    const Row* RowById(DWORD id) const;
    void  UpdateToolbarState();
    void  CheckNotifications();

    // input
    int   HitToolbar(POINT pt) const;
    int   HitSidebar(POINT pt) const;
    void  OnMouseMove(POINT pt);
    void  OnLButtonDown(POINT pt);
    void  OnLButtonUp(POINT pt);
    void  OnCommand(int id);
    void  OnContextMenu(POINT screenPt);
    void  OnColumnClick(int col);

    // commands
    void  CmdAdd();
    void  CmdResume();
    void  CmdPause();
    void  CmdStopAll();
    void  CmdResumeAll();
    void  CmdDelete();
    void  CmdOpenFolder();
    void  CmdOpen();
    void  CmdRedownload();
    void  CmdCopyUrl();
    void  CmdProperties();
    void  CmdDefault();
    void  CmdOptions();
    void  CmdRename();
    void  CmdCopyName();
    void  CmdDeleteWithFile();
    void  CmdRefreshAddress();
    void  CmdPriority(int prio);
    void  CmdSpeed(DWORD bps, bool custom);
    void  CmdMove(int cmd);
    void  CmdScheduled();
    void  CmdTraySpeed(DWORD bps, bool custom);
    void  OnSearchChanged();
    void  OnClipboardUpdate();
    void  OnDrop();
    bool  CheckBrowserDuplicates(NdmBrowserRequest& req);   // false = handled / cancelled
    void  ExitApp();
    void  ExitNow();                        // NDM_ExitNow_v1: save, stop engines, close

    // tray
    void  AddTrayIcon();
    void  RemoveTrayIcon();
    void  UpdateTrayTip();
    void  ShowTrayMenu();
    enum { BALLOON_INFO = 0, BALLOON_COMPLETE, BALLOON_FAILED };
    void  Balloon(const std::wstring& title, const std::wstring& text, DWORD id, int kind = BALLOON_INFO);
    void  HideToTray();

    // window placement persistence
    void  SavePlacement();
    bool  LoadPlacement(WINDOWPLACEMENT& wp);

    HICON FileIcon(const std::wstring& name, int size);
    int   S(int v) const { return MulDiv(v, (int)m_dpi, 96); }

    // ---- state -----------------------------------------------------------
    HWND      m_hWnd     = nullptr;
    HWND      m_hList    = nullptr;
    HWND      m_hHdr     = nullptr;
    HWND      m_hTip     = nullptr;
    HWND      m_hSearch  = nullptr;
    HBRUSH    m_brEdit   = nullptr;
    HINSTANCE m_hInst    = nullptr;
    HACCEL    m_hAccel   = nullptr;
    HICON     m_hIconBig = nullptr;
    HICON     m_hIconSm  = nullptr;
    HIMAGELIST m_hRowIml = nullptr;
    UINT      m_dpi      = 96;
    UINT      m_msgTaskbarCreated = 0;
    UINT      m_msgExitNow  = 0;            // RegisterWindowMessage("NDM_ExitNow_v1")
    bool      m_exitNowDone = false;
    bool      m_trayAdded   = false;
    bool      m_startHidden = false;
    bool      m_exiting     = false;

    // fonts
    HFONT m_fUI = nullptr, m_fSmall = nullptr, m_fBold = nullptr,
          m_fTitle = nullptr, m_fCaption = nullptr, m_fSpeed = nullptr,
          m_fTool = nullptr, m_fHdr = nullptr;

    // layout rects (client coords)
    RECT m_rcHeader{}, m_rcToolbar{}, m_rcSidebar{}, m_rcList{}, m_rcStatus{};
    RECT m_rcSpeed{};
    RECT m_rcSearch{};
    std::vector<ToolBtn> m_btns;
    RECT m_catRc[CAT_COUNT] = {};
    int  m_sideScroll = 0, m_sideContentH = 0;   // sidebar scroll offset / content height (px)
    RECT m_rcSideTrack{}, m_rcSideThumb{};      // slim scrollbar (empty when it all fits)
    bool m_sideDrag = false, m_sideTrackHot = false;
    int  m_sideDragY = 0, m_sideDragPos = 0;
    int  m_wheelAccum = 0;
    int  m_hotBtn = -1, m_pressBtn = -1, m_hotCat = -1;
    bool m_trackingMouse = false;

    // data
    std::vector<Row>     m_all;          // every download (snapshot)
    std::vector<int>     m_view;         // indices into m_all for the list
    std::vector<DWORD>   m_viewIds;
    int                  m_filter   = 0;
    int                  m_catCount[CAT_COUNT] = {};
    int                  m_sortCol  = -1;
    bool                 m_sortAsc  = true;
    ULONGLONG            m_lastSortMs = 0;   // last time the view was sorted (live columns)
    bool                 m_menuOpen = false; // a menu is up: live columns are not re-sorted
    int                  m_colW96[COL_COUNT] = {};   // the user's widths (96 dpi); Address fills the rest
    bool                 m_colTracking = false;
    int                  m_hotRow   = -1;
    bool                 m_updating = false;
    int                  m_animPhase = 0;
    std::map<DWORD, NdmStatus> m_lastStatus;
    std::map<std::wstring, HICON> m_iconCache;
    std::vector<DWORD>   m_speedHist;    // total speed history (sparkline)
    DWORD                m_totalSpeed = 0;
    int                  m_activeCount = 0;
    DWORD                m_balloonId = 0;
    int                  m_balloonKind = BALLOON_INFO;
    int                  m_saveTick = 0;

    // search / clipboard / drag and drop
    std::wstring         m_query;
    DWORD                m_ownClipSeq = 0;
    int                  m_clipRetries = 0;
    NdmUi::ClipboardGate m_clipGate;
    IDropTarget*         m_dropTarget = nullptr;    // registered on the window and the list
    std::vector<std::wstring> m_drops;
    bool                 m_dropFiles = false;
};
