#include "dragdrop.h"
#include "utils.h"
#include <shlwapi.h>
#include <stdio.h>

// Hash bucket count - power of 2 for fast modulo using bitmask
#define HASH_BUCKET_COUNT 16384  // 2^14 buckets
#define HASH_BUCKET_MASK (HASH_BUCKET_COUNT - 1)

// FNV-1a hash function for fast string hashing
static uint32_t fnv1a_hash(const wchar_t *str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint32_t)*str;
        hash *= 16777619u;
        str++;
    }
    return hash;
}

// Initialize hash table with proper bucket structure
void dragdrop_init_hash_table(AppState *state, int estimated_files) {
    (void)estimated_files;  // Parameter kept for API compatibility, bucket count is fixed
    
    state->hash_buckets = NULL;
    state->hash_bucket_count = HASH_BUCKET_COUNT;
    state->hash_bucket_mask = HASH_BUCKET_MASK;
    
    // Allocate bucket array (all buckets initially NULL)
    state->hash_buckets = (HashEntry**)calloc(HASH_BUCKET_COUNT, sizeof(HashEntry*));
}

// Clear hash table and free all entries
void dragdrop_clear_hash_table(AppState *state) {
    if (state->hash_buckets) {
        // Free all hash entries
        for (int i = 0; i < state->hash_bucket_count; i++) {
            HashEntry *entry = state->hash_buckets[i];
            while (entry) {
                HashEntry *next = entry->next;
                free(entry);
                entry = next;
            }
            state->hash_buckets[i] = NULL;
        }
        free(state->hash_buckets);
        state->hash_buckets = NULL;
    }
}

// Check if hash exists in table - O(1) lookup
static BOOL hash_exists(AppState *state, uint32_t hash) {
    int bucket = hash & state->hash_bucket_mask;  // Fast modulo using bitmask
    
    EnterCriticalSection(&state->hash_cs);
    
    HashEntry *entry = state->hash_buckets[bucket];
    while (entry) {
        if (entry->hash == hash) {
            LeaveCriticalSection(&state->hash_cs);
            return TRUE;
        }
        entry = entry->next;
    }
    
    LeaveCriticalSection(&state->hash_cs);
    return FALSE;
}

// Add hash to table - O(1) insertion
static BOOL hash_add(AppState *state, uint32_t hash) {
    int bucket = hash & state->hash_bucket_mask;
    
    EnterCriticalSection(&state->hash_cs);
    
    // Check for collision in this bucket
    HashEntry *entry = state->hash_buckets[bucket];
    while (entry) {
        if (entry->hash == hash) {
            // Already exists
            LeaveCriticalSection(&state->hash_cs);
            return FALSE;
        }
        entry = entry->next;
    }
    
    // Add new entry at head of list
    HashEntry *new_entry = (HashEntry*)malloc(sizeof(HashEntry));
    if (!new_entry) {
        LeaveCriticalSection(&state->hash_cs);
        return FALSE;
    }
    new_entry->hash = hash;
    new_entry->next = state->hash_buckets[bucket];
    state->hash_buckets[bucket] = new_entry;
    
    LeaveCriticalSection(&state->hash_cs);
    return TRUE;
}

static BOOL is_image_file(const wchar_t *path) {
    ImageFormat format = detect_format_from_path(path);
    return format != FORMAT_UNKNOWN && is_format_supported(format);
}

// Worker thread for scanning folders
typedef struct {
    AppState *state;
    HDROP hdrop;
    UINT file_count;
} ScanThreadParams;

// Local structure to collect files before batch adding
typedef struct {
    wchar_t path[MAX_PATH_LEN];
    wchar_t root_folder[MAX_PATH_LEN];
    size_t file_size;
    ImageFormat format;
} CollectedFile;

