#ifndef UTILS_H
#define UTILS_H

#include "common.h"

// Format utilities
const wchar_t* format_to_wstring(ImageFormat format);
const char* format_to_extension(ImageFormat format);
const wchar_t* format_to_extension_w(ImageFormat format);
ImageFormat detect_format_from_path(const wchar_t *path);
BOOL is_format_supported(ImageFormat format);

// String utilities
void format_size(size_t size, wchar_t *buf, size_t bufsize);
void format_time(double seconds, wchar_t *buf, size_t bufsize);

// System utilities
int get_logical_core_count(void);

// File utilities
BOOL file_exists(const wchar_t *path);
BOOL create_directory_recursive(const wchar_t *path);
void get_file_extension(const wchar_t *path, wchar_t *ext, size_t ext_size);
void replace_extension(const wchar_t *path, const wchar_t *new_ext, wchar_t *new_path, size_t new_path_size);

// EXIF utilities
BOOL get_exif_date_taken(const wchar_t *path, FILETIME *ft);

// Error utilities
const wchar_t* get_error_message(DWORD error);

// Logging utilities
typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_INFO = 2
} LogLevel;

void log_init(void);
void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_message(const wchar_t *format, ...);
void log_error(const wchar_t *format, ...);
void log_close(void);

#endif // UTILS_H
