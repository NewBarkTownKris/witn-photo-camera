#ifndef WITN_FOLDER_H
#define WITN_FOLDER_H
#include <windows.h>
const WCHAR *folder_ini(void);
BOOL folder_choose(HWND owner);
BOOL folder_busy(void);
BOOL folder_photo_path(WCHAR path[MAX_PATH]);
#endif