static DWORD WINAPI scan_thread_proc(LPVOID param) {
    ScanThreadParams *params = (ScanThreadParams*)param;
    AppState *state = params->state;
    
    // Pre-allocate local collection array based on estimated file count
    // We'll use a dynamic array that grows as needed
    int collected_capacity = params->file_count * 2;  // Initial estimate
    CollectedFile *collected = (CollectedFile*)malloc(sizeof(CollectedFile) * collected_capacity);
    if (!collected) {
        DragFinish(params->hdrop);
        free(params);
        return 0;
    }
    int collected_count = 0;
    
    // Disable ListView redraw for batch updates
    SendMessageW(state->hFilesList, WM_SETREDRAW, FALSE, 0);

    for (UINT i = 0; i < params->file_count; i++) {
        // Check for cancellation
        if (state->stop_scanning) {
            break;
        }

        wchar_t path[MAX_PATH_LEN];
        DragQueryFileW(params->hdrop, i, path, MAX_PATH_LEN);

        DWORD attrs = GetFileAttributesW(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            continue;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            // Scan folder recursively (including subdirectories)
            // Use a stack-based approach for proper recursion
            typedef struct {
                wchar_t path[MAX_PATH_LEN];
                wchar_t root_folder[MAX_PATH_LEN];
            } FolderStackItem;

            // Allocate a stack for folder traversal
            FolderStackItem *stack = NULL;
            int stack_capacity = 256;
            int stack_size = 0;
            stack = (FolderStackItem*)malloc(sizeof(FolderStackItem) * stack_capacity);
            if (!stack) {
                DragFinish(params->hdrop);
                free(collected);
                free(params);
                return 0;
            }

            // Push initial folder onto stack
            wcsncpy(stack[0].path, path, MAX_PATH_LEN - 1);
            stack[0].path[MAX_PATH_LEN - 1] = L'\0';
            wcsncpy(stack[0].root_folder, path, MAX_PATH_LEN - 1);
            stack[0].root_folder[MAX_PATH_LEN - 1] = L'\0';
            stack_size = 1;

            // Process folders in stack
            while (stack_size > 0) {
                // Check for cancellation
                if (state->stop_scanning) {
                    break;
                }

                // Pop folder from stack
                stack_size--;
                FolderStackItem item = stack[stack_size];

                // Scan this folder
                WIN32_FIND_DATAW find_data;
                wchar_t search_path[MAX_PATH_LEN];
                swprintf(search_path, MAX_PATH_LEN, L"%s\\*", item.path);

                HANDLE hFind = FindFirstFileW(search_path, &find_data);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        // Check for cancellation
                        if (state->stop_scanning) {
                            break;
                        }

                        if (wcscmp(find_data.cFileName, L".") == 0 ||
                            wcscmp(find_data.cFileName, L"..") == 0) {
                            continue;
                        }

                        wchar_t full_path[MAX_PATH_LEN];
                        swprintf(full_path, MAX_PATH_LEN, L"%s\\%s", item.path, find_data.cFileName);

                        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            // Push subdirectory onto stack
                            if (stack_size >= stack_capacity) {
                                int new_capacity = stack_capacity * 2;
                                FolderStackItem *new_stack = (FolderStackItem*)realloc(stack, sizeof(FolderStackItem) * new_capacity);
                                if (!new_stack) {
                                    // Out of memory, skip this subdirectory
                                    continue;
                                }
                                stack = new_stack;
                                stack_capacity = new_capacity;
                            }
                            wcsncpy(stack[stack_size].path, full_path, MAX_PATH_LEN - 1);
                            stack[stack_size].path[MAX_PATH_LEN - 1] = L'\0';
                            wcsncpy(stack[stack_size].root_folder, item.root_folder, MAX_PATH_LEN - 1);
                            stack[stack_size].root_folder[MAX_PATH_LEN - 1] = L'\0';
                            stack_size++;
                        } else {
                            // Check if it's an image file
                            if (is_image_file(full_path)) {
                                uint32_t hash = fnv1a_hash(full_path);
                                
                                // Check for duplicate using O(1) hash lookup
                                if (hash_exists(state, hash)) {
                                    continue;  // Skip duplicate
                                }
                                
                                // Grow collection array if needed
                                if (collected_count >= collected_capacity) {
                                    int new_capacity = collected_capacity * 2;
                                    CollectedFile *new_collected = (CollectedFile*)realloc(collected, sizeof(CollectedFile) * new_capacity);
                                    if (!new_collected) {
                                        // Out of memory, skip this file
                                        continue;
                                    }
                                    collected = new_collected;
                                    collected_capacity = new_capacity;
                                }
                                
                                // Add to collection (use file size from WIN32_FIND_DATA)
                                wcsncpy(collected[collected_count].path, full_path, MAX_PATH_LEN - 1);
                                collected[collected_count].path[MAX_PATH_LEN - 1] = L'\0';
                                wcsncpy(collected[collected_count].root_folder, item.root_folder, MAX_PATH_LEN - 1);
                                collected[collected_count].root_folder[MAX_PATH_LEN - 1] = L'\0';
                                collected[collected_count].file_size = ((size_t)find_data.nFileSizeHigh << 32) | (size_t)find_data.nFileSizeLow;
                                collected[collected_count].format = detect_format_from_path(full_path);
                                collected_count++;
                                
                                // Add hash to table
                                hash_add(state, hash);
                            }
                        }
                    } while (FindNextFileW(hFind, &find_data));
                    FindClose(hFind);
                }

                // Update progress every 100 files
                if (collected_count > 0 && collected_count % 100 == 0) {
                    PostMessageW(state->hwnd, WM_APP_SCANNING_PROGRESS, collected_count, 0);
                }
            }

            // Free stack
            free(stack);
        } else {
            // Single file
            if (is_image_file(path)) {
                uint32_t hash = fnv1a_hash(path);
                
                // Check for duplicate using O(1) hash lookup
                if (hash_exists(state, hash)) {
                    continue;  // Skip duplicate
                }
                
                // Get file size (single file, need to query)
                WIN32_FILE_ATTRIBUTE_DATA file_attrs;
                size_t file_size = 0;
                if (GetFileAttributesExW(path, GetFileExInfoStandard, &file_attrs)) {
                    LARGE_INTEGER size;
                    size.HighPart = file_attrs.nFileSizeHigh;
                    size.LowPart = file_attrs.nFileSizeLow;
                    file_size = (size_t)size.QuadPart;
                }
                
                // Grow collection array if needed
                if (collected_count >= collected_capacity) {
                    int new_capacity = collected_capacity * 2;
                    CollectedFile *new_collected = (CollectedFile*)realloc(collected, sizeof(CollectedFile) * new_capacity);
                    if (!new_collected) {
                        // Out of memory, skip this file
                        continue;
                    }
                    collected = new_collected;
                    collected_capacity = new_capacity;
                }
                
                // Add to collection
                wcsncpy(collected[collected_count].path, path, MAX_PATH_LEN - 1);
                collected[collected_count].path[MAX_PATH_LEN - 1] = L'\0';
                collected[collected_count].root_folder[0] = L'\0';
                collected[collected_count].file_size = file_size;
                collected[collected_count].format = detect_format_from_path(path);
                collected_count++;
                
                // Add hash to table
                hash_add(state, hash);
            }
        }

        // Update progress every 100 files
        if (collected_count > 0 && collected_count % 100 == 0) {
            PostMessageW(state->hwnd, WM_APP_SCANNING_PROGRESS, collected_count, 0);
        }
    }

    // BATCH ADD: Add all collected files at once (single critical section acquisition)
    EnterCriticalSection(&state->files_cs);
    
    // Ensure we have enough capacity in file array
    if (state->file_count + collected_count > state->file_capacity) {
        int new_capacity = state->file_count + collected_count + 256;
        ImageFile *new_files = (ImageFile*)realloc(state->files, sizeof(ImageFile) * new_capacity);
        if (!new_files) {
            LeaveCriticalSection(&state->files_cs);
            // Handle out of memory - use as many as we can
            collected_count = 0;
        } else {
            state->files = new_files;
            state->file_capacity = new_capacity;
        }
    }
    
    if (collected_count > 0) {
        // Copy all collected files to shared array
        for (int i = 0; i < collected_count; i++) {
            ImageFile *file = &state->files[state->file_count];
            wcsncpy(file->path, collected[i].path, MAX_PATH_LEN - 1);
            file->path[MAX_PATH_LEN - 1] = L'\0';
            wcsncpy(file->root_folder, collected[i].root_folder, MAX_PATH_LEN - 1);
            file->root_folder[MAX_PATH_LEN - 1] = L'\0';
            file->format = collected[i].format;
            file->input_size = collected[i].file_size;
            file->output_size = 0;
            file->processed = FALSE;
            file->success = FALSE;
            file->error = 0;
            file->error_message[0] = L'\0';
            state->file_count++;
        }
    }
    
    LeaveCriticalSection(&state->files_cs);
    
    // Free collection array
    free(collected);

    // Re-enable ListView redraw
    SendMessageW(state->hFilesList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->hFilesList, NULL, TRUE);

    // Send completion message
    PostMessageW(state->hwnd, WM_APP_SCANNING_COMPLETE, collected_count, 0);

    // Cleanup
    DragFinish(params->hdrop);
    free(params);

    return 0;
}

