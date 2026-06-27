#include "utils.h"
#include <stdarg.h>
#include <time.h>

const wchar_t* format_to_wstring(ImageFormat format) {
    switch (format) {
        case FORMAT_JPEG: return L"JPEG";
        case FORMAT_PNG:  return L"PNG";
        case FORMAT_WEBP: return L"WebP";
        case FORMAT_BMP:  return L"BMP";
        case FORMAT_TIFF: return L"TIFF";
        case FORMAT_GIF:  return L"GIF";
        case FORMAT_ICO:  return L"ICO";
        default:          return L"Unknown";
    }
}

const char* format_to_extension(ImageFormat format) {
    switch (format) {
        case FORMAT_JPEG: return ".jpg";
        case FORMAT_PNG:  return ".png";
        case FORMAT_WEBP: return ".webp";
        case FORMAT_BMP:  return ".bmp";
        case FORMAT_TIFF: return ".tif";
        case FORMAT_GIF:  return ".gif";
        case FORMAT_ICO:  return ".ico";
        default:          return ".bin";
    }
}

const wchar_t* format_to_extension_w(ImageFormat format) {
    switch (format) {
        case FORMAT_JPEG: return L".jpg";
        case FORMAT_PNG:  return L".png";
        case FORMAT_WEBP: return L".webp";
        case FORMAT_BMP:  return L".bmp";
        case FORMAT_TIFF: return L".tif";
        case FORMAT_GIF:  return L".gif";
        case FORMAT_ICO:  return L".ico";
        default:          return L".bin";
    }
}

ImageFormat detect_format_from_path(const wchar_t *path) {
    wchar_t ext[32] = {0};
    get_file_extension(path, ext, sizeof(ext)/sizeof(wchar_t));

    // Convert to lowercase for comparison
    for (size_t i = 0; ext[i]; i++) {
        if (ext[i] >= L'A' && ext[i] <= L'Z') {
            ext[i] += 32;
        }
    }

    if (wcscmp(ext, L".jpg") == 0 || wcscmp(ext, L".jpeg") == 0) return FORMAT_JPEG;
    if (wcscmp(ext, L".png") == 0) return FORMAT_PNG;
    if (wcscmp(ext, L".webp") == 0) return FORMAT_WEBP;
    if (wcscmp(ext, L".bmp") == 0) return FORMAT_BMP;
    if (wcscmp(ext, L".tif") == 0 || wcscmp(ext, L".tiff") == 0) return FORMAT_TIFF;
    if (wcscmp(ext, L".gif") == 0) return FORMAT_GIF;
    if (wcscmp(ext, L".ico") == 0) return FORMAT_ICO;

    return FORMAT_UNKNOWN;
}

BOOL is_format_supported(ImageFormat format) {
    switch (format) {
        case FORMAT_JPEG:
        #ifdef HAVE_JPEG
            return TRUE;
        #else
            return FALSE;
        #endif

        case FORMAT_PNG:
        #ifdef HAVE_PNG
            return TRUE;
        #else
            return FALSE;
        #endif

        case FORMAT_WEBP:
        #ifdef HAVE_WEBP
            return TRUE;
        #else
            return FALSE;
        #endif

        case FORMAT_BMP:
        case FORMAT_ICO:
            return TRUE; // Built-in support

        case FORMAT_TIFF:
        #ifdef HAVE_TIFF
            return TRUE;
        #else
            return FALSE;
        #endif

        case FORMAT_GIF:
            return FALSE; // GIF read-only for now

        default:
            return FALSE;
    }
}

void format_size(size_t size, wchar_t *buf, size_t bufsize) {
    const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    int unit_index = 0;
    double size_d = (double)size;

    while (size_d >= 1024.0 && unit_index < 4) {
        size_d /= 1024.0;
        unit_index++;
    }

    swprintf(buf, bufsize, L"%.2f %s", size_d, units[unit_index]);
}

void format_time(double seconds, wchar_t *buf, size_t bufsize) {
    int hours = (int)(seconds / 3600);
    int minutes = (int)((seconds - hours * 3600) / 60);
    double secs = seconds - hours * 3600 - minutes * 60;

    if (hours > 0) {
        swprintf(buf, bufsize, L"%dh %dm %.2fs", hours, minutes, secs);
    } else if (minutes > 0) {
        swprintf(buf, bufsize, L"%dm %.2fs", minutes, secs);
    } else {
        swprintf(buf, bufsize, L"%.2fs", secs);
    }
}

int get_logical_core_count(void) {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
}

