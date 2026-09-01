#include "config.h"
#include "util.h"
#include "resource.h"
#include "../third_party/winamp/wa_ipc_min.h"

#include <shlwapi.h>
#include <commdlg.h>
#include <vector>

Config g_config;

extern HWND      g_hwndWinamp;
extern HINSTANCE g_hInstance;
void RestartWorker();   // defined in plugin.cpp

namespace {

const wchar_t* kSection = L"discord_rpc";

std::wstring ReadString(const std::wstring& ini, const wchar_t* key, const std::wstring& def) {
    wchar_t buf[1024] = {0};
    GetPrivateProfileStringW(kSection, key, def.c_str(), buf, _countof(buf), ini.c_str());
    return buf;
}

int ReadInt(const std::wstring& ini, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(kSection, key, def, ini.c_str());
}

void WriteString(const std::wstring& ini, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(kSection, key, value.c_str(), ini.c_str());
}

void WriteInt(const std::wstring& ini, const wchar_t* key, int value) {
    wchar_t buf[32];
    swprintf_s(buf, _countof(buf), L"%d", value);
    WritePrivateProfileStringW(kSection, key, buf, ini.c_str());
}

// Winamp's settings directory, which is per-user under %APPDATA% on a normal
// install. Falls back to the DLL's own folder if the IPC is unavailable.
std::wstring GetPluginDir(HWND hwndWinamp) {
    std::wstring dir;
    if (IsWindow(hwndWinamp)) {
        DWORD_PTR raw = 0;
        if (SendMessageTimeoutW(hwndWinamp, WM_WA_IPC, 0, IPC_GETINIDIRECTORYW,
                                SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &raw) && raw) {
            const wchar_t* p = (const wchar_t*)raw;
            if (p && p[0]) dir = p;
        }
    }
    if (dir.empty()) {
        wchar_t module[MAX_PATH] = {0};
        GetModuleFileNameW(g_hInstance, module, _countof(module));
        PathRemoveFileSpecW(module);
        dir = module;
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"Plugins";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\";
}

std::wstring TrimCopy(const std::wstring& s) { return util::Trim(s); }

std::wstring GetDlgText(HWND dlg, int id) {
    const int len = GetWindowTextLengthW(GetDlgItem(dlg, id));
    if (len <= 0) return std::wstring();
    std::vector<wchar_t> buf((size_t)len + 1, L'\0');
    GetDlgItemTextW(dlg, id, buf.data(), len + 1);
    return TrimCopy(buf.data());
}

INT_PTR CALLBACK ConfigDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetDlgItemTextW(dlg, IDC_CLIENT_ID,   g_config.clientId.c_str());
            SetDlgItemTextW(dlg, IDC_ACTIVITY_NAME, g_config.activityName.c_str());
            SetDlgItemTextW(dlg, IDC_FALLBACK_IMAGE, g_config.fallbackImage.c_str());

            CheckDlgButton(dlg, IDC_ENABLED,        g_config.enabled         ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_ALBUM_ART,      g_config.showAlbumArt    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_EMBEDDED_ART,   g_config.useEmbeddedArt  ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_ONLINE_LOOKUP,  g_config.useOnlineLookup ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_PROGRESS_BAR,   g_config.showProgressBar ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_SHOW_PAUSED,    g_config.showWhenPaused  ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dlg, IDC_LOGGING,        g_config.logging         ? BST_CHECKED : BST_UNCHECKED);

            HWND host = GetDlgItem(dlg, IDC_UPLOAD_HOST);
            SendMessageW(host, CB_ADDSTRING, 0, (LPARAM)L"litterbox.catbox.moe - expires after 72 hours");
            SendMessageW(host, CB_ADDSTRING, 0, (LPARAM)L"catbox.moe - permanent");
            SendMessageW(host, CB_SETCURSEL,
                         (g_config.uploadHost == L"catbox") ? 1 : 0, 0);

            HWND type = GetDlgItem(dlg, IDC_ACTIVITY_TYPE);
            SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)L"Listening to (recommended)");
            SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)L"Playing");
            SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)L"Watching");
            SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)L"Competing in");
            int typeIndex = 0;
            switch (g_config.activityType) {
                case 0: typeIndex = 1; break;
                case 3: typeIndex = 2; break;
                case 5: typeIndex = 3; break;
                default: typeIndex = 0; break;
            }
            SendMessageW(type, CB_SETCURSEL, typeIndex, 0);

            HWND display = GetDlgItem(dlg, IDC_STATUS_DISPLAY);
            SendMessageW(display, CB_ADDSTRING, 0, (LPARAM)L"Application name");
            SendMessageW(display, CB_ADDSTRING, 0, (LPARAM)L"Artist");
            SendMessageW(display, CB_ADDSTRING, 0, (LPARAM)L"Track title (recommended)");
            int displayIndex = 2;
            if (g_config.statusDisplayType == 0) displayIndex = 0;
            else if (g_config.statusDisplayType == 1) displayIndex = 1;
            SendMessageW(display, CB_SETCURSEL, displayIndex, 0);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    g_config.clientId      = GetDlgText(dlg, IDC_CLIENT_ID);
                    g_config.activityName  = GetDlgText(dlg, IDC_ACTIVITY_NAME);
                    g_config.fallbackImage = GetDlgText(dlg, IDC_FALLBACK_IMAGE);
                    if (g_config.activityName.empty()) g_config.activityName = L"Winamp";

                    g_config.enabled         = IsDlgButtonChecked(dlg, IDC_ENABLED)       == BST_CHECKED;
                    g_config.showAlbumArt    = IsDlgButtonChecked(dlg, IDC_ALBUM_ART)     == BST_CHECKED;
                    g_config.useEmbeddedArt  = IsDlgButtonChecked(dlg, IDC_EMBEDDED_ART)  == BST_CHECKED;
                    g_config.useOnlineLookup = IsDlgButtonChecked(dlg, IDC_ONLINE_LOOKUP) == BST_CHECKED;
                    g_config.showProgressBar = IsDlgButtonChecked(dlg, IDC_PROGRESS_BAR)  == BST_CHECKED;
                    g_config.showWhenPaused  = IsDlgButtonChecked(dlg, IDC_SHOW_PAUSED)   == BST_CHECKED;
                    g_config.logging         = IsDlgButtonChecked(dlg, IDC_LOGGING)       == BST_CHECKED;

                    g_config.uploadHost =
                        (SendDlgItemMessageW(dlg, IDC_UPLOAD_HOST, CB_GETCURSEL, 0, 0) == 1)
                            ? L"catbox" : L"litterbox";

                    switch ((int)SendDlgItemMessageW(dlg, IDC_ACTIVITY_TYPE, CB_GETCURSEL, 0, 0)) {
                        case 1:  g_config.activityType = 0; break;
                        case 2:  g_config.activityType = 3; break;
                        case 3:  g_config.activityType = 5; break;
                        default: g_config.activityType = 2; break;
                    }
                    switch ((int)SendDlgItemMessageW(dlg, IDC_STATUS_DISPLAY, CB_GETCURSEL, 0, 0)) {
                        case 0:  g_config.statusDisplayType = 0; break;
                        case 1:  g_config.statusDisplayType = 1; break;
                        default: g_config.statusDisplayType = 2; break;
                    }

                    g_config.Save(GetSettingsPath(g_hwndWinamp));
                    util::SetLoggingEnabled(g_config.logging);
                    RestartWorker();   // pick up the new client ID / settings
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;

                case IDC_OPEN_PORTAL:
                    ShellExecuteW(dlg, L"open", L"https://discord.com/developers/applications",
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;

                case IDC_BROWSE_FALLBACK: {
                    wchar_t path[MAX_PATH] = {0};
                    const std::wstring current = GetDlgText(dlg, IDC_FALLBACK_IMAGE);
                    if (!current.empty() && current.size() < MAX_PATH &&
                        GetFileAttributesW(current.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        wcscpy_s(path, _countof(path), current.c_str());
                    }

                    OPENFILENAMEW ofn;
                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize  = sizeof(ofn);
                    ofn.hwndOwner    = dlg;
                    ofn.lpstrFilter  = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
                    ofn.lpstrFile    = path;
                    ofn.nMaxFile     = _countof(path);
                    ofn.lpstrTitle   = L"Choose the image to show when a track has no cover";
                    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(dlg, IDC_FALLBACK_IMAGE, path);
                    }
                    return TRUE;
                }
            }
            break;

        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

} // namespace

