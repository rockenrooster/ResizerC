#include "processing.h"
#include "window.h"
#include "settings.h"
#include "resource.h"
#include "utils.h"
#include "image.h"
#include <shlwapi.h>

// Map Windows error codes to our custom error codes
static DWORD map_windows_error(DWORD win_error) {
    // Handle common Windows errors for image files
    switch (win_error) {
        case ERROR_INVALID_DATA:
        case ERROR_BAD_FORMAT:
        case ERROR_CRC:
        case ERROR_FILE_CORRUPT:
            return 0x01; // ERROR_CORRUPTED_FILE
        
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_INVALID_DRIVE:
            return 0x02; // ERROR_INVALID_FORMAT
        
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return 0x03; // ERROR_LOAD_FAILED
        
        case ERROR_DISK_FULL:
        case ERROR_WRITE_FAULT:
            return 0x04; // ERROR_SAVE_FAILED
        
        default:
            // For other errors, use 0x05 (decode failed) or 0 for generic error
            if (win_error != 0) {
                return 0x05; // ERROR_DECODE_FAILED
            }
            return 0; // No specific error
    }
}

static LogLevel g_prev_log_level = LOG_LEVEL_INFO;
static BOOL g_log_level_overridden = FALSE;

#define IO_THREAD_COUNT 2

typedef struct {
    int index;
    unsigned char *data;
    size_t size;
    HANDLE hFile;
    HANDLE hMap;
    BOOL mapped;
} IoJob;

typedef struct {
    IoJob *items;
    int capacity;
    int head;
    int tail;
    int count;
    BOOL done;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
} IoQueue;

typedef struct {
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
    LONG64 total_input_size;  
    LONG64 total_output_size;  
    


} ThreadStats;

static IoQueue g_io_queue = {0};
static HANDLE g_io_threads[IO_THREAD_COUNT] = {0};
static LONG g_next_io_index = 0;
static LONG g_active_io_threads = 0;

static BOOL io_queue_push(IoQueue *q, const IoJob *job, BOOL *stop_flag);
static BOOL io_queue_pop(IoQueue *q, IoJob *out_job, BOOL *stop_flag);
static void io_job_cleanup(IoJob *job);
static BOOL io_job_prepare_mmap(const wchar_t *path, IoJob *job);
static void merge_thread_stats(AppState *state, const ThreadStats *stats);

static DWORD WINAPI io_reader_thread(LPVOID param) {
    AppState *state = (AppState*)param;
    for (;;) {
        if (state->stop_processing) {
            break;
        }

        LONG index = InterlockedIncrement(&g_next_io_index) - 1;
        if (index >= state->file_count) {
            break;
        }

        IoJob job = {0};
        job.index = (int)index;

#if defined(HAVE_JPEG)
        if (state->files[index].format == FORMAT_JPEG) {
            io_job_prepare_mmap(state->files[index].path, &job);
        }
#endif

        if (!io_queue_push(&g_io_queue, &job, &state->stop_processing)) {
            io_job_cleanup(&job);
            break;
        }
    }

    if (InterlockedDecrement(&g_active_io_threads) == 0) {
        EnterCriticalSection(&g_io_queue.cs);
        g_io_queue.done = TRUE;
        WakeAllConditionVariable(&g_io_queue.not_empty);
        LeaveCriticalSection(&g_io_queue.cs);
    }

    return 0;
}

