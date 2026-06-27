#include "window.h"
#include "resource.h"
#include "processing.h"
#include "settings.h"
#include "dragdrop.h"
#include "utils.h"
#include "update.h"
#include <shlwapi.h>
#include <shellapi.h>

// Control layout helpers
#define CONTROL_MARGIN 10
#define CONTROL_HEIGHT 22
#define LABEL_WIDTH 100
#define VALUE_WIDTH 60

static BOOL CALLBACK enum_child_proc(HWND hwnd, LPARAM lParam) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

static double qpc_elapsed_ms(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq) {
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static ImageFormat get_selected_format(AppState *state) {
    int format_idx = (int)SendMessageW(state->hFormatCombo, CB_GETCURSEL, 0, 0);
    switch (format_idx) {
        case 0: return FORMAT_WEBP;
        case 1: return FORMAT_JPEG;
        case 2: return FORMAT_PNG;
        case 3: return FORMAT_BMP;
        case 4: return FORMAT_TIFF;
        case 5: return FORMAT_ICO;
        default: return FORMAT_WEBP;
    }
}

static int get_max_res_from_combo(AppState *state) {
    int sel = (int)SendMessageW(state->hMaxResCombo, CB_GETCURSEL, 0, 0);
    switch (sel) {
        case 1: return 2160;
        case 2: return 1440;
        case 3: return 1080;
        default: return 0;
    }
}

static void sync_max_res_from_ui(AppState *state) {
    state->max_resolution = get_max_res_from_combo(state);
}

static void update_max_res_enable(AppState *state) {
    int max_res = get_max_res_from_combo(state);
    BOOL use_max_res = (max_res > 0);
    EnableWindow(state->hResolutionTrack, !use_max_res);
    EnableWindow(state->hResolutionLabel, !use_max_res);
    EnableWindow(state->hResolutionTextLabel, !use_max_res);
}

static void update_jpeg_fast_enable(AppState *state) {
    (void)state;
}

static void create_label(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                  x, y, w, h, parent, NULL, NULL, NULL);
}

static void window_init_menu(HWND hwnd) {
    HMENU menu = CreateMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuW(help, MF_STRING, ID_HELP_CHECK_UPDATES, L"Check for updates");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"About");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"Help");
    SetMenu(hwnd, menu);
}

