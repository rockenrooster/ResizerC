// dll_loader.c - Embedded DLL loader implementation
#include "dll_loader.h"
#include "utils.h"
#include "resource.h"
#include <stdio.h>
#include <shlwapi.h>

// Resource IDs for embedded DLLs (defined in resource.h)
typedef struct {
    int resource_id;
    const wchar_t *filename;
} EmbeddedDLL;

// List of all 62 embedded DLLs
static const EmbeddedDLL g_embedded_dlls[] = {
    { IDR_DLL_libaom, L"libaom.dll" },
    { IDR_DLL_libarchive_13, L"libarchive-13.dll" },
    { IDR_DLL_libbrotlicommon, L"libbrotlicommon.dll" },
    { IDR_DLL_libbrotlidec, L"libbrotlidec.dll" },
    { IDR_DLL_libbrotlienc, L"libbrotlienc.dll" },
    { IDR_DLL_libc__, L"libc++.dll" },
    { IDR_DLL_libcairo_2, L"libcairo-2.dll" },
    { IDR_DLL_libcfitsio, L"libcfitsio.dll" },
    { IDR_DLL_libcgif_0, L"libcgif-0.dll" },
    { IDR_DLL_libdicom_1, L"libdicom-1.dll" },
    { IDR_DLL_libexif_12, L"libexif-12.dll" },
    { IDR_DLL_libexpat_1, L"libexpat-1.dll" },
    { IDR_DLL_libffi_8, L"libffi-8.dll" },
    { IDR_DLL_libfftw3_3, L"libfftw3-3.dll" },
    { IDR_DLL_libfontconfig_1, L"libfontconfig-1.dll" },
    { IDR_DLL_libfreetype_6, L"libfreetype-6.dll" },
    { IDR_DLL_libfribidi_0, L"libfribidi-0.dll" },
    { IDR_DLL_libgdk_pixbuf_2_0_0, L"libgdk_pixbuf-2.0-0.dll" },
    { IDR_DLL_libgio_2_0_0, L"libgio-2.0-0.dll" },
    { IDR_DLL_libglib_2_0_0, L"libglib-2.0-0.dll" },
    { IDR_DLL_libgmodule_2_0_0, L"libgmodule-2.0-0.dll" },
    { IDR_DLL_libgobject_2_0_0, L"libgobject-2.0-0.dll" },
    { IDR_DLL_libharfbuzz_0, L"libharfbuzz-0.dll" },
    { IDR_DLL_libheif, L"libheif.dll" },
    { IDR_DLL_libhwy, L"libhwy.dll" },
    { IDR_DLL_libIex_3_1, L"libIex-3_1.dll" },
    { IDR_DLL_libIlmThread_3_1, L"libIlmThread-3_1.dll" },
    { IDR_DLL_libimagequant, L"libimagequant.dll" },
    { IDR_DLL_libjpeg_62, L"libjpeg-62.dll" },
    { IDR_DLL_libjxl_cms, L"libjxl_cms.dll" },
    { IDR_DLL_libjxl_threads, L"libjxl_threads.dll" },
    { IDR_DLL_libjxl, L"libjxl.dll" },
    { IDR_DLL_liblcms2_2, L"liblcms2-2.dll" },
    { IDR_DLL_libMagickCore_6_Q16_7, L"libMagickCore-6.Q16-7.dll" },
    { IDR_DLL_libmatio_13, L"libmatio-13.dll" },
    { IDR_DLL_libniftiio, L"libniftiio.dll" },
    { IDR_DLL_libOpenEXR_3_1, L"libOpenEXR-3_1.dll" },
    { IDR_DLL_libopenjp2, L"libopenjp2.dll" },
    { IDR_DLL_libopenslide_1, L"libopenslide-1.dll" },
    { IDR_DLL_libpango_1_0_0, L"libpango-1.0-0.dll" },
    { IDR_DLL_libpangocairo_1_0_0, L"libpangocairo-1.0-0.dll" },
    { IDR_DLL_libpangoft2_1_0_0, L"libpangoft2-1.0-0.dll" },
    { IDR_DLL_libpixman_1_0, L"libpixman-1-0.dll" },
    { IDR_DLL_libpng16_16, L"libpng16-16.dll" },
    { IDR_DLL_libpoppler_155, L"libpoppler-155.dll" },
    { IDR_DLL_libpoppler_glib_8, L"libpoppler-glib-8.dll" },
    { IDR_DLL_libraw_r_23, L"libraw_r-23.dll" },
    { IDR_DLL_librsvg_2_2, L"librsvg-2-2.dll" },
    { IDR_DLL_libsharpyuv_0, L"libsharpyuv-0.dll" },
    { IDR_DLL_libsqlite3_0, L"libsqlite3-0.dll" },
    { IDR_DLL_libtiff_6, L"libtiff-6.dll" },
    { IDR_DLL_libuhdr, L"libuhdr.dll" },
    { IDR_DLL_libunwind, L"libunwind.dll" },
    { IDR_DLL_libvips_42, L"libvips-42.dll" },
    { IDR_DLL_libvips_cpp_42, L"libvips-cpp-42.dll" },
    { IDR_DLL_libwebp_7, L"libwebp-7.dll" },
    { IDR_DLL_libwebpdemux_2, L"libwebpdemux-2.dll" },
    { IDR_DLL_libwebpmux_3, L"libwebpmux-3.dll" },
    { IDR_DLL_libxml2_16, L"libxml2-16.dll" },
    { IDR_DLL_libz1, L"libz1.dll" },
    { IDR_DLL_libznz, L"libznz.dll" },
    { IDR_DLL_libzstd, L"libzstd.dll" },
};