static void generate_output_path(AppState *state, ImageFile *file) {
    if (state->preserve_folder_structure) {
        const wchar_t *full_path = file->path;
        const wchar_t *relative_path = full_path;
        wchar_t prefix[MAX_PATH_LEN] = L"";

        if (full_path[0] && full_path[1] == L':') {
            // Drive letter path: C:\...
            swprintf(prefix, MAX_PATH_LEN, L"%c", towupper(full_path[0]));
            relative_path = full_path + 2;
            if (*relative_path == L'\\' || *relative_path == L'/') {
                relative_path++;
            }
        } else if (full_path[0] == L'\\' && full_path[1] == L'\\') {
            // UNC path: \\server\share\...
            const wchar_t *p = full_path + 2;
            const wchar_t *slash = wcschr(p, L'\\');
            if (slash) {
                size_t server_len = (size_t)(slash - p);
                const wchar_t *share = slash + 1;
                const wchar_t *slash2 = wcschr(share, L'\\');
                size_t share_len = slash2 ? (size_t)(slash2 - share) : wcslen(share);
                swprintf(prefix, MAX_PATH_LEN, L"%.*s\\%.*s",
                         (int)server_len, p,
                         (int)share_len, share);
                relative_path = slash2 ? slash2 + 1 : share + share_len;
            }
        }

        if (prefix[0] != L'\0') {
            swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s\\%s",
                     state->output_dir, prefix, relative_path);
        } else {
            swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s",
                     state->output_dir, relative_path);
        }

        // Replace extension
        wchar_t *last_slash = wcsrchr(file->output_path, L'\\');
        if (last_slash) {
            wchar_t new_name[MAX_PATH_LEN];
            const wchar_t *old_name = last_slash + 1;
            replace_extension(old_name, format_to_extension_w(state->output_format), new_name, MAX_PATH_LEN);
            wchar_t dir_buf[MAX_PATH_LEN];
            wcsncpy(dir_buf, file->output_path, (size_t)(last_slash - file->output_path));
            dir_buf[(size_t)(last_slash - file->output_path)] = L'\0';
            swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s",
                     dir_buf,
                     new_name);
        }
        return;
    }

    if (file->root_folder[0] != L'\0') {
        // Preserve folder structure relative to the root folder
        // Get relative path from root folder to file
        size_t root_len = wcslen(file->root_folder);

        // The file path should start with the root folder
        if (_wcsnicmp(file->path, file->root_folder, root_len) == 0) {
            // File is under root folder, get the relative part
            const wchar_t *relative_path = file->path + root_len;
            // Skip leading backslash if present
            if (*relative_path == L'\\' || *relative_path == L'/') {
                relative_path++;
            }

            // Create output path preserving folder structure
            swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s",
                     state->output_dir,
                     relative_path);

            // Replace extension
            wchar_t *last_slash = wcsrchr(file->output_path, L'\\');
            if (last_slash) {
                wchar_t new_name[MAX_PATH_LEN];
                const wchar_t *old_name = last_slash + 1;
                replace_extension(old_name, format_to_extension_w(state->output_format), new_name, MAX_PATH_LEN);
                wchar_t dir_buf[MAX_PATH_LEN];
                wcsncpy(dir_buf, file->output_path, (size_t)(last_slash - file->output_path));
                dir_buf[(size_t)(last_slash - file->output_path)] = L'\0';
                swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s",
                         dir_buf,
                         new_name);
            }
        } else {
            // File doesn't start with root folder (shouldn't happen), fallback to simple
            const wchar_t *filename = wcsrchr(file->path, L'\\');
            if (!filename) filename = wcsrchr(file->path, L'/');
            if (filename) filename++;
            else filename = file->path;

            wchar_t new_name[MAX_PATH_LEN];
            replace_extension(filename, format_to_extension_w(state->output_format), new_name, MAX_PATH_LEN);
            swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s", state->output_dir, new_name);
        }
        return;
    }

    // Just put all files in the output directory
    {
        const wchar_t *filename = wcsrchr(file->path, L'\\');
        if (!filename) filename = wcsrchr(file->path, L'/');
        if (filename) filename++;

        // Replace extension
        if (filename) {
            wchar_t new_name[MAX_PATH_LEN];
            replace_extension(filename, format_to_extension_w(state->output_format), new_name, MAX_PATH_LEN);
            swprintf(file->output_path, MAX_PATH_LEN, L"%s\\%s", state->output_dir, new_name);
        } else {
            wcscpy(file->output_path, file->path);
        }
    }
}

static void set_file_timestamp(const wchar_t *path, FILETIME *ft) {
    HANDLE hFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFileTime(hFile, NULL, NULL, ft);
        CloseHandle(hFile);
    }
}

static void preserve_timestamp(const wchar_t *src_path, const wchar_t *dst_path) {
    FILETIME ft;
    if (get_exif_date_taken(src_path, &ft)) {
        // Use EXIF date
        set_file_timestamp(dst_path, &ft);
    } else {
        // Fall back to file modification time
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        if (GetFileAttributesExW(src_path, GetFileExInfoStandard, &attrs)) {
            set_file_timestamp(dst_path, &attrs.ftLastWriteTime);
        }
    }
}

static double qpc_elapsed_ms(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq) {
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static void io_queue_init(IoQueue *q, int capacity) {
    q->items = (IoJob*)calloc((size_t)capacity, sizeof(IoJob));
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->done = FALSE;
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->not_empty);
    InitializeConditionVariable(&q->not_full);
}

static void io_queue_destroy(IoQueue *q) {
    if (!q->items) return;
    free(q->items);
    q->items = NULL;
    q->capacity = 0;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->done = FALSE;
    DeleteCriticalSection(&q->cs);
}

static BOOL io_queue_push(IoQueue *q, const IoJob *job, BOOL *stop_flag) {
    EnterCriticalSection(&q->cs);
    while (q->count == q->capacity && !q->done && !*stop_flag) {
        SleepConditionVariableCS(&q->not_full, &q->cs, INFINITE);
    }
    if (*stop_flag) {
        LeaveCriticalSection(&q->cs);
        return FALSE;
    }
    q->items[q->tail] = *job;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    WakeConditionVariable(&q->not_empty);
    LeaveCriticalSection(&q->cs);
    return TRUE;
}

static BOOL io_queue_pop(IoQueue *q, IoJob *out_job, BOOL *stop_flag) {
    EnterCriticalSection(&q->cs);
    while (q->count == 0 && !q->done && !*stop_flag) {
        SleepConditionVariableCS(&q->not_empty, &q->cs, INFINITE);
    }
    if (q->count == 0) {
        LeaveCriticalSection(&q->cs);
        return FALSE;
    }
    *out_job = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    WakeConditionVariable(&q->not_full);
    LeaveCriticalSection(&q->cs);
    return TRUE;
}

static void io_job_cleanup(IoJob *job) {
    if (!job) return;
    if (job->mapped) {
        if (job->data) UnmapViewOfFile(job->data);
        if (job->hMap) CloseHandle(job->hMap);
        if (job->hFile) CloseHandle(job->hFile);
    } else {
        if (job->data) free(job->data);
    }
    job->data = NULL;
    job->size = 0;
    job->hFile = NULL;
    job->hMap = NULL;
    job->mapped = FALSE;
    job->index = -1;
}

