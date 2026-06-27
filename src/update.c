#include "update.h"
#include <wininet.h>

#define UPDATE_USER_AGENT L"ResizerC/" APP_VERSION

#ifndef RESIZERC_GITHUB_OWNER_TEXT
#define RESIZERC_GITHUB_OWNER_TEXT "OWNER"
#endif

#ifndef RESIZERC_GITHUB_REPO_TEXT
#define RESIZERC_GITHUB_REPO_TEXT "ResizerC"
#endif

#define GITHUB_OWNER WIDEN_LITERAL(RESIZERC_GITHUB_OWNER_TEXT)
#define GITHUB_REPO WIDEN_LITERAL(RESIZERC_GITHUB_REPO_TEXT)

static const wchar_t UPDATE_CHECK_URL[] =
    L"https://api.github.com/repos/" GITHUB_OWNER L"/" GITHUB_REPO L"/releases/latest";
static const wchar_t UPDATE_RELEASE_URL[] =
    L"https://github.com/" GITHUB_OWNER L"/" GITHUB_REPO L"/releases/latest";

static wchar_t g_latest_release_url[1024];

static BOOL http_get(const wchar_t *url, wchar_t *response, size_t response_size) {
    HINTERNET hInternet = NULL;
    HINTERNET hUrl = NULL;
    BOOL result = FALSE;
    const wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n";

    hInternet = InternetOpenW(UPDATE_USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) goto cleanup;

    hUrl = InternetOpenUrlW(hInternet, url, headers, (DWORD)-1,
                            INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) goto cleanup;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &status_size, NULL) && (status < 200 || status >= 300)) {
        goto cleanup;
    }

    char buffer[65536];
    DWORD bytes_read;
    size_t total_read = 0;

    if (response_size == 0) goto cleanup;
    response[0] = L'\0';

    while (total_read < sizeof(buffer) - 1 &&
           InternetReadFile(hUrl, buffer + total_read, (DWORD)(sizeof(buffer) - 1 - total_read), &bytes_read) &&
           bytes_read > 0) {
        total_read += bytes_read;
    }
    buffer[total_read] = '\0';

    if (MultiByteToWideChar(CP_UTF8, 0, buffer, -1, response, (int)response_size) == 0) {
        goto cleanup;
    }
    response[response_size - 1] = L'\0';

    result = TRUE;

cleanup:
    if (hUrl) InternetCloseHandle(hUrl);
    if (hInternet) InternetCloseHandle(hInternet);

    return result;
}

static BOOL http_download_file(const wchar_t *url, const wchar_t *path) {
    HINTERNET hInternet = NULL;
    HINTERNET hUrl = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    BOOL result = FALSE;

    hInternet = InternetOpenW(UPDATE_USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) goto cleanup;

    hUrl = InternetOpenUrlW(hInternet, url, NULL, 0,
                            INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) goto cleanup;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &status_size, NULL) && (status < 200 || status >= 300)) {
        goto cleanup;
    }

    hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) goto cleanup;

    char buffer[65536];
    DWORD bytes_read = 0;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0) {
        DWORD bytes_written = 0;
        if (!WriteFile(hFile, buffer, bytes_read, &bytes_written, NULL) || bytes_written != bytes_read) {
            goto cleanup;
        }
    }

    result = TRUE;

cleanup:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (!result) DeleteFileW(path);
    if (hUrl) InternetCloseHandle(hUrl);
    if (hInternet) InternetCloseHandle(hInternet);

    return result;
}

static BOOL github_updates_configured(void) {
    return GITHUB_OWNER[0] != L'\0' && GITHUB_REPO[0] != L'\0' && wcscmp(GITHUB_OWNER, L"OWNER") != 0;
}

