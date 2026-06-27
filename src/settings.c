#include "settings.h"
#include <shlobj.h>

static const wchar_t SETTINGS_FILENAME[] = L"ResizerC.ini";

void get_settings_path(wchar_t *path, size_t path_size) {
    // Get AppData folder
    wchar_t appdata[MAX_PATH_LEN];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
        swprintf(path, path_size, L"%s\\%s", appdata, SETTINGS_FILENAME);
    } else {
        // Fallback to current directory
        GetCurrentDirectoryW(path_size, path);
        wcscat(path, L"\\");
        wcscat(path, SETTINGS_FILENAME);
    }
}

void settings_init_defaults(AppState *state) {
    // Default output directory is desktop
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, state->output_dir) != S_OK) {
        GetCurrentDirectoryW(MAX_PATH_LEN, state->output_dir);
    }

    state->output_format = FORMAT_WEBP; // Default to WebP for best compression
    state->quality = 85;
    state->resolution_percent = 100;
    state->max_resolution = 0;
    state->num_threads = get_logical_core_count();
    state->preserve_folder_structure = FALSE;
    state->enable_logging = TRUE;
    state->webp_method = 4;  // Default: balanced speed/compression (0-6 scale)
}

BOOL settings_save(AppState *state) {
    wchar_t path[MAX_PATH_LEN];
    get_settings_path(path, MAX_PATH_LEN);

    FILE *f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return FALSE;

    fwprintf(f, L"[Settings]\n");
    fwprintf(f, L"OutputDir=%s\n", state->output_dir);
    fwprintf(f, L"OutputFormat=%d\n", (int)state->output_format);
    fwprintf(f, L"Quality=%d\n", state->quality);
    fwprintf(f, L"ResolutionPercent=%d\n", state->resolution_percent);
    fwprintf(f, L"MaxResolution=%d\n", state->max_resolution);
    fwprintf(f, L"NumThreads=%d\n", state->num_threads);
    fwprintf(f, L"PreserveFolderStructure=%d\n", state->preserve_folder_structure ? 1 : 0);
    fwprintf(f, L"EnableLogging=%d\n", state->enable_logging ? 1 : 0);

    fclose(f);
    return TRUE;
}

BOOL settings_load(AppState *state) {
    wchar_t path[MAX_PATH_LEN];
    get_settings_path(path, MAX_PATH_LEN);

    settings_init_defaults(state);

    FILE *f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) {
        return FALSE;
    }

    wchar_t line[4096];
    wchar_t section[64] = {0};

    while (fgetws(line, sizeof(line)/sizeof(wchar_t), f)) {
        // Remove trailing newline
        size_t len = wcslen(line);
        if (len > 0 && line[len-1] == L'\n') line[len-1] = L'\0';
        if (len > 1 && line[len-2] == L'\r') line[len-2] = L'\0';

        // Skip empty lines and comments
        if (line[0] == L'\0' || line[0] == L';' || line[0] == L'#') continue;

        // Section header
        if (line[0] == L'[') {
            wchar_t *end = wcschr(line, L']');
            if (end) {
                *end = L'\0';
                wcscpy(section, line + 1);
            }
            continue;
        }

        // Key=value pair
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;

        *eq = L'\0';
        wchar_t *key = line;
        wchar_t *value = eq + 1;

        // Trim whitespace
        while (*key == L' ' || *key == L'\t') key++;
        while (*value == L' ' || *value == L'\t') value++;

        if (wcscmp(section, L"Settings") == 0) {
            if (wcscmp(key, L"OutputDir") == 0) {
                wcscpy(state->output_dir, value);
            } else if (wcscmp(key, L"OutputFormat") == 0) {
                state->output_format = (ImageFormat)_wtoi(value);
            } else if (wcscmp(key, L"Quality") == 0) {
                state->quality = _wtoi(value);
            } else if (wcscmp(key, L"ResolutionPercent") == 0) {
                state->resolution_percent = _wtoi(value);
                // Clamp to valid range (10-100%)
                if (state->resolution_percent < 10) state->resolution_percent = 10;
                if (state->resolution_percent > 100) state->resolution_percent = 100;
            } else if (wcscmp(key, L"MaxResolution") == 0) {
                state->max_resolution = _wtoi(value);
                if (state->max_resolution < 0) state->max_resolution = 0;
            } else if (wcscmp(key, L"NumThreads") == 0) {
                state->num_threads = _wtoi(value);
                // Clamp to valid range (1 to logical core count)
                int max_cores = get_logical_core_count();
                if (state->num_threads < 1) state->num_threads = 1;
                if (state->num_threads > max_cores) state->num_threads = max_cores;
            } else if (wcscmp(key, L"PreserveFolderStructure") == 0) {
                state->preserve_folder_structure = (_wtoi(value) != 0);
            } else if (wcscmp(key, L"EnableLogging") == 0) {
                state->enable_logging = (_wtoi(value) != 0);
            }
        }
    }

    fclose(f);
    return TRUE;
}

void settings_apply_to_ui(AppState *state) {
    // These will be implemented when we create the UI
    // For now, this is a placeholder
}

void settings_get_from_ui(AppState *state) {
    // These will be implemented when we create the UI
    // For now, this is a placeholder
}