void Config::Load(const std::wstring& iniPath) {
    enabled           = ReadInt(iniPath, L"enabled", 1) != 0;
    clientId          = TrimCopy(ReadString(iniPath, L"client_id", L""));
    activityName      = ReadString(iniPath, L"activity_name", L"Winamp");
    activityType      = ReadInt(iniPath, L"activity_type", 2);
    showAlbumArt      = ReadInt(iniPath, L"show_album_art", 1) != 0;
    useEmbeddedArt    = ReadInt(iniPath, L"use_embedded_art", 1) != 0;
    useOnlineLookup   = ReadInt(iniPath, L"use_online_lookup", 1) != 0;
    uploadHost        = ReadString(iniPath, L"upload_host", L"litterbox");
    uploadExpiryHours = ReadInt(iniPath, L"upload_expiry_hours", 72);
    artMaxEdge        = ReadInt(iniPath, L"art_max_edge", 512);
    showProgressBar   = ReadInt(iniPath, L"show_progress_bar", 1) != 0;
    showWhenPaused    = ReadInt(iniPath, L"show_when_paused", 1) != 0;
    statusDisplayType = ReadInt(iniPath, L"status_display_type", 2);
    fallbackImage     = ReadString(iniPath, L"fallback_image", L"winamp");
    smallImagePlaying = ReadString(iniPath, L"small_image_playing", L"play");
    smallImagePaused  = ReadString(iniPath, L"small_image_paused", L"pause");
    logging           = ReadInt(iniPath, L"logging", 0) != 0;

    if (activityType != 0 && activityType != 2 && activityType != 3 && activityType != 5) {
        activityType = 2;
    }
    if (statusDisplayType < -1 || statusDisplayType > 2) statusDisplayType = 2;
    if (activityName.empty()) activityName = L"Winamp";

    // The upload API only accepts these four lifetimes.
    if (uploadExpiryHours != 1 && uploadExpiryHours != 12 &&
        uploadExpiryHours != 24 && uploadExpiryHours != 72) {
        uploadExpiryHours = 72;
    }
    if (artMaxEdge < 64)   artMaxEdge = 64;
    if (artMaxEdge > 1024) artMaxEdge = 1024;
    if (uploadHost != L"catbox") uploadHost = L"litterbox";
}