static wchar_t g_temp_dir[MAX_PATH] = {0};
static BOOL g_initialized = FALSE;
static HINSTANCE g_hinstance = NULL;

// Extract a single DLL from resources to temp directory
static BOOL extract_dll(int resource_id, const wchar_t *filename, const wchar_t *temp_dir) {
    // Find the resource - use GetModuleHandleW(NULL) to get current exe handle
    HMODULE hModule = GetModuleHandleW(NULL);
    HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resource_id), L"DLL");

    if (!hResource) {
        // Resource not found - silently fail
        return FALSE;
    }

    // Load the resource
    HGLOBAL hLoadedResource = LoadResource(hModule, hResource);
    if (!hLoadedResource) {
        // Failed to load resource
        return FALSE;
    }

    // Lock the resource to get a pointer to the data
    void *pResourceData = LockResource(hLoadedResource);
    DWORD dwResourceSize = SizeofResource(hModule, hResource);
    if (!pResourceData || dwResourceSize == 0) {
        // Failed to lock resource
        return FALSE;
    }

    // Create the output file path
    wchar_t output_path[MAX_PATH];
    swprintf(output_path, MAX_PATH, L"%s\\%s", temp_dir, filename);

    // Write the resource data to the file
    HANDLE hFile = CreateFileW(output_path, GENERIC_WRITE, 0, NULL, 
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        // Failed to create output file
        return FALSE;
    }

    DWORD bytesWritten;
    BOOL success = WriteFile(hFile, pResourceData, dwResourceSize, &bytesWritten, NULL);
    CloseHandle(hFile);

    if (!success || bytesWritten != dwResourceSize) {
        // Failed to write complete file
        DeleteFileW(output_path);
        return FALSE;
    }

    // Successfully extracted DLL
    return TRUE;
}

// Create a unique temp directory for this instance
static BOOL create_temp_directory(wchar_t *output_path, size_t output_size) {
    wchar_t temp_path[MAX_PATH];
    
    // Get system temp directory
    if (GetTempPathW(MAX_PATH, temp_path) == 0) {
        // Failed to get temp path
        return FALSE;
    }

    // Create a unique subdirectory for our app
    // Use process ID to make it unique per instance
    DWORD pid = GetCurrentProcessId();
    swprintf(output_path, output_size, L"%sResizerC_vips_%lu", temp_path, pid);

    // Create the directory
    if (!CreateDirectoryW(output_path, NULL)) {
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            // Failed to create temp directory
            return FALSE;
        }
    }

    // Created temp directory successfully
    return TRUE;
}

// Delete directory and all files in it
static void delete_temp_directory(const wchar_t *dir_path) {
    if (!dir_path || dir_path[0] == 0) return;

    // Delete all files in the directory
    wchar_t search_path[MAX_PATH];
    swprintf(search_path, MAX_PATH, L"%s\\*", dir_path);

    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FindFirstFileW(search_path, &find_data);
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(find_data.cFileName, L".") != 0 && 
                wcscmp(find_data.cFileName, L"..") != 0) {
                wchar_t file_path[MAX_PATH];
                swprintf(file_path, MAX_PATH, L"%s\\%s", dir_path, find_data.cFileName);
                DeleteFileW(file_path);
            }
        } while (FindNextFileW(hFind, &find_data));
        FindClose(hFind);
    }

    // Remove the directory itself
    RemoveDirectoryW(dir_path);
    // Cleaned up temp directory
}

BOOL dll_loader_init(HINSTANCE hinstance) {
    g_hinstance = hinstance;
    
    if (g_initialized) {
        return TRUE;
    }

    // Initializing DLL loader

    // Create temp directory
    if (!create_temp_directory(g_temp_dir, MAX_PATH)) {
        return FALSE;
    }

    // Extract all embedded DLLs
    int dll_count = sizeof(g_embedded_dlls) / sizeof(g_embedded_dlls[0]);
    int success_count = 0;

    for (int i = 0; i < dll_count; i++) {
        if (extract_dll(g_embedded_dlls[i].resource_id, 
                       g_embedded_dlls[i].filename, 
                       g_temp_dir)) {
            success_count++;
        } else {
            // Failed to extract this DLL
        }
    }

    if (success_count == 0) {
        // No DLLs extracted
        delete_temp_directory(g_temp_dir);
        g_temp_dir[0] = 0;
        return FALSE;
    }

    // Add temp directory to DLL search path
    // This allows Windows to find the DLLs when vips tries to load them
    if (!SetDllDirectoryW(g_temp_dir)) {
        // Failed to set DLL directory
        delete_temp_directory(g_temp_dir);
        g_temp_dir[0] = 0;
        return FALSE;
    }

    // Also add to PATH environment variable as backup
    wchar_t current_path[8192];
    DWORD path_len = GetEnvironmentVariableW(L"PATH", current_path, 8192);
    if (path_len > 0 && path_len < 8192) {
        wchar_t new_path[8192];
        swprintf(new_path, 8192, L"%s;%s", g_temp_dir, current_path);
        SetEnvironmentVariableW(L"PATH", new_path);
    }

    g_initialized = TRUE;
    return TRUE;
}

void dll_loader_cleanup(void) {
    if (!g_initialized) {
        return;
    }

    // Cleaning up DLLs

    // Reset DLL directory
    SetDllDirectoryW(NULL);

    // Delete temp directory and all extracted DLLs
    delete_temp_directory(g_temp_dir);
    
    g_temp_dir[0] = 0;
    g_initialized = FALSE;
}

const wchar_t* dll_loader_get_temp_dir(void) {
    return g_temp_dir;
}