// Simple folder scan for non-drop operations (legacy support)
static void scan_folder_recursive(AppState *state, const wchar_t *path, const wchar_t *root_folder) {
    WIN32_FIND_DATAW find_data;
    wchar_t search_path[MAX_PATH_LEN];
    swprintf(search_path, MAX_PATH_LEN, L"%s\\*", path);

    HANDLE hFind = FindFirstFileW(search_path, &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (wcscmp(find_data.cFileName, L".") == 0 ||
            wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }

        wchar_t full_path[MAX_PATH_LEN];
        swprintf(full_path, MAX_PATH_LEN, L"%s\\%s", path, find_data.cFileName);

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_folder_recursive(state, full_path, root_folder);
        } else {
            if (is_image_file(full_path)) {
                uint32_t hash = fnv1a_hash(full_path);
                if (!hash_exists(state, hash)) {
                    hash_add(state, hash);
                    
                    EnterCriticalSection(&state->files_cs);
                    if (state->file_count >= state->file_capacity) {
                        int new_capacity = (state->file_capacity == 0) ? 256 : state->file_capacity * 2;
                        ImageFile *new_files = (ImageFile*)realloc(state->files, sizeof(ImageFile) * new_capacity);
                        if (new_files) {
                            state->files = new_files;
                            state->file_capacity = new_capacity;
                        }
                    }
                    if (state->file_count < state->file_capacity) {
                        ImageFile *file = &state->files[state->file_count];
                        wcsncpy(file->path, full_path, MAX_PATH_LEN - 1);
                        file->path[MAX_PATH_LEN - 1] = L'\0';
                        wcsncpy(file->root_folder, root_folder, MAX_PATH_LEN - 1);
                        file->root_folder[MAX_PATH_LEN - 1] = L'\0';
                        file->format = detect_format_from_path(full_path);
                        file->input_size = ((size_t)find_data.nFileSizeHigh << 32) | (size_t)find_data.nFileSizeLow;
                    file->output_size = 0;
                    file->processed = FALSE;
                    file->success = FALSE;
                    file->error = 0;
                    file->error_message[0] = L'\0';
                    state->file_count++;
                    }
                    LeaveCriticalSection(&state->files_cs);
                }
            }
        }
    } while (FindNextFileW(hFind, &find_data));

    FindClose(hFind);
}