void Config::Save(const std::wstring& iniPath) const {
    WriteInt(iniPath,    L"enabled",             enabled ? 1 : 0);
    WriteString(iniPath, L"client_id",           clientId);
    WriteString(iniPath, L"activity_name",       activityName);
    WriteInt(iniPath,    L"activity_type",       activityType);
    WriteInt(iniPath,    L"show_album_art",      showAlbumArt ? 1 : 0);
    WriteInt(iniPath,    L"use_embedded_art",    useEmbeddedArt ? 1 : 0);
    WriteInt(iniPath,    L"use_online_lookup",   useOnlineLookup ? 1 : 0);
    WriteString(iniPath, L"upload_host",         uploadHost);
    WriteInt(iniPath,    L"upload_expiry_hours", uploadExpiryHours);
    WriteInt(iniPath,    L"art_max_edge",        artMaxEdge);
    WriteInt(iniPath,    L"show_progress_bar",   showProgressBar ? 1 : 0);
    WriteInt(iniPath,    L"show_when_paused",    showWhenPaused ? 1 : 0);
    WriteInt(iniPath,    L"status_display_type", statusDisplayType);
    WriteString(iniPath, L"fallback_image",      fallbackImage);
    WriteString(iniPath, L"small_image_playing", smallImagePlaying);
    WriteString(iniPath, L"small_image_paused",  smallImagePaused);
    WriteInt(iniPath,    L"logging",             logging ? 1 : 0);
}

std::wstring GetSettingsPath(HWND hwndWinamp) {
    return GetPluginDir(hwndWinamp) + L"gen_discord_rpc.ini";
}

std::wstring GetCachePath(HWND hwndWinamp) {
    return GetPluginDir(hwndWinamp) + L"gen_discord_rpc_artcache.txt";
}

std::wstring GetLogPath(HWND hwndWinamp) {
    return GetPluginDir(hwndWinamp) + L"gen_discord_rpc.log";
}

void ShowConfigDialog(HWND parent, HINSTANCE instance) {
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, ConfigDlgProc, 0);
}