static BOOL io_job_prepare_mmap(const wchar_t *path, IoJob *job) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile, &file_size) || file_size.QuadPart == 0) {
        CloseHandle(hFile);
        return FALSE;
    }

    if ((unsigned long long)file_size.QuadPart > (size_t)-1) {
        CloseHandle(hFile);
        return FALSE;
    }

    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        return FALSE;
    }

    unsigned char *data = (unsigned char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!data) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    job->data = data;
    job->size = (size_t)file_size.QuadPart;
    job->hFile = hFile;
    job->hMap = hMap;
    job->mapped = TRUE;
    return TRUE;
}

static void prepare_output_paths(AppState *state, BOOL create_dirs) {
    wchar_t last_dir[MAX_PATH_LEN] = L"";
    for (int index = 0; index < state->file_count; index++) {
        ImageFile *file = &state->files[index];
        generate_output_path(state, file);

        if (!create_dirs) {
            continue;
        }

        wchar_t output_dir[MAX_PATH_LEN];
        wcscpy(output_dir, file->output_path);
        wchar_t *slash = wcsrchr(output_dir, L'\\');
        if (!slash) continue;
        *slash = L'\0';

        if (wcscmp(output_dir, last_dir) == 0) {
            continue;
        }

        if (!create_directory_recursive(output_dir)) {
            log_error(L"  Failed to create directory: %s", output_dir);
        }

        wcsncpy(last_dir, output_dir, MAX_PATH_LEN - 1);
        last_dir[MAX_PATH_LEN - 1] = L'\0';
    }
}