static BOOL json_get_string(const wchar_t *json, const wchar_t *key, wchar_t *out, size_t out_size) {
    const wchar_t *p = wcsstr(json, key);
    if (!p || out_size == 0) return FALSE;

    p = wcschr(p + wcslen(key), L':');
    if (!p) return FALSE;
    p = wcschr(p, L'"');
    if (!p) return FALSE;
    p++;

    size_t i = 0;
    while (*p && *p != L'"' && i < out_size - 1) {
        if (*p == L'\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = L'\0';
    return i > 0;
}

static BOOL json_get_exe_asset_url(const wchar_t *json, wchar_t *out, size_t out_size) {
    const wchar_t *p = json;

    while ((p = wcsstr(p, L"\"browser_download_url\"")) != NULL) {
        wchar_t url[2048];
        if (json_get_string(p, L"\"browser_download_url\"", url, sizeof(url)/sizeof(wchar_t))) {
            wchar_t lower[2048];
            wcscpy_s(lower, sizeof(lower)/sizeof(wchar_t), url);
            for (wchar_t *c = lower; *c; c++) {
                if (*c >= L'A' && *c <= L'Z') *c = *c - L'A' + L'a';
            }
            if (wcsstr(lower, L".exe")) {
                wcscpy_s(out, out_size, url);
                return TRUE;
            }
        }
        p++;
    }

    return FALSE;
}

static BOOL parse_version(const wchar_t *version, int *major, int *minor, int *patch) {
    while (*version == L' ' || *version == L'\t') version++;
    if (*version == L'v' || *version == L'V') version++;

    *major = 0;
    *minor = 0;
    *patch = 0;
    return swscanf(version, L"%d.%d.%d", major, minor, patch) >= 2;
}

static BOOL compare_versions(const wchar_t *current, const wchar_t *available) {
    int curr_major, curr_minor, curr_patch;
    int avail_major, avail_minor, avail_patch;

    if (!parse_version(current, &curr_major, &curr_minor, &curr_patch)) return FALSE;
    if (!parse_version(available, &avail_major, &avail_minor, &avail_patch)) return FALSE;

    if (avail_major > curr_major) return TRUE;
    if (avail_major == curr_major && avail_minor > curr_minor) return TRUE;
    if (avail_major == curr_major && avail_minor == curr_minor && avail_patch > curr_patch) return TRUE;

    return FALSE;
}

BOOL update_check(HWND hwnd_parent) {
    if (!github_updates_configured()) {
        MessageBoxW(hwnd_parent,
            L"GitHub updates are not configured.\n\n"
            L"Build with -DRESIZERC_GITHUB_OWNER=your-user -DRESIZERC_GITHUB_REPO=your-repo.",
            L"Update Check", MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }

    wchar_t response[65536];
    wchar_t latest_tag[64];

    if (!http_get(UPDATE_CHECK_URL, response, sizeof(response)/sizeof(wchar_t))) {
        MessageBoxW(hwnd_parent, L"Failed to check for updates.", L"Update Check", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (!json_get_string(response, L"\"tag_name\"", latest_tag, sizeof(latest_tag)/sizeof(wchar_t))) {
        MessageBoxW(hwnd_parent, L"Could not read the latest GitHub release.", L"Update Check", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    if (!json_get_string(response, L"\"html_url\"", g_latest_release_url, sizeof(g_latest_release_url)/sizeof(wchar_t))) {
        wcscpy_s(g_latest_release_url, sizeof(g_latest_release_url)/sizeof(wchar_t), UPDATE_RELEASE_URL);
    }

    if (compare_versions(APP_VERSION, latest_tag)) {
        wchar_t msg[512];
        swprintf(msg, 512, L"A new version (%s) is available.\n\nCurrent version: %s\n\nOpen the GitHub release?",
                 latest_tag, APP_VERSION);

        if (MessageBoxW(hwnd_parent, msg, L"Update Available", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            return update_download_and_install(hwnd_parent);
        }
    } else {
        MessageBoxW(hwnd_parent, L"You are using the latest version!", L"Update Check", MB_OK | MB_ICONINFORMATION);
    }

    return FALSE;
}

BOOL update_download_and_install(HWND hwnd_parent) {
    const wchar_t *url = g_latest_release_url[0] ? g_latest_release_url : UPDATE_RELEASE_URL;
    HINSTANCE result = ShellExecuteW(hwnd_parent, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        MessageBoxW(hwnd_parent, L"Failed to open the GitHub release.", L"Update", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    return TRUE;
}

static BOOL relaunch_with_update(HWND hwnd_parent, const wchar_t *download_path) {
    wchar_t exe_path[MAX_PATH_LEN];
    wchar_t script_path[MAX_PATH_LEN];
    wchar_t temp_dir[MAX_PATH_LEN];

    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH_LEN)) return FALSE;
    if (!GetTempPathW(MAX_PATH_LEN, temp_dir)) return FALSE;
    swprintf(script_path, sizeof(script_path)/sizeof(wchar_t), L"%sResizerC_update_%lu.cmd", temp_dir, GetCurrentProcessId());

    DWORD pid = GetCurrentProcessId();
    FILE *script = _wfopen(script_path, L"w");
    if (!script) return FALSE;
    fwprintf(script,
             L"@echo off\n"
             L"setlocal\n"
             L"set \"pid=%lu\"\n"
             L"set \"src=%s\"\n"
             L"set \"dst=%s\"\n"
             L":wait\n"
             L"tasklist /fi \"PID eq %%pid%%\" /fo csv /nh | findstr /i \"%%pid%%\" >nul\n"
             L"if not errorlevel 1 (\n"
             L"  timeout /t 1 /nobreak >nul\n"
             L"  goto wait\n"
             L")\n"
             L"move /y \"%%src%%\" \"%%dst%%\" >nul\n"
             L"if errorlevel 1 exit /b 1\n"
             L"start \"\" \"%%dst%%\"\n"
             L"del \"%%~f0\"\n",
             pid, download_path, exe_path);
    fclose(script);

    HINSTANCE result = ShellExecuteW(hwnd_parent, L"open", script_path, NULL, temp_dir, SW_HIDE);
    if ((INT_PTR)result <= 32) return FALSE;

    PostMessageW(hwnd_parent, WM_CLOSE, 0, 0);
    return TRUE;
}

BOOL update_check_automatic(HWND hwnd_parent) {
    if (!github_updates_configured()) return FALSE;

    wchar_t response[65536];
    wchar_t latest_tag[64];
    wchar_t download_url[2048];
    wchar_t exe_path[MAX_PATH_LEN];
    wchar_t update_path[MAX_PATH_LEN];

    if (!http_get(UPDATE_CHECK_URL, response, sizeof(response)/sizeof(wchar_t))) return FALSE;
    if (!json_get_string(response, L"\"tag_name\"", latest_tag, sizeof(latest_tag)/sizeof(wchar_t))) return FALSE;
    if (!compare_versions(APP_VERSION, latest_tag)) return FALSE;
    if (!json_get_exe_asset_url(response, download_url, sizeof(download_url)/sizeof(wchar_t))) return FALSE;

    wchar_t msg[512];
    swprintf(msg, 512,
             L"A new version (%s) is available.\n\nCurrent version: %s\n\nDownload, install, and relaunch now?",
             latest_tag, APP_VERSION);
    if (MessageBoxW(hwnd_parent, msg, L"Update Available", MB_YESNO | MB_ICONINFORMATION) != IDYES) {
        return FALSE;
    }

    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH_LEN)) return FALSE;
    wcscpy_s(update_path, sizeof(update_path)/sizeof(wchar_t), exe_path);
    wcscat_s(update_path, sizeof(update_path)/sizeof(wchar_t), L".update.exe");

    if (!http_download_file(download_url, update_path)) return FALSE;
    return relaunch_with_update(hwnd_parent, update_path);
}

void get_update_url(wchar_t *url, size_t url_size) {
    wcscpy_s(url, url_size, UPDATE_RELEASE_URL);
}
