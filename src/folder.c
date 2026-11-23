#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <wchar.h>
#include "folder.h"

static WCHAR ini[MAX_PATH], base[MAX_PATH], selected[MAX_PATH];
static LONG picking, serial;
static BOOL ready;
static SRWLOCK folder_lock = SRWLOCK_INIT;
static BOOL write_bytes(HANDLE file, const void *data, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, data, size, &written, NULL) && written == size;
}

// Profile APIs only preserve Unicode when the file starts with a UTF-16LE BOM.
// New files start in this format. Convert older ANSI profiles without dropping
// existing sections and comments, publishing the replacement after the write.
static void prepare_profile(void) {
    static const WORD bom = 0xfeff;
    HANDLE output =
        CreateFileW(ini, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output != INVALID_HANDLE_VALUE) {
        BOOL ok = write_bytes(output, &bom, sizeof bom);
        CloseHandle(output);
        if (!ok)
            DeleteFileW(ini);
        return;
    }
    if (GetLastError() != ERROR_FILE_EXISTS)
        return;
    HANDLE input = CreateFileW(ini, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE)
        return;
    BYTE header[3];
    DWORD head = 0;
    LARGE_INTEGER size;
    if (!ReadFile(input, header, sizeof header, &head, NULL) ||
        (head >= 2 &&
         ((header[0] == 0xff && header[1] == 0xfe) || (header[0] == 0xfe && header[1] == 0xff))) ||
        !GetFileSizeEx(input, &size) || size.QuadPart > 0x1fffffff) {
        CloseHandle(input);
        return;
    }
    BOOL utf8 = head == 3 && header[0] == 0xef && header[1] == 0xbb && header[2] == 0xbf;
    DWORD length = (DWORD)size.QuadPart - (utf8 ? 3 : 0), read = 0;
    char *bytes = HeapAlloc(GetProcessHeap(), 0, length + 1);
    WCHAR *wide = NULL, temp[MAX_PATH];
    BOOL temporary = FALSE;
    if (!bytes || SetFilePointer(input, utf8 ? 3 : 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
        !ReadFile(input, bytes, length, &read, NULL) || read != length)
        goto done;
    UINT codepage = utf8 ? CP_UTF8 : CP_ACP;
    int units = length ? MultiByteToWideChar(codepage, 0, bytes, (int)length, NULL, 0) : 0;
    if (length && !units)
        goto done;
    wide = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(units + 1) * sizeof *wide);
    if (!wide || (units && MultiByteToWideChar(codepage, 0, bytes, (int)length, wide, units) != units))
        goto done;
    if (!GetTempFileNameW(base, L"wpc", 0, temp))
        goto done;
    temporary = TRUE;
    output = CreateFileW(temp, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output != INVALID_HANDLE_VALUE) {
        BOOL ok =
            write_bytes(output, &bom, sizeof bom) && write_bytes(output, wide, (DWORD)units * sizeof *wide);
        CloseHandle(output);
        CloseHandle(input);
        input = INVALID_HANDLE_VALUE;
        if (ok && MoveFileExW(temp, ini, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            temporary = FALSE;
    }
done:
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    if (temporary)
        DeleteFileW(temp);
    if (wide)
        HeapFree(GetProcessHeap(), 0, wide);
    if (bytes)
        HeapFree(GetProcessHeap(), 0, bytes);
}

static BOOL append(WCHAR *out, const WCHAR *root, const WCHAR *tail) {
    size_t len = wcslen(root), extra = wcslen(tail);
    if (len + extra >= MAX_PATH)
        return FALSE;
    memcpy(out, root, len * sizeof(WCHAR));
    memcpy(out + len, tail, (extra + 1) * sizeof(WCHAR));
    return TRUE;
}
const WCHAR *folder_ini(void) {
    if (ready)
        return ini;
    WCHAR appdata[MAX_PATH], def[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT,
                                appdata)) ||
        !append(base, appdata, L"\\WITNPhotoCamera") || !append(ini, base, L"\\settings.ini") ||
        !append(def, base, L"\\Photos")) {
        ini[0] = 0;
        return ini;
    }
    CreateDirectoryW(base, NULL);
    prepare_profile();
    GetPrivateProfileStringW(L"Output", L"Directory", def, selected, MAX_PATH, ini);
    ready = TRUE;
    return ini;
}
BOOL folder_busy(void) {
    return InterlockedCompareExchange(&picking, 0, 0) != 0;
}
static int CALLBACK set_start(HWND dlg, UINT msg, LPARAM arg, LPARAM path) {
    (void)arg;
    if (msg == BFFM_INITIALIZED)
        SendMessageW(dlg, BFFM_SETSELECTIONW, TRUE, path);
    return 0;
}
static DWORD WINAPI choose_thread(void *owner) {
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(com)) {
        WCHAR start[MAX_PATH], path[MAX_PATH], display[MAX_PATH];
        AcquireSRWLockShared(&folder_lock);
        lstrcpyW(start, selected);
        ReleaseSRWLockShared(&folder_lock);
        BROWSEINFOW info = {.hwndOwner = (HWND)owner,
                            .pszDisplayName = display,
                            .lpszTitle = L"Choose where War in the North saves photographs",
                            .ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX,
                            .lpfn = set_start,
                            .lParam = (LPARAM)start};
        LPITEMIDLIST item = SHBrowseForFolderW(&info);
        if (item && SHGetPathFromIDListW(item, path)) {
            AcquireSRWLockExclusive(&folder_lock);
            lstrcpyW(selected, path);
            WritePrivateProfileStringW(L"Output", L"Directory", path, ini);
            ReleaseSRWLockExclusive(&folder_lock);
        }
        if (item)
            CoTaskMemFree(item);
        CoUninitialize();
    }
    InterlockedExchange(&picking, 0);
    return 0;
}
BOOL folder_choose(HWND owner) {
    if (!ready || InterlockedCompareExchange(&picking, 1, 0))
        return FALSE;
    HANDLE thread = CreateThread(NULL, 0, choose_thread, owner, 0, NULL);
    if (!thread) {
        InterlockedExchange(&picking, 0);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}
BOOL folder_photo_path(WCHAR path[MAX_PATH]) {
    if (!ready)
        return FALSE;
    WCHAR dir[MAX_PATH], suffix[80];
    AcquireSRWLockShared(&folder_lock);
    lstrcpyW(dir, selected);
    ReleaseSRWLockShared(&folder_lock);
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfW(suffix, L"\\WITN-%04u%02u%02u-%02u%02u%02u-%03u-%04u.png", t.wYear, t.wMonth, t.wDay, t.wHour,
              t.wMinute, t.wSecond, t.wMilliseconds, (unsigned)InterlockedIncrement(&serial) % 10000);
    return append(path, dir, suffix) && wcslen(path) + 5 < MAX_PATH;
}