static BOOL process_file(AppState *state, ImageFile *file, IoJob *job, ThreadStats *stats) {
    log_message(L"Processing file: %s", file->path);
    log_message(L"  Input format: %d, Quality: %d, Resolution: %d%%",
                file->format, state->quality, state->resolution_percent);
    LARGE_INTEGER t0, t1;

    if (state->enable_logging && file->format == FORMAT_JPEG) {
#if defined(HAVE_JPEG) && defined(HAVE_TURBOJPEG)
        int subsamp = -1;
        int colorspace = -1;
        BOOL info_ok = FALSE;
        if (job && job->data && job->size > 0) {
            info_ok = image_get_jpeg_info_from_memory(job->data, job->size, &subsamp, &colorspace);
        } else {
            info_ok = image_get_jpeg_info(file->path, &subsamp, &colorspace);
        }
        if (info_ok) {
            switch (subsamp) {
                case TJSAMP_444: stats->jpeg_subsamp_444++; break;
                case TJSAMP_422: stats->jpeg_subsamp_422++; break;
                case TJSAMP_420: stats->jpeg_subsamp_420++; break;
                case TJSAMP_GRAY: stats->jpeg_subsamp_gray++; break;
                default: stats->jpeg_subsamp_other++; break;
            }
            switch (colorspace) {
                case TJCS_GRAY: stats->jpeg_colorspace_gray++; break;
                case TJCS_YCbCr: stats->jpeg_colorspace_ycbcr++; break;
                case TJCS_RGB: stats->jpeg_colorspace_rgb++; break;
                case TJCS_CMYK: stats->jpeg_colorspace_cmyk++; break;
                default: stats->jpeg_colorspace_other++; break;
            }
        } else {
            stats->jpeg_info_fail++;
        }
#endif
    }

#if defined(HAVE_VIPS)
    if (file->format == FORMAT_JPEG &&
        state->output_format == FORMAT_WEBP) {
        QueryPerformanceCounter(&t0);
        BOOL vips_ok = image_convert_jpeg_to_webp_vips(file->path, file->output_path,
                                                       state->quality, state->webp_method,
                                                       state->max_resolution,
                                                       state->resolution_percent);
        QueryPerformanceCounter(&t1);
        if (vips_ok) {
            double save_ms = qpc_elapsed_ms(t0, t1, state->frequency);
            stats->total_save_us += (LONG64)(save_ms * 1000.0);
            stats->files_saved++;

            file->success = TRUE;
            file->processed = TRUE;

            WIN32_FILE_ATTRIBUTE_DATA attrs;
            if (GetFileAttributesExW(file->output_path, GetFileExInfoStandard, &attrs)) {
                LARGE_INTEGER size;
                size.HighPart = attrs.nFileSizeHigh;
                size.LowPart = attrs.nFileSizeLow;
                file->output_size = (size_t)size.QuadPart;
            }

            preserve_timestamp(file->path, file->output_path);
            // Update output size only (input size is pre-calculated)
            InterlockedAdd64(&state->total_output_size, (LONG64)file->output_size);
            // Update input bytes processed for MB/s calculation
            InterlockedAdd64(&state->total_input_bytes_processed, (LONG64)file->input_size);

            return TRUE;
        }
    }
#endif

#if defined(HAVE_JPEG) && defined(HAVE_TURBOJPEG)
    // Fast-path for no-resize JPEG -> WEBP/JPEG using YUV420 (avoids RGB conversion).
    if (file->format == FORMAT_JPEG &&
        state->max_resolution == 0 &&
        state->resolution_percent == 100) {
        BOOL fast_success = FALSE;
        int fail_stage = YUV_FAST_STAGE_NONE;
        QueryPerformanceCounter(&t0);
        if (state->output_format == FORMAT_WEBP) {
#ifdef HAVE_WEBP
            (void)fail_stage;
#endif
        } else if (state->output_format == FORMAT_JPEG) {
            stats->fastpath_jpeg_to_jpeg_attempts++;
            fast_success = image_convert_jpeg_to_jpeg_yuv_fast(
                file->path,
                file->output_path,
                state->quality,
                (job && job->data && job->size > 0) ? job->data : NULL,
                (job && job->data && job->size > 0) ? job->size : 0,
                &fail_stage);
        }
        QueryPerformanceCounter(&t1);

        if (fast_success) {
            if (state->output_format == FORMAT_WEBP) {
                stats->fastpath_jpeg_to_webp_hits++;
            } else if (state->output_format == FORMAT_JPEG) {
                stats->fastpath_jpeg_to_jpeg_hits++;
            }
            double save_ms = qpc_elapsed_ms(t0, t1, state->frequency);
            stats->total_save_us += (LONG64)(save_ms * 1000.0);
            stats->files_saved++;

            file->success = TRUE;
            file->processed = TRUE;

            // Get output file size
            WIN32_FILE_ATTRIBUTE_DATA attrs;
            if (GetFileAttributesExW(file->output_path, GetFileExInfoStandard, &attrs)) {
                LARGE_INTEGER size;
                size.HighPart = attrs.nFileSizeHigh;
                size.LowPart = attrs.nFileSizeLow;
                file->output_size = (size_t)size.QuadPart;
            }

            // Preserve timestamp
            preserve_timestamp(file->path, file->output_path);

            // Update output size only (input size is pre-calculated)
            InterlockedAdd64(&state->total_output_size, (LONG64)file->output_size);
            // Update input bytes processed for MB/s calculation
            InterlockedAdd64(&state->total_input_bytes_processed, (LONG64)file->input_size);

            log_message(L"  Fast-path complete. Input: %zu bytes, Output: %zu bytes",
                        file->input_size, file->output_size);
            return TRUE;
        } else {
            if (state->output_format == FORMAT_WEBP) {
                if (fail_stage == YUV_FAST_STAGE_DECODE) {
                    stats->fastpath_jpeg_to_webp_decode_fail++;
                } else if (fail_stage == YUV_FAST_STAGE_ENCODE) {
                    stats->fastpath_jpeg_to_webp_encode_fail++;
                } else {
                    stats->jpeg_yuv_decode_fail++;
                }
            } else if (state->output_format == FORMAT_JPEG) {
                if (fail_stage == YUV_FAST_STAGE_DECODE) {
                    stats->fastpath_jpeg_to_jpeg_decode_fail++;
                } else if (fail_stage == YUV_FAST_STAGE_ENCODE) {
                    stats->fastpath_jpeg_to_jpeg_encode_fail++;
                } else {
                    stats->jpeg_yuv_decode_fail++;
                }
            } else {
                stats->jpeg_yuv_decode_fail++;
            }
        }
    }
#endif

    // Load image
    log_message(L"  Loading image...");
    QueryPerformanceCounter(&t0);
    BOOL prefer_rgb = (state->output_format == FORMAT_JPEG) ||
                      (file->format == FORMAT_JPEG && state->output_format == FORMAT_WEBP);
    ImageData *img = NULL;
#if defined(HAVE_JPEG)
    if (job && job->data && job->size > 0 && file->format == FORMAT_JPEG) {
        img = image_load_jpeg_from_memory(job->data, job->size, prefer_rgb);
    }
#endif
    if (!img) {
        img = image_load_ex(file->path, prefer_rgb);
    }
    QueryPerformanceCounter(&t1);
    double load_ms = qpc_elapsed_ms(t0, t1, state->frequency);
    stats->total_load_us += (LONG64)(load_ms * 1000.0);
    stats->files_loaded++;
    if (!img) {
        DWORD win_error = GetLastError();
        file->error = map_windows_error(win_error);
        // If Windows error is 0, use decode failed as default
        if (file->error == 0) {
            file->error = 0x05; // ERROR_DECODE_FAILED
        }
        // Get full error message from log
        swprintf(file->error_message, 256, L"Failed to load image. Error: %lu", win_error);
        log_error(L"  %s", file->error_message);
        file->success = FALSE;
        file->processed = TRUE;
        return FALSE;
    }
    log_message(L"  Loaded: %dx%d, %d channels", img->width, img->height, img->channels);

    // Resize if needed
    if (state->max_resolution > 0) {
        int max_dim = (img->width > img->height) ? img->width : img->height;
        if (max_dim > state->max_resolution) {
            double scale = (double)state->max_resolution / (double)max_dim;
            int new_width = (int)(img->width * scale + 0.5);
            int new_height = (int)(img->height * scale + 0.5);
            if (new_width < 1) new_width = 1;
            if (new_height < 1) new_height = 1;
            log_message(L"  Resizing to %dx%d...", new_width, new_height);

            QueryPerformanceCounter(&t0);
            ImageData *resized = image_resize(img, new_width, new_height);
            QueryPerformanceCounter(&t1);
            double resize_ms = qpc_elapsed_ms(t0, t1, state->frequency);
            stats->total_resize_us += (LONG64)(resize_ms * 1000.0);
            stats->files_resized++;
            if (resized) {
                image_release(img);
                img = resized;
                log_message(L"  Resize complete");
            } else {
                log_error(L"  Resize failed");
            }
        }
    } else if (state->resolution_percent != 100) {
        int new_width = (img->width * state->resolution_percent + 50) / 100;
        int new_height = (img->height * state->resolution_percent + 50) / 100;
        log_message(L"  Resizing to %dx%d...", new_width, new_height);

        QueryPerformanceCounter(&t0);
        ImageData *resized = image_resize(img, new_width, new_height);
        QueryPerformanceCounter(&t1);
        double resize_ms = qpc_elapsed_ms(t0, t1, state->frequency);
        stats->total_resize_us += (LONG64)(resize_ms * 1000.0);
        stats->files_resized++;
        if (resized) {
            image_release(img);
            img = resized;
            log_message(L"  Resize complete");
        } else {
            log_error(L"  Resize failed");
        }
    }

    // ICO max size clamp (256x256)
    if (state->output_format == FORMAT_ICO &&
        (img->width > 256 || img->height > 256)) {
        double scale_w = 256.0 / (double)img->width;
        double scale_h = 256.0 / (double)img->height;
        double scale = (scale_w < scale_h) ? scale_w : scale_h;
        int new_width = (int)(img->width * scale + 0.5);
        int new_height = (int)(img->height * scale + 0.5);
        if (new_width < 1) new_width = 1;
        if (new_height < 1) new_height = 1;

        log_message(L"  ICO resize to %dx%d (max 256)", new_width, new_height);
        QueryPerformanceCounter(&t0);
        ImageData *resized = image_resize(img, new_width, new_height);
        QueryPerformanceCounter(&t1);
        double resize_ms = qpc_elapsed_ms(t0, t1, state->frequency);
        stats->total_resize_us += (LONG64)(resize_ms * 1000.0);
        stats->files_resized++;
        if (resized) {
            image_release(img);
            img = resized;
            log_message(L"  ICO resize complete");
        } else {
            log_error(L"  ICO resize failed");
        }
    }

    // Generate output path
    log_message(L"  Output path: %s", file->output_path);
    log_message(L"  Output format: %d", state->output_format);

    // Save image
    log_message(L"  Saving image...");
    QueryPerformanceCounter(&t0);
    file->success = image_save(img, file->output_path, state->output_format, state->quality);
    QueryPerformanceCounter(&t1);
    double save_ms = qpc_elapsed_ms(t0, t1, state->frequency);
    stats->total_save_us += (LONG64)(save_ms * 1000.0);
    stats->files_saved++;
    file->processed = TRUE;

    if (file->success) {
        log_message(L"  Save successful!");
        // Get output file size with retry for timing issues
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        int retries = 3;
        BOOL got_size = FALSE;
        while (retries-- > 0 && !got_size) {
            if (GetFileAttributesExW(file->output_path, GetFileExInfoStandard, &attrs)) {
                LARGE_INTEGER size;
                size.HighPart = attrs.nFileSizeHigh;
                size.LowPart = attrs.nFileSizeLow;
                file->output_size = (size_t)size.QuadPart;
                got_size = TRUE;
            }
            if (!got_size && retries > 0) {
                Sleep(10); // Wait 10ms before retry
            }
        }
        if (!got_size) {
            log_error(L"  Failed to get output file size");
        }

        // Preserve timestamp
        preserve_timestamp(file->path, file->output_path);

        // Update output size only (input size is pre-calculated)
        InterlockedAdd64(&state->total_output_size, (LONG64)file->output_size);
        // Update input bytes processed for MB/s calculation
        InterlockedAdd64(&state->total_input_bytes_processed, (LONG64)file->input_size);

        log_message(L"  Complete. Input: %zu bytes, Output: %zu bytes", file->input_size, file->output_size);
    } else {
        log_error(L"  Failed to save image!");
    }

    image_release(img);
    return file->success;
}