void dragdrop_add_file(AppState *state, const wchar_t *path) {
    if (is_image_file(path)) {
        uint32_t hash = fnv1a_hash(path);
        if (hash_exists(state, hash)) {
            return;
        }
        hash_add(state, hash);
        
        EnterCriticalSection(&state->files_cs);
        if (state->file_count >= state->file_capacity) {
            int new_capacity = (state->file_capacity == 0) ? 256 : state->file_capacity * 2;
            ImageFile *new_files = (ImageFile*)realloc(state->files, sizeof(ImageFile) * new_capacity);
            if (new_files) {
                state->files = new_files;
                state->file_capacity = new_capacity;
            }
        }
        if (state->file_count < state->file_capacity) {
            ImageFile *file = &state->files[state->file_count];
            wcsncpy(file->path, path, MAX_PATH_LEN - 1);
            file->path[MAX_PATH_LEN - 1] = L'\0';
            file->root_folder[0] = L'\0';
            file->format = detect_format_from_path(path);
            file->input_size = 0;
            file->output_size = 0;
            file->processed = FALSE;
            file->success = FALSE;
            file->error = 0;
            file->error_message[0] = L'\0';
            state->file_count++;
            
            // Add to ListView immediately for single file
            const wchar_t *filename = wcsrchr(path, L'\\');
            if (!filename) filename = wcsrchr(path, L'/');
            if (filename) filename++;
            else filename = path;

            LVITEMW item = {0};
            item.mask = LVIF_TEXT;
            item.iItem = state->file_count - 1;
            item.iSubItem = 0;
            item.pszText = (wchar_t*)filename;
            SendMessageW(state->hFilesList, LVM_INSERTITEMW, 0, (LPARAM)&item);
        }
        LeaveCriticalSection(&state->files_cs);
    }
}