static void window_layout_controls(HWND hwnd, AppState *state, int width, int height) {
    if (!state) return;

    int splitter_x = width * 65 / 100;
    int splitter_width = 5;
    int progress_height = 30;
    int content_height = height - progress_height - CONTROL_MARGIN;

    int left_width = splitter_x - CONTROL_MARGIN;
    int right_x = splitter_x + splitter_width;
    int right_width = width - right_x - CONTROL_MARGIN;

    int right_y = CONTROL_MARGIN;
    int control_margin = 4;
    int short_control_width = 60;
    int label_width = 95;
    int label_h = 14;
    int row_h = 20;
    int track_h = 18;

    // Tab control and file list
    MoveWindow(state->hTabControl, CONTROL_MARGIN, CONTROL_MARGIN,
               left_width, 30, TRUE);
    MoveWindow(state->hFilesList, CONTROL_MARGIN, CONTROL_MARGIN + 35,
               left_width, content_height - CONTROL_MARGIN - 35, TRUE);
    ListView_SetColumnWidth(state->hFilesList, 0, left_width - 25);

    // Output directory row
    MoveWindow(state->hOutputDirLabel, right_x, right_y, 90, label_h, TRUE);
    right_y += label_h;

    MoveWindow(state->hOutputDirEdit, right_x, right_y,
               right_width - 58, row_h, TRUE);
    MoveWindow(state->hBrowseBtn, right_x + right_width - 54, right_y, 50, row_h, TRUE);
    right_y += row_h + control_margin;

    // File count labels
    MoveWindow(state->hNumFilesTextLabel, right_x, right_y, label_width, label_h, TRUE);
    MoveWindow(state->hNumFilesLabel, right_x + label_width + 5, right_y, 50, label_h, TRUE);
    right_y += label_h;

    MoveWindow(state->hCompletedTextLabel, right_x, right_y, label_width, label_h, TRUE);
    MoveWindow(state->hCompletedLabel, right_x + label_width + 5, right_y, 50, label_h, TRUE);
    right_y += label_h + control_margin;

    // Size labels
    MoveWindow(state->hBeforeSizeTextLabel, right_x, right_y, label_width, label_h, TRUE);
    MoveWindow(state->hBeforeSizeLabel, right_x + label_width + 5, right_y, 80, label_h, TRUE);
    right_y += label_h;

    MoveWindow(state->hAfterSizeTextLabel, right_x, right_y, label_width, label_h, TRUE);
    MoveWindow(state->hAfterSizeLabel, right_x + label_width + 5, right_y, 80, label_h, TRUE);
    right_y += label_h;

    MoveWindow(state->hSavedTextLabel, right_x, right_y, label_width, label_h, TRUE);
    MoveWindow(state->hPercentSavedLabel, right_x + label_width + 5, right_y, 60, label_h, TRUE);
    right_y += label_h + control_margin;

    // Statistics labels
    MoveWindow(state->hElapsedLabel, right_x, right_y, 120, label_h, TRUE);
    right_y += label_h;
    MoveWindow(state->hFilesPerSecLabel, right_x, right_y, 90, label_h, TRUE);
    right_y += label_h;
    MoveWindow(state->hMbPerSecLabel, right_x, right_y, 90, label_h, TRUE);
    right_y += label_h + control_margin;

    // Format dropdown
    MoveWindow(state->hFormatLabel, right_x, right_y, 50, label_h, TRUE);
    MoveWindow(state->hFormatCombo, right_x + 55, right_y - 2, short_control_width, 150, TRUE);
    right_y += row_h + control_margin;

    // Quality
    MoveWindow(state->hQualityTextLabel, right_x, right_y, 50, label_h, TRUE);
    MoveWindow(state->hQualityTrack, right_x + 55, right_y - 2, short_control_width + 80, track_h, TRUE);
    MoveWindow(state->hQualityLabel, right_x + 55 + short_control_width + 80, right_y - 2, 30, track_h, TRUE);
    right_y += track_h + control_margin;

    // Resolution
    MoveWindow(state->hResolutionTextLabel, right_x, right_y, 65, label_h, TRUE);
    MoveWindow(state->hResolutionTrack, right_x + 55, right_y - 2, short_control_width + 80, track_h, TRUE);
    MoveWindow(state->hResolutionLabel, right_x + 55 + short_control_width + 80, right_y - 2, 30, track_h, TRUE);
    right_y += track_h + control_margin;

    // Max Res
    MoveWindow(state->hMaxResLabel, right_x, right_y, 60, label_h, TRUE);
    MoveWindow(state->hMaxResCombo, right_x + 70, right_y - 2, short_control_width + 20, 120, TRUE);
    right_y += row_h + control_margin;

    // Threads
    MoveWindow(state->hThreadsTextLabel, right_x, right_y, 50, label_h, TRUE);
    MoveWindow(state->hThreadTrack, right_x + 55, right_y - 2, short_control_width + 80, track_h, TRUE);
    MoveWindow(state->hThreadLabel, right_x + 55 + short_control_width + 80, right_y - 2, 30, track_h, TRUE);
    right_y += track_h + control_margin;

    // Checkboxes / buttons
    MoveWindow(state->hPreserveFolderCheck, right_x, right_y + 1, 170, 18, TRUE);
    right_y += 20 + control_margin;

    MoveWindow(state->hOptimizedCheck, right_x, right_y + 1, 100, 24, TRUE);
    right_y += 24 + control_margin;

    MoveWindow(state->hLoggingCheck, right_x, right_y + 1, 120, 18, TRUE);
    right_y += 20 + control_margin;

    MoveWindow(state->hProcessBtn, right_x, right_y, right_width, 34, TRUE);
    right_y += 40 + control_margin;

    // Anchor Clear/Cancel to right edge of panel
    int action_btn_w = 110;
    MoveWindow(state->hCancelBtn, right_x + right_width - action_btn_w, right_y - 70, action_btn_w, 24, TRUE);
    MoveWindow(state->hClearBtn, right_x + right_width - action_btn_w, right_y - 96, action_btn_w, 24, TRUE);

    // Progress bar
    MoveWindow(state->hProgressFiles, 0, height - progress_height, width, progress_height, TRUE);
}

static void create_trackbar(HWND parent, int id, const wchar_t *label, int x, int y, int min, int max, int val, HWND *trackbar_hwnd, HWND *label_hwnd) {
    create_label(parent, label, x, y + 3, LABEL_WIDTH, CONTROL_HEIGHT);

    HWND hTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                  x + LABEL_WIDTH + 5, y, 150, CONTROL_HEIGHT,
                                  parent, (HMENU)(INT_PTR)id, NULL, NULL);

    SendMessage(hTrack, TBM_SETRANGE, TRUE, MAKELPARAM(min, max));
    SendMessage(hTrack, TBM_SETPOS, TRUE, val);

    wchar_t buf[32];
    swprintf(buf, 32, L"%d", val);
    *label_hwnd = CreateWindowW(L"STATIC", buf, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                               x + LABEL_WIDTH + 160, y + 3, VALUE_WIDTH, CONTROL_HEIGHT,
                               parent, NULL, NULL, NULL);

    *trackbar_hwnd = hTrack;
}