static void merge_thread_stats(AppState *state, const ThreadStats *stats) {
    if (!state || !stats) return;

    InterlockedAdd64(&state->total_load_us, stats->total_load_us);
    InterlockedAdd64(&state->total_resize_us, stats->total_resize_us);
    InterlockedAdd64(&state->total_save_us, stats->total_save_us);
    InterlockedAdd(&state->files_loaded, stats->files_loaded);
    InterlockedAdd(&state->files_resized, stats->files_resized);
    InterlockedAdd(&state->files_saved, stats->files_saved);
    InterlockedAdd64(&state->total_input_size, stats->total_input_size);
    InterlockedAdd64(&state->total_output_size, stats->total_output_size);
    InterlockedAdd(&state->fastpath_jpeg_to_jpeg_attempts, stats->fastpath_jpeg_to_jpeg_attempts);
    InterlockedAdd(&state->fastpath_jpeg_to_jpeg_hits, stats->fastpath_jpeg_to_jpeg_hits);
    InterlockedAdd(&state->fastpath_jpeg_to_webp_attempts, stats->fastpath_jpeg_to_webp_attempts);
    InterlockedAdd(&state->fastpath_jpeg_to_webp_hits, stats->fastpath_jpeg_to_webp_hits);
    InterlockedAdd(&state->jpeg_subsamp_444, stats->jpeg_subsamp_444);
    InterlockedAdd(&state->jpeg_subsamp_422, stats->jpeg_subsamp_422);
    InterlockedAdd(&state->jpeg_subsamp_420, stats->jpeg_subsamp_420);
    InterlockedAdd(&state->jpeg_subsamp_gray, stats->jpeg_subsamp_gray);
    InterlockedAdd(&state->jpeg_subsamp_other, stats->jpeg_subsamp_other);
    InterlockedAdd(&state->jpeg_colorspace_gray, stats->jpeg_colorspace_gray);
    InterlockedAdd(&state->jpeg_colorspace_ycbcr, stats->jpeg_colorspace_ycbcr);
    InterlockedAdd(&state->jpeg_colorspace_rgb, stats->jpeg_colorspace_rgb);
    InterlockedAdd(&state->jpeg_colorspace_cmyk, stats->jpeg_colorspace_cmyk);
    InterlockedAdd(&state->jpeg_colorspace_other, stats->jpeg_colorspace_other);
    InterlockedAdd(&state->jpeg_yuv_decode_fail, stats->jpeg_yuv_decode_fail);
    InterlockedAdd(&state->jpeg_info_fail, stats->jpeg_info_fail);
    InterlockedAdd(&state->fastpath_jpeg_to_jpeg_decode_fail, stats->fastpath_jpeg_to_jpeg_decode_fail);
    InterlockedAdd(&state->fastpath_jpeg_to_jpeg_encode_fail, stats->fastpath_jpeg_to_jpeg_encode_fail);
    InterlockedAdd(&state->fastpath_jpeg_to_webp_decode_fail, stats->fastpath_jpeg_to_webp_decode_fail);
    InterlockedAdd(&state->fastpath_jpeg_to_webp_encode_fail, stats->fastpath_jpeg_to_webp_encode_fail);
}

