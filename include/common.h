#ifndef COMMON_H
#define COMMON_H

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATIONS

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>

#ifdef HAVE_JPEG
#include <jpeglib.h>
#ifdef HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif
#endif

#ifdef HAVE_PNG
#include <png.h>
#endif

#ifdef HAVE_WEBP
#include <webp/encode.h>
#include <webp/decode.h>
#include <webp/mux.h>
#endif

#ifdef HAVE_TIFF
#include <tiffio.h>
#endif

#ifdef HAVE_VIPS
#include <vips/vips.h>
#endif

#include <zlib.h>

// Windows control macros
#ifndef ComboBox_AddString
#define ComboBox_AddString(hwndCtl, lpsz) \
    ((int)(DWORD)SendMessage((hwndCtl), CB_ADDSTRING, 0, (LPARAM)(lpsz)))
#endif
#ifndef ComboBox_SetCurSel
#define ComboBox_SetCurSel(hwndCtl, index) \
    ((int)(DWORD)SendMessage((hwndCtl), CB_SETCURSEL, (WPARAM)(index), 0L))
#endif
#ifndef ComboBox_GetCurSel
#define ComboBox_GetCurSel(hwndCtl) \
    ((int)(DWORD)SendMessage((hwndCtl), CB_GETCURSEL, 0L, 0L))
#endif
#ifndef Button_GetCheck
#define Button_GetCheck(hwndCtl) \
    ((int)(DWORD)SendMessage((hwndCtl), BM_GETCHECK, 0L, 0L))
#endif
#ifndef Button_SetCheck
#define Button_SetCheck(hwndCtl, check) \
    ((void)(DWORD)SendMessage((hwndCtl), BM_SETCHECK, (WPARAM)(check), 0L))
#endif
#ifndef ListView_InsertColumn
#define ListView_InsertColumn(hwnd, iCol, pcol) \
    ((int)(DWORD)SendMessage((hwnd), LVM_INSERTCOLUMN, (WPARAM)(int)(iCol), (LPARAM)(const LV_COLUMN *)(pcol)))
#endif

// Application information
#define WIDEN_LITERAL2(x) L##x
#define WIDEN_LITERAL(x) WIDEN_LITERAL2(x)

#ifndef APP_VERSION_TEXT
#define APP_VERSION_TEXT "1.0.0"
#endif

#define APP_NAME L"ResizerC"
#define APP_VERSION WIDEN_LITERAL(APP_VERSION_TEXT)
#define APP_AUTHOR L"ResizerC Team"

// Window message IDs for background operations
#define WM_APP_SCANNING_PROGRESS (WM_APP + 1)
#define WM_APP_SCANNING_COMPLETE (WM_APP + 2)

// Window dimensions
#define MIN_WINDOW_WIDTH 700
#define MIN_WINDOW_HEIGHT 480
#define WINDOW_WIDTH 850
#define WINDOW_HEIGHT 480

// Maximum paths and buffers
#define MAX_PATH_LEN 32768
#define MAX_FILES 10000
#define MAX_THREADS 64

// Supported image formats
typedef enum {
    FORMAT_JPEG,
    FORMAT_PNG,
    FORMAT_WEBP,
    FORMAT_BMP,
    FORMAT_TIFF,
    FORMAT_GIF,
    FORMAT_ICO,
    FORMAT_UNKNOWN
} ImageFormat;

// Image file structure
typedef struct {
    wchar_t path[MAX_PATH_LEN];
    wchar_t output_path[MAX_PATH_LEN];
    wchar_t root_folder[MAX_PATH_LEN];  // Root folder when dropped (for preserving folder structure)
    ImageFormat format;
    size_t input_size;
    size_t output_size;
    BOOL processed;
    BOOL success;
    DWORD error;
    wchar_t error_message[256];  // Full error message
} ImageFile;

// Hash table entry for duplicate detection
typedef struct HashEntry {
    uint32_t hash;
    struct HashEntry *next;
} HashEntry;