BOOL file_exists(const wchar_t *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

BOOL create_directory_recursive(const wchar_t *path) {
    wchar_t temp[MAX_PATH_LEN];
    wcscpy(temp, path);

    // Remove trailing slash if present
    size_t len = wcslen(temp);
    if (len > 0 && (temp[len-1] == L'\\' || temp[len-1] == L'/')) {
        temp[len-1] = L'\0';
    }

    if (file_exists(temp)) {
        return TRUE;
    }

    // Find parent directory
    wchar_t *slash = wcsrchr(temp, L'\\');
    if (!slash) slash = wcsrchr(temp, L'/');

    if (slash) {
        *slash = L'\0';
        if (!create_directory_recursive(temp)) {
            return FALSE;
        }
        *slash = L'\\';
    }

    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void get_file_extension(const wchar_t *path, wchar_t *ext, size_t ext_size) {
    const wchar_t *dot = wcsrchr(path, L'.');
    if (dot) {
        wcsncpy(ext, dot, ext_size - 1);
        ext[ext_size - 1] = L'\0';
    } else {
        ext[0] = L'\0';
    }
}

void replace_extension(const wchar_t *path, const wchar_t *new_ext, wchar_t *new_path, size_t new_path_size) {
    wcsncpy(new_path, path, new_path_size - 1);
    new_path[new_path_size - 1] = L'\0';

    wchar_t *dot = wcsrchr(new_path, L'.');
    if (dot) {
        wcscpy(dot, new_ext);
    } else {
        wcscat(new_path, new_ext);
    }
}

// Error codes
#define ERROR_CORRUPTED_FILE   0x01
#define ERROR_INVALID_FORMAT   0x02
#define ERROR_LOAD_FAILED      0x03
#define ERROR_SAVE_FAILED      0x04
#define ERROR_DECODE_FAILED    0x05

const wchar_t* get_error_message(DWORD error) {
    switch (error) {
        case ERROR_CORRUPTED_FILE:
            return L"corrupted file";
        case ERROR_INVALID_FORMAT:
            return L"invalid format";
        case ERROR_LOAD_FAILED:
            return L"load failed";
        case ERROR_SAVE_FAILED:
            return L"save failed";
        case ERROR_DECODE_FAILED:
            return L"decode failed";
        default:
            return L"error";
    }
}

// Simple EXIF date reading (basic implementation)
BOOL get_exif_date_taken(const wchar_t *path, FILETIME *ft) {
    // This is a simplified implementation
    // A full implementation would parse EXIF data from JPEG/PNG files
    // For now, just return FALSE to use file modification time
    return FALSE;
}

// Logging implementation
static CRITICAL_SECTION g_log_cs;
static FILE *g_log_file = NULL;
static wchar_t g_log_path[MAX_PATH_LEN];
static volatile LogLevel g_log_level = LOG_LEVEL_INFO;
static BOOL g_log_initialized = FALSE;

void log_init(void) {
    if (g_log_initialized) return;
    InitializeCriticalSection(&g_log_cs);

    // Get executable directory
    GetModuleFileNameW(NULL, g_log_path, MAX_PATH_LEN);
    wchar_t *slash = wcsrchr(g_log_path, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    wcscat(g_log_path, L"\\resizerc_debug.log");

    g_log_file = _wfopen(g_log_path, L"w, ccs=UTF-8");
    if (g_log_file) {
        fwprintf(g_log_file, L"=== ResizerC Debug Log ===\n");
        fflush(g_log_file);
    }
    g_log_initialized = TRUE;
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

LogLevel log_get_level(void) {
    return g_log_level;
}

void log_message(const wchar_t *format, ...) {
    if (!g_log_file || g_log_level < LOG_LEVEL_INFO) return;

    EnterCriticalSection(&g_log_cs);

    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(g_log_file, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfwprintf(g_log_file, format, args);
    va_end(args);

    fwprintf(g_log_file, L"\n");
    fflush(g_log_file);

    LeaveCriticalSection(&g_log_cs);
}

void log_error(const wchar_t *format, ...) {
    if (!g_log_file || g_log_level < LOG_LEVEL_ERROR) return;

    EnterCriticalSection(&g_log_cs);

    // Get timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(g_log_file, L"[%02d:%02d:%02d.%03d] ERROR: ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfwprintf(g_log_file, format, args);
    va_end(args);

    fwprintf(g_log_file, L"\n");
    fflush(g_log_file);

    LeaveCriticalSection(&g_log_cs);
}

void log_close(void) {
    if (!g_log_initialized) return;
    if (g_log_file) {
        fwprintf(g_log_file, L"=== Log End ===\n");
        fclose(g_log_file);
        g_log_file = NULL;
    }
    DeleteCriticalSection(&g_log_cs);
    g_log_initialized = FALSE;
}