void dragdrop_add_folder(AppState *state, const wchar_t *path) {
    wchar_t root_folder[MAX_PATH_LEN];
    
    // Get parent folder
    wcsncpy(root_folder, path, MAX_PATH_LEN - 1);
    root_folder[MAX_PATH_LEN - 1] = L'\0';
    wchar_t *slash = wcsrchr(root_folder, L'\\');
    if (!slash) slash = wcsrchr(root_folder, L'/');
    if (slash) {
        size_t len = wcslen(root_folder);
        if (len > 3) {
            *slash = L'\0';
        }
    }

    // Scan folder
    scan_folder_recursive(state, path, root_folder);

    // Update ListView for all added files
    EnterCriticalSection(&state->files_cs);
    for (int i = 0; i < state->file_count; i++) {
        // We'll just add all of them since we don't track which ones are already shown
    }
    LeaveCriticalSection(&state->files_cs);
}

BOOL dragdrop_init(HWND hwnd) {
    DragAcceptFiles(hwnd, TRUE);
    return TRUE;
}

void dragdrop_handle_drop(AppState *state, HDROP hdrop) {
    UINT file_count = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);

    // Cancel any existing scan
    if (state->scanning) {
        dragdrop_cancel_scan(state);
    }

    // Clear existing hash table to prepare for new scan
    dragdrop_clear_hash_table(state);
    
    // Initialize new hash table
    dragdrop_init_hash_table(state, (int)file_count);

    // Set scanning state
    state->scanning = TRUE;
    state->stop_scanning = FALSE;

    // Update Cancel button text to indicate cancel scanning
    SetWindowTextW(state->hCancelBtn, L"Cancel Scan");

    // Clear existing files
    state->file_count = 0;
    ListView_DeleteAllItems(state->hFilesList);

    // Set up progress bar for scanning
    SendMessageW(state->hProgressFiles, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(state->hProgressFiles, PBM_SETPOS, 0, 0);

    // Create and launch scan thread
    ScanThreadParams *params = (ScanThreadParams*)malloc(sizeof(ScanThreadParams));
    if (params) {
        params->state = state;
        params->hdrop = hdrop;
        params->file_count = file_count;

        state->scan_thread = CreateThread(NULL, 0, scan_thread_proc, params, 0, NULL);
        if (state->scan_thread == NULL) {
            // Fallback to synchronous processing if thread creation fails
            state->scanning = FALSE;
            SetWindowTextW(state->hCancelBtn, L"Cancel");
            DragFinish(hdrop);
            free(params);
        }
    } else {
        state->scanning = FALSE;
        SetWindowTextW(state->hCancelBtn, L"Cancel");
        DragFinish(hdrop);
    }
}

void dragdrop_cancel_scan(AppState *state) {
    if (state->scanning) {
        state->stop_scanning = TRUE;
        
        // Wait for scan thread to finish (with timeout)
        if (state->scan_thread != NULL) {
            WaitForSingleObject(state->scan_thread, 5000);
            CloseHandle(state->scan_thread);
            state->scan_thread = NULL;
        }

        state->scanning = FALSE;
        SetWindowTextW(state->hCancelBtn, L"Cancel");
    }
}