// Application state
typedef struct {
    HWND hwnd;
    HINSTANCE hinstance;

    // UI controls
    HWND hDropTarget;
    HWND hFilesList;
    HWND hOutputDirLabel;
    HWND hOutputDirEdit;
    HWND hBrowseBtn;
    HWND hFormatLabel;
    HWND hFormatCombo;
    HWND hQualityTextLabel;
    HWND hQualityTrack;
    HWND hQualityLabel;
    HWND hResolutionTextLabel;
    HWND hResolutionTrack;
    HWND hResolutionLabel;
    HWND hMaxResLabel;
    HWND hMaxResCombo;
    HWND hThreadsTextLabel;
    HWND hThreadTrack;
    HWND hThreadLabel;
    HWND hProcessBtn;
    HWND hClearBtn;
    HWND hPreserveFolderCheck;
    HWND hOptimizedCheck;
    HWND hLoggingCheck;

    // Tab control for filtering files
    HWND hTabControl;
    int current_tab;  // 0=All Files, 1=Failed Files
    
    // Progress info
    HWND hProgressFiles;
    HWND hProgressTotal;

    // Statistics labels
    HWND hNumFilesLabel;
    HWND hCompletedLabel;
    HWND hBeforeSizeLabel;
    HWND hAfterSizeLabel;
    HWND hPercentSavedLabel;
    HWND hNumFilesTextLabel;
    HWND hCompletedTextLabel;
    HWND hBeforeSizeTextLabel;
    HWND hAfterSizeTextLabel;
    HWND hSavedTextLabel;
    HWND hElapsedLabel;
    HWND hFilesPerSecLabel;
    HWND hMbPerSecLabel;
    HWND hStatsLabel;

    // Cancel button
    HWND hCancelBtn;

    // File list
    ImageFile *files;
    int file_count;
    int file_capacity;
    CRITICAL_SECTION files_cs;

    // Hash table for duplicate detection (for drag-drop scanning)
    // Proper hash table with O(1) lookup using buckets
    HashEntry **hash_buckets;
    int hash_bucket_count;
    int hash_bucket_mask;  // bucket_count - 1 for modulo optimization
    CRITICAL_SECTION hash_cs;  // Separate lock for hash table
    
    volatile BOOL scanning;
    volatile BOOL stop_scanning;
    HANDLE scan_thread;

    // Settings
    wchar_t output_dir[MAX_PATH_LEN];
    ImageFormat output_format;
    int quality;
    int resolution_percent;
    int max_resolution;
    int num_threads;
    BOOL preserve_folder_structure;
    BOOL enable_logging;
    int webp_method;  // WebP compression method (0-6, where 0=fastest, 6=slowest/best)

    // Processing
    BOOL processing;
    BOOL stop_processing;
    HANDLE *threads;
    int active_threads;

    // Statistics
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LONG files_processed;
    LONG64 total_input_size;
    LONG64 total_output_size;
    LONG64 total_input_bytes_processed;  // Running total of input bytes processed for MB/s calculation
    LONG64 total_load_us;
    LONG64 total_resize_us;
    LONG64 total_save_us;
    LONG files_loaded;
    LONG files_resized;
    LONG files_saved;
    LONG fastpath_jpeg_to_jpeg_attempts;
    LONG fastpath_jpeg_to_jpeg_hits;
    LONG fastpath_jpeg_to_webp_attempts;
    LONG fastpath_jpeg_to_webp_hits;
    LONG jpeg_subsamp_444;
    LONG jpeg_subsamp_422;
    LONG jpeg_subsamp_420;
    LONG jpeg_subsamp_gray;
    LONG jpeg_subsamp_other;
    LONG jpeg_colorspace_gray;
    LONG jpeg_colorspace_ycbcr;
    LONG jpeg_colorspace_rgb;
    LONG jpeg_colorspace_cmyk;
    LONG jpeg_colorspace_other;
    LONG jpeg_yuv_decode_fail;
    LONG jpeg_info_fail;
    LONG fastpath_jpeg_to_jpeg_decode_fail;
    LONG fastpath_jpeg_to_jpeg_encode_fail;
    LONG fastpath_jpeg_to_webp_decode_fail;
    LONG fastpath_jpeg_to_webp_encode_fail;
    double mb_per_sec;
    double files_per_sec;
    double total_ui_progress_ms;
    int ui_progress_updates;

    // Rolling window for instantaneous MB/s calculation (1-second window, updated every 250ms)
    struct {
        LARGE_INTEGER timestamps[16];  // Circular buffer of timestamps
        LONG64 bytes_processed[16];    // Cumulative bytes at each timestamp
        int pos;                       // Current position in circular buffer
        int count;                     // Number of entries in buffer
        LARGE_INTEGER last_display_update;  // Last time MB/s display was updated
    } speed_window;

} AppState;

// Format functions
const wchar_t* format_to_wstring(ImageFormat format);
const char* format_to_extension(ImageFormat format);
ImageFormat detect_format_from_path(const wchar_t *path);
BOOL is_format_supported(ImageFormat format);

// String utilities
void format_size(size_t size, wchar_t *buf, size_t bufsize);
void format_time(double seconds, wchar_t *buf, size_t bufsize);
int get_logical_core_count(void);

#endif // COMMON_H