DWORD WINAPI processing_thread(LPVOID param) {
    AppState *state = (AppState*)param;
    ThreadStats stats = {0};
    log_message(L"Thread started");

    while (!state->stop_processing) {
        IoJob job = {0};
        if (!io_queue_pop(&g_io_queue, &job, &state->stop_processing)) {
            log_message(L"Thread: No more files, exiting");
            break;
        }

        int index = job.index;
        if (index < 0 || index >= state->file_count) {
            io_job_cleanup(&job);
            continue;
        }

        log_message(L"Thread: Processing file %d/%d", index + 1, state->file_count);

        // Process file
        ImageFile *file = &state->files[index];
        process_file(state, file, &job, &stats);
        io_job_cleanup(&job);
        InterlockedIncrement(&state->files_processed);
    }

    merge_thread_stats(state, &stats);
    log_message(L"Thread exiting");
    return 0;
}

void processing_start(AppState *state) {
    if (state->file_count == 0) {
        log_error(L"processing_start called but no files in queue!");
        return;
    }

    state->enable_logging = (Button_GetCheck(state->hLoggingCheck) == BST_CHECKED);
    if (state->enable_logging) {
        log_init();
        if (log_get_level() == LOG_LEVEL_NONE) {
            log_set_level(LOG_LEVEL_INFO);
        }
    }

    if (!state->enable_logging) {
        g_prev_log_level = log_get_level();
        log_set_level(LOG_LEVEL_NONE);
        g_log_level_overridden = TRUE;
    } else if (state->file_count > 1) {
        g_prev_log_level = log_get_level();
        log_set_level(LOG_LEVEL_ERROR);
        g_log_level_overridden = TRUE;
    }

    log_message(L"=== Starting processing ===");
#if defined(HAVE_JPEG)
    {
        extern int image_get_simd_level(void);
        int simd_level = image_get_simd_level();
        const wchar_t *simd_name = L"SSE2";
        if (simd_level == 3) {
            simd_name = L"AVX2";
        } else if (simd_level == 2) {
            simd_name = L"AVX";
        }
        log_message(L"SIMD resize: %s", simd_name);
    }
#endif
    log_message(L"Files to process: %d", state->file_count);

    // Get current settings from UI
    GetWindowTextW(state->hOutputDirEdit, state->output_dir, MAX_PATH_LEN);
    log_message(L"Output directory: %s", state->output_dir);

    int format_idx = (int)SendMessageW(state->hFormatCombo, CB_GETCURSEL, 0, 0);
    switch (format_idx) {
        case 0: state->output_format = FORMAT_WEBP; break;
        case 1: state->output_format = FORMAT_JPEG; break;
        case 2: state->output_format = FORMAT_PNG; break;
        case 3: state->output_format = FORMAT_BMP; break;
        case 4: state->output_format = FORMAT_TIFF; break;
        case 5: state->output_format = FORMAT_ICO; break;
        default: state->output_format = FORMAT_WEBP; break;
    }
    log_message(L"Output format: %s (%d)", format_to_wstring(state->output_format), state->output_format);

    state->quality = (int)SendMessage(state->hQualityTrack, TBM_GETPOS, 0, 0);
    state->resolution_percent = (int)SendMessage(state->hResolutionTrack, TBM_GETPOS, 0, 0);
    state->num_threads = (int)SendMessage(state->hThreadTrack, TBM_GETPOS, 0, 0);
    log_message(L"Settings: Quality=%d%%, Resolution=%d%%, Threads=%d",
                state->quality, state->resolution_percent, state->num_threads);

    int max_res_sel = (int)SendMessage(state->hMaxResCombo, CB_GETCURSEL, 0, 0);
    switch (max_res_sel) {
        case 1: state->max_resolution = 2160; break;
        case 2: state->max_resolution = 1440; break;
        case 3: state->max_resolution = 1080; break;
        default: state->max_resolution = 0; break;
    }
    if (state->max_resolution > 0) {
        log_message(L"Max resolution: %d", state->max_resolution);
    }

    state->preserve_folder_structure = (Button_GetCheck(state->hPreserveFolderCheck) == BST_CHECKED);
    log_message(L"Preserve folder structure: %d", state->preserve_folder_structure);
    log_message(L"Enable logging: %d", state->enable_logging);
    log_message(L"UI progress updates: 1");

    // Pre-generate output paths; create directories once per run
    prepare_output_paths(state, TRUE);

    // Initialize processing state
    state->processing = TRUE;
    state->stop_processing = FALSE;
    state->files_processed = 0;

    // Calculate total input size from all files BEFORE processing starts
    // This ensures "Before size" shows the correct total immediately
    size_t total_input = 0;
    for (int i = 0; i < state->file_count; i++) {
        total_input += state->files[i].input_size;
    }
    state->total_input_size = (LONG64)total_input;
    state->total_output_size = 0;
    state->total_input_bytes_processed = 0;
    state->total_load_us = 0;
    state->total_resize_us = 0;
    state->total_save_us = 0;
    state->files_loaded = 0;
    state->files_resized = 0;
    state->files_saved = 0;
    state->fastpath_jpeg_to_jpeg_attempts = 0;
    state->fastpath_jpeg_to_jpeg_hits = 0;
    state->fastpath_jpeg_to_webp_attempts = 0;
    state->fastpath_jpeg_to_webp_hits = 0;
    state->jpeg_subsamp_444 = 0;
    state->jpeg_subsamp_422 = 0;
    state->jpeg_subsamp_420 = 0;
    state->jpeg_subsamp_gray = 0;
    state->jpeg_subsamp_other = 0;
    state->jpeg_colorspace_gray = 0;
    state->jpeg_colorspace_ycbcr = 0;
    state->jpeg_colorspace_rgb = 0;
    state->jpeg_colorspace_cmyk = 0;
    state->jpeg_colorspace_other = 0;
    state->jpeg_yuv_decode_fail = 0;
    state->jpeg_info_fail = 0;
    state->fastpath_jpeg_to_jpeg_decode_fail = 0;
    state->fastpath_jpeg_to_jpeg_encode_fail = 0;
    state->fastpath_jpeg_to_webp_decode_fail = 0;
    state->fastpath_jpeg_to_webp_encode_fail = 0;
    state->total_ui_progress_ms = 0.0;
    state->ui_progress_updates = 0;
    // Set progress range (one tick per file)
    SendMessage(state->hProgressFiles, PBM_SETRANGE, 0, MAKELPARAM(0, state->file_count));
    SendMessage(state->hProgressFiles, PBM_SETPOS, 0, 0);

    // Disable UI
    window_set_processing_state(state, TRUE);

    // Get start time
    QueryPerformanceFrequency(&state->frequency);
    QueryPerformanceCounter(&state->start_time);

    io_queue_init(&g_io_queue, 64);
    g_next_io_index = 0;
    g_active_io_threads = IO_THREAD_COUNT;
    for (int i = 0; i < IO_THREAD_COUNT; i++) {
        g_io_threads[i] = CreateThread(NULL, 0, io_reader_thread, state, 0, NULL);
        if (!g_io_threads[i]) {
            log_error(L"Failed to create IO reader thread %d", i);
            InterlockedDecrement(&g_active_io_threads);
        }
    }
    if (g_active_io_threads == 0) {
        EnterCriticalSection(&g_io_queue.cs);
        g_io_queue.done = TRUE;
        WakeAllConditionVariable(&g_io_queue.not_empty);
        LeaveCriticalSection(&g_io_queue.cs);
    }

    // Create worker threads
    log_message(L"Creating %d worker threads...", state->num_threads);
    state->threads = (HANDLE*)malloc(sizeof(HANDLE) * state->num_threads);
    for (int i = 0; i < state->num_threads; i++) {
        state->threads[i] = CreateThread(NULL, 0, processing_thread, state, 0, NULL);
        if (state->threads[i]) {
            log_message(L"  Thread %d created successfully", i);
        } else {
            log_error(L"  Failed to create thread %d", i);
        }
    }
}