BOOL window_init_controls(HWND hwnd, AppState *state) {
    // Get client area
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    // Layout dimensions matching C# version
    // Left panel (file list): ~65% of width, right panel (controls): ~35%
    int splitter_x = width * 65 / 100;  // Split at 65%
    int splitter_width = 5;
    int progress_height = 30;
    int content_height = height - progress_height - CONTROL_MARGIN;

    int left_width = splitter_x - CONTROL_MARGIN;
    int right_x = splitter_x + splitter_width;
    int right_width = width - right_x - CONTROL_MARGIN;

    // ===== LEFT PANEL: Tab Control =====
    state->hTabControl = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | TCS_BUTTONS,
        CONTROL_MARGIN, CONTROL_MARGIN, left_width, 30,
        hwnd, NULL, NULL, NULL);
    
    // Add tabs
    TCITEMW tci = {0};
    tci.mask = TCIF_TEXT;
    tci.pszText = L"All Files";
    SendMessageW(state->hTabControl, TCM_INSERTITEMW, 0, (LPARAM)&tci);
    
    // "Failed Files" tab is added dynamically when files fail
    // Initialize tab state
    state->current_tab = 0;
    
    // ===== LEFT PANEL: File List =====
    state->hFilesList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
        CONTROL_MARGIN, CONTROL_MARGIN + 35, left_width, content_height - CONTROL_MARGIN - 35,
        hwnd, (HMENU)(INT_PTR)IDC_FILES_LIST, NULL, NULL);

    // Enable grid lines and full row selection
    ListView_SetExtendedListViewStyle(state->hFilesList, LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT);

    // Add columns - single column for file list like C# version
    LVCOLUMNW lvc = {0};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.pszText = L"Input Files (Drag and Drop)";
    lvc.cx = left_width - 25;
    SendMessageW(state->hFilesList, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);

    // ===== RIGHT PANEL: Controls =====
    int right_y = CONTROL_MARGIN;
    int control_margin = 4;
    int short_control_width = 60;
    int label_width = 95;

    // Output directory section
    state->hOutputDirLabel = CreateWindowW(L"STATIC", L"Output Path:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, 90, 14, hwnd, NULL, NULL, NULL);
    right_y += 14;

    state->hOutputDirEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        right_x, right_y, right_width - 58, 22,
        hwnd, (HMENU)(INT_PTR)IDC_OUTPUT_DIR_EDIT, NULL, NULL);

    state->hBrowseBtn = CreateWindowW(L"BUTTON", L"Open",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        right_x + right_width - 54, right_y, 50, 22,
        hwnd, (HMENU)(INT_PTR)IDC_BROWSE_BTN, NULL, NULL);

    right_y += 20 + control_margin;

    // File count labels
    state->hNumFilesTextLabel = CreateWindowW(L"STATIC", L"Number of Files:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, label_width, 14, hwnd, NULL, NULL, NULL);
    state->hNumFilesLabel = CreateWindowW(L"STATIC", L"0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x + label_width + 5, right_y, 50, 14,
        hwnd, (HMENU)(INT_PTR)IDC_NUM_FILES_LABEL, NULL, NULL);
    right_y += 14;

    state->hCompletedTextLabel = CreateWindowW(L"STATIC", L"Completed:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, label_width, 14, hwnd, NULL, NULL, NULL);
    state->hCompletedLabel = CreateWindowW(L"STATIC", L"0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x + label_width + 5, right_y, 50, 14,
        hwnd, (HMENU)(INT_PTR)IDC_COMPLETED_LABEL, NULL, NULL);
    right_y += 14 + control_margin;

    // Size labels
    state->hBeforeSizeTextLabel = CreateWindowW(L"STATIC", L"Before Size:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, label_width, 14, hwnd, NULL, NULL, NULL);
    state->hBeforeSizeLabel = CreateWindowW(L"STATIC", L"0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x + label_width + 5, right_y, 80, 14,
        hwnd, (HMENU)(INT_PTR)IDC_BEFORE_SIZE_LABEL, NULL, NULL);
    right_y += 14;

    state->hAfterSizeTextLabel = CreateWindowW(L"STATIC", L"After Size:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, label_width, 14, hwnd, NULL, NULL, NULL);
    state->hAfterSizeLabel = CreateWindowW(L"STATIC", L"0",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x + label_width + 5, right_y, 80, 14,
        hwnd, (HMENU)(INT_PTR)IDC_AFTER_SIZE_LABEL, NULL, NULL);
    right_y += 14;

    state->hSavedTextLabel = CreateWindowW(L"STATIC", L"Saved:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, label_width, 14, hwnd, NULL, NULL, NULL);
    state->hPercentSavedLabel = CreateWindowW(L"STATIC", L"0.00%",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x + label_width + 5, right_y, 60, 14,
        hwnd, (HMENU)(INT_PTR)IDC_PERCENT_SAVED_LABEL, NULL, NULL);
    right_y += 14 + control_margin;

    // Statistics labels
    state->hElapsedLabel = CreateWindowW(L"STATIC", L"Elapsed: 00:00:00",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x, right_y, 120, 14,
        hwnd, (HMENU)(INT_PTR)IDC_ELAPSED_LABEL, NULL, NULL);
    right_y += 14;

    state->hFilesPerSecLabel = CreateWindowW(L"STATIC", L"0.00 files/s",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x, right_y, 90, 14,
        hwnd, (HMENU)(INT_PTR)IDC_FILES_PER_SEC_LABEL, NULL, NULL);
    right_y += 14;

    state->hMbPerSecLabel = CreateWindowW(L"STATIC", L"0.00 MB/s",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        right_x, right_y, 90, 14,
        hwnd, (HMENU)(INT_PTR)IDC_MB_PER_SEC_LABEL, NULL, NULL);
    right_y += 14 + control_margin;

    // Format dropdown (shorter like C#)
    state->hFormatLabel = CreateWindowW(L"STATIC", L"Format:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, 50, 14, hwnd, NULL, NULL, NULL);
    state->hFormatCombo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        right_x + 55, right_y - 2, short_control_width, 150,
        hwnd, (HMENU)(INT_PTR)IDC_FORMAT_COMBO, NULL, NULL);
    SendMessageW(state->hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"WebP");
    SendMessageW(state->hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"JPEG");
    SendMessageW(state->hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"PNG");
    SendMessageW(state->hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"BMP");
    SendMessageW(state->hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"TIFF");
    SendMessageW(state->hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"ICO");
    SendMessageW(state->hFormatCombo, CB_SETCURSEL, 0, 0);
    right_y += 20 + control_margin;

    // Quality trackbar (compact)
    state->hQualityTextLabel = CreateWindowW(L"STATIC", L"Quality:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, 50, 14, hwnd, NULL, NULL, NULL);
    state->hQualityTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                  right_x + 55, right_y - 2, short_control_width + 80, 18,
                                  hwnd, (HMENU)(INT_PTR)IDC_QUALITY_TRACK, NULL, NULL);
    SendMessage(state->hQualityTrack, TBM_SETRANGE, TRUE, MAKELPARAM(1, 100));
    SendMessage(state->hQualityTrack, TBM_SETPOS, TRUE, state->quality);
    state->hQualityLabel = CreateWindowW(L"STATIC", L"85", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               right_x + 55 + short_control_width + 80, right_y - 2, 30, 18,
                               hwnd, (HMENU)(INT_PTR)IDC_QUALITY_LABEL, NULL, NULL);
    right_y += 20 + control_margin;

    // Resolution trackbar (compact)
    state->hResolutionTextLabel = CreateWindowW(L"STATIC", L"Resolution:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, 65, 14, hwnd, NULL, NULL, NULL);
    state->hResolutionTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                  right_x + 55, right_y - 2, short_control_width + 80, 18,
                                  hwnd, (HMENU)(INT_PTR)IDC_RESOLUTION_TRACK, NULL, NULL);
    SendMessage(state->hResolutionTrack, TBM_SETRANGE, TRUE, MAKELPARAM(10, 100));
    SendMessage(state->hResolutionTrack, TBM_SETPOS, TRUE, state->resolution_percent);
    state->hResolutionLabel = CreateWindowW(L"STATIC", L"100", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               right_x + 55 + short_control_width + 80, right_y - 2, 30, 18,
                               hwnd, (HMENU)(INT_PTR)IDC_RESOLUTION_LABEL, NULL, NULL);
    right_y += 20 + control_margin;

    // Max resolution combo
    state->hMaxResLabel = CreateWindowW(L"STATIC", L"Max Res:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, 60, 14, hwnd, NULL, NULL, NULL);
    state->hMaxResCombo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        right_x + 70, right_y - 2, short_control_width + 20, 110,
        hwnd, (HMENU)(INT_PTR)IDC_MAXRES_COMBO, NULL, NULL);
    SendMessageW(state->hMaxResCombo, CB_ADDSTRING, 0, (LPARAM)L"Off");
    SendMessageW(state->hMaxResCombo, CB_ADDSTRING, 0, (LPARAM)L"2160p");
    SendMessageW(state->hMaxResCombo, CB_ADDSTRING, 0, (LPARAM)L"1440p");
    SendMessageW(state->hMaxResCombo, CB_ADDSTRING, 0, (LPARAM)L"1080p");
    SendMessageW(state->hMaxResCombo, CB_SETCURSEL, 0, 0);
    right_y += 20 + control_margin;

    // Threads trackbar (compact)
    state->hThreadsTextLabel = CreateWindowW(L"STATIC", L"Threads:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                  right_x, right_y, 50, 14, hwnd, NULL, NULL, NULL);
    int max_threads = get_logical_core_count();
    state->hThreadTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                  WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                  right_x + 55, right_y - 2, short_control_width + 80, 18,
                                  hwnd, (HMENU)(INT_PTR)IDC_THREAD_TRACK, NULL, NULL);
    SendMessage(state->hThreadTrack, TBM_SETRANGE, TRUE, MAKELPARAM(1, max_threads));
    SendMessage(state->hThreadTrack, TBM_SETPOS, TRUE, state->num_threads);
    state->hThreadLabel = CreateWindowW(L"STATIC", L"4", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                               right_x + 55 + short_control_width + 80, right_y - 2, 30, 18,
                               hwnd, (HMENU)(INT_PTR)IDC_THREAD_LABEL, NULL, NULL);
    right_y += 20 + control_margin;

    // Preserve folder checkbox
    state->hPreserveFolderCheck = CreateWindowW(L"BUTTON", L"Output Full Folder Structure",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        right_x, right_y + 1, 170, 18,
        hwnd, (HMENU)(INT_PTR)IDC_PRESERVE_FOLDER_CHECK, NULL, NULL);
    right_y += 20 + control_margin;

    // Optimized button
    state->hOptimizedCheck = CreateWindowW(L"BUTTON", L"Optimized Settings",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        right_x , right_y + 1, 150, 24,
        hwnd, (HMENU)(INT_PTR)IDC_OPTIMIZED_CHECK, NULL, NULL);
    right_y += 24 + control_margin;

    // Logging checkbox
    state->hLoggingCheck = CreateWindowW(L"BUTTON", L"Enable logging",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        right_x, right_y + 1, 120, 18,
        hwnd, (HMENU)(INT_PTR)IDC_LOGGING_CHECK, NULL, NULL);
    right_y += 20 + control_margin;

    // Bench options


    // Convert button (large like C#)
    state->hProcessBtn = CreateWindowW(L"BUTTON", L"Convert",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER | BS_VCENTER | WS_TABSTOP,
        right_x, right_y, right_width, 34,
        hwnd, (HMENU)(INT_PTR)IDC_PROCESS_BTN, NULL, NULL);
    right_y += 40 + control_margin;

    // Cancel button
    state->hCancelBtn = CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        right_x + 184, right_y - 70, 110, 24,
        hwnd, (HMENU)(INT_PTR)IDC_CANCEL_BTN, NULL, NULL);
        right_y += 24 + control_margin;

    // Clear button (replaces Update button from C#)
    state->hClearBtn = CreateWindowW(L"BUTTON", L"Clear List",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        right_x + 184, right_y - 96, 110, 24,
        hwnd, (HMENU)(INT_PTR)IDC_CLEAR_BTN, NULL, NULL);

    // ===== PROGRESS BAR AT BOTTOM =====
    state->hProgressFiles = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0, height - progress_height, width, progress_height,
        hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_FILES, NULL, NULL);

    // Set fonts
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(hwnd, enum_child_proc, (LPARAM)hFont);

    // Make Convert button larger and bold
    LOGFONTW lf = {0};
   GetObject(hFont, sizeof(LOGFONTW), &lf);
    lf.lfHeight = -18;  // Larger font
    lf.lfWeight = FW_BOLD;
    HFONT hBoldFont = CreateFontIndirectW(&lf);
    SendMessage(state->hProcessBtn, WM_SETFONT, (WPARAM)hBoldFont, TRUE);

    // Apply settings to UI
    SetWindowTextW(state->hOutputDirEdit, state->output_dir);

    // Set format combo box based on loaded format
    int format_idx = 0;
    switch (state->output_format) {
        case FORMAT_WEBP: format_idx = 0; break;
        case FORMAT_JPEG: format_idx = 1; break;
        case FORMAT_PNG: format_idx = 2; break;
        case FORMAT_BMP: format_idx = 3; break;
        case FORMAT_TIFF: format_idx = 4; break;
        case FORMAT_ICO: format_idx = 5; break;
        default: format_idx = 0; break;
    }
    ComboBox_SetCurSel(state->hFormatCombo, format_idx);

    // Set slider positions and labels (with clamping for safety)
    SendMessage(state->hQualityTrack, TBM_SETPOS, TRUE, state->quality);
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", state->quality);
    SetWindowTextW(state->hQualityLabel, buf);

    // Clamp resolution to slider range (10-100%)
    if (state->resolution_percent < 10) state->resolution_percent = 10;
    if (state->resolution_percent > 100) state->resolution_percent = 100;
    SendMessage(state->hResolutionTrack, TBM_SETPOS, TRUE, state->resolution_percent);
    swprintf(buf, 32, L"%d", state->resolution_percent);
    SetWindowTextW(state->hResolutionLabel, buf);

    // Max resolution combo selection
    int max_res_idx = 0;
    switch (state->max_resolution) {
        case 2160: max_res_idx = 1; break;
        case 1440: max_res_idx = 2; break;
        case 1080: max_res_idx = 3; break;
        default: max_res_idx = 0; break;
    }
    ComboBox_SetCurSel(state->hMaxResCombo, max_res_idx);
    update_max_res_enable(state);

    // Clamp threads to slider range (1 to logical cores)
    int max_cores = get_logical_core_count();
    if (state->num_threads < 1) state->num_threads = 1;
    if (state->num_threads > max_cores) state->num_threads = max_cores;
    SendMessage(state->hThreadTrack, TBM_SETPOS, TRUE, state->num_threads);
    swprintf(buf, 32, L"%d", state->num_threads);
    SetWindowTextW(state->hThreadLabel, buf);

    // Set checkbox states
    Button_SetCheck(state->hPreserveFolderCheck, state->preserve_folder_structure ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(state->hLoggingCheck, state->enable_logging ? BST_CHECKED : BST_UNCHECKED);

    return TRUE;
}

void window_update_stats(AppState *state) {
    wchar_t buf[64];
    wchar_t size_buf[32];

    LONG processed = InterlockedCompareExchange(&state->files_processed, 0, 0);
    LONG64 total_input = InterlockedCompareExchange64(&state->total_input_size, 0, 0);
    LONG64 total_output = InterlockedCompareExchange64(&state->total_output_size, 0, 0);

    // When not processing, calculate total input size from files array
    if (!state->processing) {
        total_input = 0;
        EnterCriticalSection(&state->files_cs);
        for (int i = 0; i < state->file_count; i++) {
            total_input += (LONG64)state->files[i].input_size;
        }
        LeaveCriticalSection(&state->files_cs);
    }

    // Number of files and completed
    swprintf(buf, 64, L"%d", state->file_count);
    SetWindowTextW(state->hNumFilesLabel, buf);

    swprintf(buf, 64, L"%d", processed);
    SetWindowTextW(state->hCompletedLabel, buf);

    // Update Convert button text with percentage
    if (state->processing && state->file_count > 0) {
        int percent = (processed * 100) / state->file_count;
        // Clamp percentage to 0-100
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        wchar_t btn_buf[64];
        swprintf(btn_buf, 64, L"Convert %d%%", percent);
        SetWindowTextW(state->hProcessBtn, btn_buf);
    }

    // Before and after size
    format_size((size_t)total_input, size_buf, 32);
    SetWindowTextW(state->hBeforeSizeLabel, size_buf);

    format_size((size_t)total_output, size_buf, 32);
    SetWindowTextW(state->hAfterSizeLabel, size_buf);

    // Percent saved
    double percent_saved = total_input > 0 ?
        (1.0 - (double)total_output / (double)total_input) * 100.0 : 0.0;
    swprintf(buf, 64, L"%.2f%%", percent_saved);
    SetWindowTextW(state->hPercentSavedLabel, buf);

    // Progress bar (UI thread only)
    if (state->file_count > 0) {
        int pos = (processed > state->file_count) ? state->file_count : processed;
        LARGE_INTEGER ui0, ui1;
        QueryPerformanceCounter(&ui0);
        SendMessage(state->hProgressFiles, PBM_SETPOS, pos, 0);
        QueryPerformanceCounter(&ui1);
        if (state->frequency.QuadPart != 0) {
            state->total_ui_progress_ms += qpc_elapsed_ms(ui0, ui1, state->frequency);
            state->ui_progress_updates++;
        }
    }

    // Elapsed time
    size_t elapsed_ms = 0;
    if (state->frequency.QuadPart != 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        elapsed_ms = (now.QuadPart - state->start_time.QuadPart) * 1000 / state->frequency.QuadPart;
    }
    double elapsed_sec = elapsed_ms / 1000.0;
    swprintf(buf, 64, L"Elapsed: %.2f s", elapsed_sec);
    SetWindowTextW(state->hElapsedLabel, buf);

    // Files per second (average from start)
    if (elapsed_sec > 0.0) {
        state->files_per_sec = (double)processed / elapsed_sec;
    } else {
        state->files_per_sec = 0.0;
    }
    swprintf(buf, 64, L"%.2f files/s", state->files_per_sec);
    SetWindowTextW(state->hFilesPerSecLabel, buf);

    // MB per second using rolling 1-second window
    if (state->processing && state->frequency.QuadPart != 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        
        // Get the running total of input bytes processed
        LONG64 input_bytes_processed = InterlockedCompareExchange64(&state->total_input_bytes_processed, 0, 0);
        
        // Update display every 250ms
        double time_since_last_update_ms = qpc_elapsed_ms(state->speed_window.last_display_update, now, state->frequency);
        
        if (time_since_last_update_ms >= 250.0) {
            // Add current data point to circular buffer
            int pos = state->speed_window.pos;
            state->speed_window.timestamps[pos] = now;
            state->speed_window.bytes_processed[pos] = input_bytes_processed;
            state->speed_window.pos = (pos + 1) % 16;
            if (state->speed_window.count < 16) state->speed_window.count++;
            
            // Find bytes processed in last 1 second
            LARGE_INTEGER one_second_ago = {0};
            one_second_ago.QuadPart = now.QuadPart - state->frequency.QuadPart;
            
            LONG64 bytes_in_window = 0;
            LONG64 earliest_bytes = input_bytes_processed;
            
            for (int i = 0; i < state->speed_window.count; i++) {
                int idx = (state->speed_window.pos - 1 - i + 16) % 16;
                if (state->speed_window.timestamps[idx].QuadPart >= one_second_ago.QuadPart) {
                    // This sample is within the 1-second window
                    // Keep the earliest bytes in window to calculate delta
                    if (state->speed_window.bytes_processed[idx] < earliest_bytes) {
                        earliest_bytes = state->speed_window.bytes_processed[idx];
                    }
                } else {
                    // This sample is outside the 1-second window
                    break;
                }
            }
            
            bytes_in_window = input_bytes_processed - earliest_bytes;
            
            // Calculate MB/s based on bytes in last 1 second
            if (bytes_in_window > 0) {
                state->mb_per_sec = (double)bytes_in_window / (1024.0 * 1024.0);
            } else {
                state->mb_per_sec = 0.0;
            }
            
            state->speed_window.last_display_update = now;
        }
    }

    // MB per second (preserve final value after processing completes)
    swprintf(buf, 64, L"%.2f MB/s", state->mb_per_sec);
    SetWindowTextW(state->hMbPerSecLabel, buf);
}

void window_set_processing_state(AppState *state, BOOL processing) {
    EnableWindow(state->hProcessBtn, !processing);
    EnableWindow(state->hClearBtn, !processing);
    EnableWindow(state->hOutputDirEdit, !processing);
    EnableWindow(state->hBrowseBtn, !processing);
    EnableWindow(state->hFormatCombo, !processing);
    EnableWindow(state->hQualityTrack, !processing);
    EnableWindow(state->hResolutionTrack, !processing);
    EnableWindow(state->hMaxResCombo, !processing);
    EnableWindow(state->hThreadTrack, !processing);
    EnableWindow(state->hOptimizedCheck, !processing);
    EnableWindow(state->hPreserveFolderCheck, !processing);
    EnableWindow(state->hLoggingCheck, !processing);
    if (!processing) {
        update_jpeg_fast_enable(state);
        // Reset button text when not processing
        SetWindowTextW(state->hProcessBtn, L"Convert");
    } else {
        // Reset speed window when processing starts
        memset(&state->speed_window, 0, sizeof(state->speed_window));
        state->mb_per_sec = 0.0;
        if (state->frequency.QuadPart != 0) {
            QueryPerformanceCounter(&state->speed_window.last_display_update);
        }
        // Initialize button text with 0%
        SetWindowTextW(state->hProcessBtn, L"Convert 0%");
    }

    state->processing = processing;
}

static LRESULT handle_command(AppState *state, WORD cmd, HWND hwnd) {
    switch (cmd) {
        case IDC_BROWSE_BTN: {
            wchar_t path[MAX_PATH_LEN];
            BROWSEINFOW bi = {0};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = path;
            bi.lpszTitle = L"Select Output Directory";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                SHGetPathFromIDListW(pidl, path);
                SetWindowTextW(state->hOutputDirEdit, path);
                CoTaskMemFree(pidl);
            }
            break;
        }

        case IDC_PROCESS_BTN:
            if (!state->processing && state->file_count > 0) {
                processing_start(state);
            }
            break;

        case IDC_CLEAR_BTN:
            if (!state->processing) {
                // Clear file list
                state->file_count = 0;
                ListView_DeleteAllItems(state->hFilesList);

                // Reset stats labels
                SetWindowTextW(state->hNumFilesLabel, L"0");
                SetWindowTextW(state->hCompletedLabel, L"0");
                SetWindowTextW(state->hBeforeSizeLabel, L"0");
                SetWindowTextW(state->hAfterSizeLabel, L"0");
                SetWindowTextW(state->hPercentSavedLabel, L"0.00%");
                SetWindowTextW(state->hElapsedLabel, L"Elapsed: 00:00:00");
                SetWindowTextW(state->hFilesPerSecLabel, L"0.00 files/s");
                SetWindowTextW(state->hMbPerSecLabel, L"0.00 MB/s");
            }
            break;

        case IDC_CANCEL_BTN:
            if (state->scanning) {
                dragdrop_cancel_scan(state);
            } else if (state->processing) {
                processing_stop(state);
            }
            break;

        case IDC_OPTIMIZED_CHECK:
            if (!state->processing) {
                ComboBox_SetCurSel(state->hFormatCombo, 0); // WebP
                SendMessage(state->hQualityTrack, TBM_SETPOS, TRUE, 85);
                SetWindowTextW(state->hQualityLabel, L"85");
                SendMessage(state->hResolutionTrack, TBM_SETPOS, TRUE, 100);
                SetWindowTextW(state->hResolutionLabel, L"100");
                ComboBox_SetCurSel(state->hMaxResCombo, 1); // 2160p
                update_max_res_enable(state);
                sync_max_res_from_ui(state);
            }
            break;

        case IDC_LOGGING_CHECK:
            state->enable_logging = Button_GetCheck(state->hLoggingCheck) == BST_CHECKED;
            break;

        case ID_HELP_CHECK_UPDATES:
            update_check(hwnd);
            break;

        case ID_HELP_ABOUT: {
            wchar_t msg[256];
            swprintf(msg, 256, L"%s %s\n%s", APP_NAME, APP_VERSION, APP_AUTHOR);
            MessageBoxW(hwnd, msg, L"About ResizerC", MB_OK | MB_ICONINFORMATION);
            break;
        }

        case IDC_MAXRES_COMBO:
            sync_max_res_from_ui(state);
            update_max_res_enable(state);
            break;

        case IDC_QUALITY_TRACK:
        case IDC_RESOLUTION_TRACK:
        case IDC_THREAD_TRACK: {
            HWND hTrack = GetDlgItem(hwnd, cmd);
            HWND hLabel = (cmd == IDC_QUALITY_TRACK) ? state->hQualityLabel :
                         (cmd == IDC_RESOLUTION_TRACK) ? state->hResolutionLabel :
                         state->hThreadLabel;
            int pos = (int)SendMessage(hTrack, TBM_GETPOS, 0, 0);
            wchar_t buf[32];
            swprintf(buf, 32, L"%d", pos);
            SetWindowTextW(hLabel, buf);
            break;
        }

        default:
            break;
    }
    return 0;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *state = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW *cs = (CREATESTRUCTW*)lparam;
            state = (AppState*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
            state->hwnd = hwnd;

            if (!window_init_controls(hwnd, state)) {
                return -1;
            }

            // Initialize drag-drop
            dragdrop_init(hwnd);

            // Initialize speed window
            memset(&state->speed_window, 0, sizeof(state->speed_window));
            QueryPerformanceCounter(&state->speed_window.last_display_update);

            // Start stats timer
            SetTimer(hwnd, TIMER_STATS, 100, NULL);

            // Set accept files
            ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, NULL);

            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wparam) == IDC_FORMAT_COMBO && HIWORD(wparam) == CBN_SELCHANGE) {
                update_jpeg_fast_enable(state);
            } else if (LOWORD(wparam) == IDC_MAXRES_COMBO && HIWORD(wparam) == CBN_SELCHANGE) {
                update_max_res_enable(state);
            }
            return handle_command(state, LOWORD(wparam), hwnd);

        case WM_TIMER:
            if (wparam == TIMER_STATS) {
                if (state->processing) {
                    window_update_stats(state);
                    // Check if processing is complete
                    if (processing_is_complete(state)) {
                        processing_stop(state);
                        // Final stats update
                        window_update_stats(state);
                        // Update tab labels to show failed file count
                        dragdrop_populate_listview(state);
                    }
                }
            }
            break;

        case WM_NOTIFY: {
            LPNMHDR nmhdr = (LPNMHDR)lparam;
            if (nmhdr->hwndFrom == state->hTabControl && nmhdr->code == TCN_SELCHANGE) {
                // Tab changed - update current_tab and refresh listview
                int new_tab = (int)SendMessageW(state->hTabControl, TCM_GETCURSEL, 0, 0);
                if (new_tab != state->current_tab) {
                    state->current_tab = new_tab;
                    dragdrop_populate_listview(state);
                }
            } else if (nmhdr->hwndFrom == state->hFilesList && nmhdr->code == NM_DBLCLK) {
                // Double-click on file list - open file in default Windows application
                NMITEMACTIVATE *nmia = (NMITEMACTIVATE*)lparam;
                int listview_index = nmia->iItem;
                
                if (listview_index >= 0) {
                    // Map ListView index to file path by iterating through files with same filter logic
                    const wchar_t *file_path = NULL;
                    int current_display_index = 0;
                    
                    EnterCriticalSection(&state->files_cs);
                    for (int i = 0; i < state->file_count; i++) {
                        // Apply same filter logic as dragdrop_populate_listview()
                        if (state->current_tab == 1) {  // Failed Files tab
                            if (!state->files[i].processed || state->files[i].success) {
                                continue;
                            }
                        }
                        
                        // Found the matching ListView index
                        if (current_display_index == listview_index) {
                            file_path = state->files[i].path;
                            break;
                        }
                        current_display_index++;
                    }
                    
                    if (file_path) {
                        // Open file with default Windows application
                        ShellExecuteW(NULL, L"open", file_path, NULL, NULL, SW_SHOW);
                    }
                    LeaveCriticalSection(&state->files_cs);
                }
            }
            return 0;
        }

        case WM_DROPFILES:
            dragdrop_handle_drop(state, (HDROP)wparam);
            return 0;

        case WM_APP_SCANNING_PROGRESS: {
            // Update progress bar during scanning
            int files_found = (int)wparam;
            wchar_t buf[64];
            swprintf(buf, 64, L"Scanning: %d files...", files_found);
            SetWindowTextW(state->hProgressFiles, buf);
            return 0;
        }

        case WM_APP_SCANNING_COMPLETE: {
            // Scanning complete
            
            // Stop scanning state
            state->scanning = FALSE;
            SetWindowTextW(state->hCancelBtn, L"Cancel");
            
            // Populate ListView with all files
            dragdrop_populate_listview(state);
            
            // Update UI to show file count and calculate total size from files
            window_update_stats(state);
            
            // Reset progress bar text and position
            SetWindowTextW(state->hProgressFiles, L"");
            SendMessageW(state->hProgressFiles, PBM_SETPOS, 0, 0);
            
            return 0;
        }

        case WM_HSCROLL: {
            HWND hTrack = (HWND)lparam;
            int pos = (int)SendMessage(hTrack, TBM_GETPOS, 0, 0);
            wchar_t buf[32];
            swprintf(buf, 32, L"%d", pos);

            if (hTrack == state->hQualityTrack) {
                SetWindowTextW(state->hQualityLabel, buf);
            } else if (hTrack == state->hResolutionTrack) {
                SetWindowTextW(state->hResolutionLabel, buf);
            } else if (hTrack == state->hThreadTrack) {
                SetWindowTextW(state->hThreadLabel, buf);
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO*)lparam;
            mmi->ptMinTrackSize.x = MIN_WINDOW_WIDTH;
            mmi->ptMinTrackSize.y = MIN_WINDOW_HEIGHT;
            return 0;
        }

        case WM_SIZE:
            if (state) {
                int width = LOWORD(lparam);
                int height = HIWORD(lparam);
                window_layout_controls(hwnd, state, width, height);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_STATS);
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    return 0;
}

HWND window_create(HINSTANCE hinstance, AppState *state) {
    // Register window class
    static WNDCLASSEXW wc = {0};
    static BOOL registered = FALSE;

    if (!registered) {
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = window_proc;
        wc.hInstance = hinstance;
        wc.hIcon = LoadIconW(hinstance, MAKEINTRESOURCEW(IDI_ICON));
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ResizerCWindow";
        wc.hIconSm = LoadIconW(hinstance, MAKEINTRESOURCEW(IDI_ICON_SMALL));

        if (!RegisterClassExW(&wc)) {
            return NULL;
        }
        registered = TRUE;
    }

    // Create window with AppState as lpParam
    HWND hwnd = CreateWindowExW(
        0,
        L"ResizerCWindow",
        APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, hinstance, state
    );

    if (hwnd) {
        window_init_menu(hwnd);
    }

    return hwnd;
}