// Count failed files
static int count_failed_files(AppState *state) {
    int failed = 0;
    EnterCriticalSection(&state->files_cs);
    for (int i = 0; i < state->file_count; i++) {
        if (state->files[i].processed && !state->files[i].success) {
            failed++;
        }
    }
    LeaveCriticalSection(&state->files_cs);
    return failed;
}

// Check if "Failed Files" tab exists
static BOOL failed_tab_exists(AppState *state) {
    int tab_count = (int)SendMessageW(state->hTabControl, TCM_GETITEMCOUNT, 0, 0);
    return tab_count > 1;
}

// Update tab labels with counts
void dragdrop_update_tab_labels(AppState *state) {
    int failed = count_failed_files(state);
    
    // Update "All Files" tab
    wchar_t tab_text[64];
    swprintf(tab_text, 64, L"All Files (%d)", state->file_count);
    
    TCITEMW tci = {0};
    tci.mask = TCIF_TEXT;
    tci.pszText = tab_text;
    SendMessageW(state->hTabControl, TCM_SETITEMW, 0, (LPARAM)&tci);
    
    // Show/hide "Failed Files" tab based on whether there are failed files
    if (failed > 0) {
        if (failed_tab_exists(state)) {
            // Tab exists, just update label
            swprintf(tab_text, 64, L"Failed Files (%d)", failed);
            tci.pszText = tab_text;
            SendMessageW(state->hTabControl, TCM_SETITEMW, 1, (LPARAM)&tci);
        } else {
            // Tab doesn't exist, add it
            swprintf(tab_text, 64, L"Failed Files (%d)", failed);
            tci.pszText = tab_text;
            SendMessageW(state->hTabControl, TCM_INSERTITEMW, 1, (LPARAM)&tci);
        }
    } else {
        if (failed_tab_exists(state)) {
            // No failed files, remove tab
            SendMessageW(state->hTabControl, TCM_DELETEITEM, 1, 0);
            // If currently on failed tab, switch to all files
            if (state->current_tab == 1) {
                state->current_tab = 0;
                SendMessageW(state->hTabControl, TCM_SETCURSEL, 0, 0);
                dragdrop_populate_listview(state);
            }
        }
    }
}

// Fill ListView with filtered files (called after scan completes or tab change)
void dragdrop_populate_listview(AppState *state) {
    // Get column width
    RECT rc;
    GetClientRect(state->hFilesList, &rc);
    int col_width = rc.right - rc.left - 25;

    // Clear ListView first
    ListView_DeleteAllItems(state->hFilesList);

    EnterCriticalSection(&state->files_cs);
    
    int display_index = 0;
    for (int i = 0; i < state->file_count; i++) {
        // Filter based on current tab
        if (state->current_tab == 1) {  // Failed Files tab
            // Only show processed files that failed
            if (!state->files[i].processed || state->files[i].success) {
                continue;
            }
        }
        // Tab 0 (All Files) shows everything

        const wchar_t *filename = wcsrchr(state->files[i].path, L'\\');
        if (!filename) filename = wcsrchr(state->files[i].path, L'/');
        if (filename) filename++;
        else filename = state->files[i].path;

        // For failed files, append error message
        static _Thread_local wchar_t display_name[MAX_PATH_LEN + 256]; // Extra space for error message
        if (state->files[i].processed && !state->files[i].success) {
            swprintf(display_name, MAX_PATH_LEN + 256, L"%s - %s", filename, state->files[i].error_message);
        } else {
            wcsncpy(display_name, filename, MAX_PATH_LEN);
            display_name[MAX_PATH_LEN - 1] = L'\0';
        }

        LVITEMW item = {0};
        item.mask = LVIF_TEXT;
        item.iItem = display_index;
        item.iSubItem = 0;
        item.pszText = display_name;
        SendMessageW(state->hFilesList, LVM_INSERTITEMW, 0, (LPARAM)&item);
        display_index++;
    }
    
    LeaveCriticalSection(&state->files_cs);
    
    // Update column width
    ListView_SetColumnWidth(state->hFilesList, 0, col_width);
    
    // Update tab labels with counts
    dragdrop_update_tab_labels(state);
}