void processing_stop(AppState *state) {
    state->stop_processing = TRUE;

    EnterCriticalSection(&g_io_queue.cs);
    g_io_queue.done = TRUE;
    WakeAllConditionVariable(&g_io_queue.not_empty);
    WakeAllConditionVariable(&g_io_queue.not_full);
    LeaveCriticalSection(&g_io_queue.cs);

    // Wait for threads to finish
    if (state->threads) {
        WaitForMultipleObjects(state->num_threads, state->threads, TRUE, INFINITE);
        for (int i = 0; i < state->num_threads; i++) {
            CloseHandle(state->threads[i]);
        }
        free(state->threads);
        state->threads = NULL;
    }

    state->processing = FALSE;

    for (int i = 0; i < IO_THREAD_COUNT; i++) {
        if (g_io_threads[i]) {
            WaitForSingleObject(g_io_threads[i], INFINITE);
            CloseHandle(g_io_threads[i]);
            g_io_threads[i] = NULL;
        }
    }
    if (g_io_queue.items) {
        IoJob job = {0};
        while (io_queue_pop(&g_io_queue, &job, &state->stop_processing)) {
            io_job_cleanup(&job);
        }
        io_queue_destroy(&g_io_queue);
    }

    double total_load_ms = (double)state->total_load_us / 1000.0;
    double total_resize_ms = (double)state->total_resize_us / 1000.0;
    double total_save_ms = (double)state->total_save_us / 1000.0;
    LONG files_loaded = state->files_loaded;
    LONG files_resized = state->files_resized;
    LONG files_saved = state->files_saved;

    if (state->enable_logging && state->frequency.QuadPart != 0) {
        LogLevel prev_level = log_get_level();
        log_set_level(LOG_LEVEL_INFO);
        log_message(L"=== Perf Summary ===");
        log_message(L"Format: %s, Quality: %d%%, Resolution: %d%%, Threads: %d",
                    format_to_wstring(state->output_format),
                    state->quality,
                    state->resolution_percent,
                    state->num_threads);
        log_message(L"Total load: %.2f ms (avg %.2f ms)", total_load_ms,
                    files_loaded > 0 ? total_load_ms / files_loaded : 0.0);
        if (state->resolution_percent == 100 && files_resized == 0) {
            log_message(L"Resize skipped (resolution 100%%)");
        } else {
            log_message(L"Total resize: %.2f ms (avg %.2f ms, %d files)",
                        total_resize_ms,
                        files_resized > 0 ? total_resize_ms / files_resized : 0.0,
                        files_resized);
        }
        log_message(L"Total save: %.2f ms (avg %.2f ms)", total_save_ms,
                    files_saved > 0 ? total_save_ms / files_saved : 0.0);
        if (state->fastpath_jpeg_to_jpeg_attempts > 0 || state->fastpath_jpeg_to_webp_attempts > 0) {
            double jpeg_hit_pct = state->fastpath_jpeg_to_jpeg_attempts > 0
                ? (100.0 * state->fastpath_jpeg_to_jpeg_hits / state->fastpath_jpeg_to_jpeg_attempts)
                : 0.0;
            double webp_hit_pct = state->fastpath_jpeg_to_webp_attempts > 0
                ? (100.0 * state->fastpath_jpeg_to_webp_hits / state->fastpath_jpeg_to_webp_attempts)
                : 0.0;
            log_message(L"Fast-path hits: JPEG->JPEG %ld/%ld (%.1f%%), JPEG->WEBP %ld/%ld (%.1f%%)",
                        state->fastpath_jpeg_to_jpeg_hits,
                        state->fastpath_jpeg_to_jpeg_attempts,
                        jpeg_hit_pct,
                        state->fastpath_jpeg_to_webp_hits,
                        state->fastpath_jpeg_to_webp_attempts,
                        webp_hit_pct);
            if (state->jpeg_yuv_decode_fail > 0) {
                log_message(L"Fast-path decode failures: %ld", state->jpeg_yuv_decode_fail);
            }
            if (state->fastpath_jpeg_to_jpeg_decode_fail || state->fastpath_jpeg_to_jpeg_encode_fail ||
                state->fastpath_jpeg_to_webp_decode_fail || state->fastpath_jpeg_to_webp_encode_fail) {
                log_message(L"Fast-path failures: JPEG->JPEG decode=%ld encode=%ld, JPEG->WEBP decode=%ld encode=%ld",
                            state->fastpath_jpeg_to_jpeg_decode_fail,
                            state->fastpath_jpeg_to_jpeg_encode_fail,
                            state->fastpath_jpeg_to_webp_decode_fail,
                            state->fastpath_jpeg_to_webp_encode_fail);
            }
            if (state->jpeg_info_fail > 0) {
                log_message(L"JPEG info failures: %ld", state->jpeg_info_fail);
            }
            if (state->jpeg_subsamp_444 || state->jpeg_subsamp_422 || state->jpeg_subsamp_420 ||
                state->jpeg_subsamp_gray || state->jpeg_subsamp_other) {
                log_message(L"JPEG subsampling: 420=%ld 422=%ld 444=%ld gray=%ld other=%ld",
                            state->jpeg_subsamp_420,
                            state->jpeg_subsamp_422,
                            state->jpeg_subsamp_444,
                            state->jpeg_subsamp_gray,
                            state->jpeg_subsamp_other);
            }
            if (state->jpeg_colorspace_ycbcr || state->jpeg_colorspace_rgb || state->jpeg_colorspace_gray ||
                state->jpeg_colorspace_cmyk || state->jpeg_colorspace_other) {
                log_message(L"JPEG colorspace: YCbCr=%ld RGB=%ld Gray=%ld CMYK=%ld other=%ld",
                            state->jpeg_colorspace_ycbcr,
                            state->jpeg_colorspace_rgb,
                            state->jpeg_colorspace_gray,
                            state->jpeg_colorspace_cmyk,
                            state->jpeg_colorspace_other);
            }
        }
        log_message(L"UI progress updates: %.2f ms (avg %.2f ms, %d updates)",
                    state->total_ui_progress_ms,
                    state->ui_progress_updates > 0 ? state->total_ui_progress_ms / state->ui_progress_updates : 0.0,
                    state->ui_progress_updates);
        log_set_level(prev_level);
    }

    if (g_log_level_overridden && state->enable_logging) {
        log_set_level(g_prev_log_level);
        g_log_level_overridden = FALSE;
    } else if (g_log_level_overridden) {
        g_log_level_overridden = FALSE;
    }

    // Re-enable UI
    window_set_processing_state(state, FALSE);

    // Save settings
    settings_save(state);
}

BOOL processing_is_complete(AppState *state) {
    if (!state->processing) return FALSE;

    // Check if all files have been processed
    LONG processed = InterlockedCompareExchange(&state->files_processed, 0, 0);
    if (processed < state->file_count) return FALSE;

    // Check if all threads have finished
    if (state->threads) {
        DWORD wait_result = WaitForMultipleObjects(state->num_threads, state->threads, TRUE, 0);
        if (wait_result == WAIT_TIMEOUT) return FALSE; // Still running
    }

    return TRUE;
}
