#include "window.h"
#include "settings.h"
#include "resource.h"
#include "utils.h"
#include "image.h"
#include "dll_loader.h"
#include <commctrl.h>

// Global application state
static AppState g_state = {0};

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hprevinstance, LPWSTR cmdline, int cmdshow) {
    (void)hprevinstance;
    (void)cmdline;

    // Set process priority to below normal
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    // Initialize COM for drag-drop
    CoInitialize(NULL);

    // Initialize common controls
    INITCOMMONCONTROLSEX icc = {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    // Initialize critical sections
    InitializeCriticalSection(&g_state.files_cs);
    InitializeCriticalSection(&g_state.hash_cs);

    // Initialize state
    g_state.hinstance = hinstance;
    g_state.file_capacity = 0;
    g_state.file_count = 0;
    g_state.files = NULL;
    g_state.processing = FALSE;
    g_state.stop_processing = FALSE;
    g_state.threads = NULL;
    g_state.frequency.QuadPart = 0;
    
    // Initialize hash table
    g_state.hash_buckets = NULL;
    g_state.hash_bucket_count = 0;
    g_state.hash_bucket_mask = 0;
    g_state.scanning = FALSE;
    g_state.stop_scanning = FALSE;
    g_state.scan_thread = NULL;
    
    // Initialize hash table for drag-drop
    dragdrop_init_hash_table(&g_state, 256);  // Small initial estimate

    // Load settings
    settings_load(&g_state);

    // Initialize logging (only if enabled)
    if (g_state.enable_logging) {
        log_init();
        log_message(L"Application starting");
    }

    // Initialize embedded DLL loader FIRST (before vips init)
    if (!dll_loader_init(hinstance)) {
        MessageBoxW(NULL, 
                   L"Failed to extract embedded DLLs.\nPlease check write permissions to TEMP folder.", 
                   L"ResizerC - Initialization Error", 
                   MB_OK | MB_ICONERROR);
        if (g_state.enable_logging) {
            log_error(L"Failed to initialize DLL loader");
            log_close();
        }
        return 1;
    }

#ifdef HAVE_VIPS
    if (!image_init_vips()) {
        log_error(L"Failed to initialize libvips");
        MessageBoxW(NULL, 
                   L"Failed to initialize libvips library.", 
                   L"ResizerC - Initialization Error", 
                   MB_OK | MB_ICONWARNING);
        dll_loader_cleanup();
    }
#endif

    // Create window
    HWND hwnd = window_create(hinstance, &g_state);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Set window as main window
    g_state.hwnd = hwnd;

    // Show window
    ShowWindow(hwnd, cmdshow);
    UpdateWindow(hwnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Cleanup
    if (g_state.files) free(g_state.files);
    dragdrop_clear_hash_table(&g_state);  // Clean up hash table
    DeleteCriticalSection(&g_state.hash_cs);
    if (g_state.threads) {
        for (int i = 0; i < g_state.num_threads; i++) {
            if (g_state.threads[i]) CloseHandle(g_state.threads[i]);
        }
        free(g_state.threads);
    }
    if (g_state.scan_thread) {
        CloseHandle(g_state.scan_thread);
    }
    DeleteCriticalSection(&g_state.files_cs);

    // Save settings
    settings_save(&g_state);

    if (g_state.enable_logging) {
        log_message(L"Application shutting down");
    }
    log_close();

#ifdef HAVE_VIPS
    image_shutdown_vips();
#endif

    dll_loader_cleanup();

    CoUninitialize();

    return (int)msg.wParam;
}
