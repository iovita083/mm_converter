// main.cpp
// Converts Project DIVA Mega Mix+ song mods to MicroMix+ format.

#include "converter.h"

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;

HWND g_hLog = NULL;
std::mutex g_LogMutex;

void GuiLog(const char* fmt, ...)
{
    if (!g_hLog) return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::string out(buf);
    size_t pos = 0;
    while ((pos = out.find("\n", pos)) != std::string::npos) {
        if (pos == 0 || out[pos - 1] != '\r') {
            out.replace(pos, 1, "\r\n");
            pos += 2;
        }
        else {
            pos++;
        }
    }

    std::lock_guard<std::mutex> lock(g_LogMutex);
    int len = GetWindowTextLengthA(g_hLog);
    SendMessageA(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_hLog, EM_REPLACESEL, 0, (LPARAM)out.c_str());
}

void to_lower(std::string& s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
}

// ---- Native Win32 GUI Implementation ----------------------------------------

HWND g_hMain, g_hEditMod, g_hEditOut, g_hSearch, g_hList, g_hRunBtn, g_hCancelBtn;
HWND g_hChkSkipVideo;

// Try every drive letter for "X:\SteamLibrary\steamapps\common\Hatsune Miku Project DIVA Mega Mix Plus\mods"
static std::string AutoDetectModsFolder() {
    const char* rel = "SteamLibrary\\steamapps\\common\\Hatsune Miku Project DIVA Mega Mix Plus\\mods";
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::string found;
    for (char d = 'A'; d <= 'Z'; d++) {
        std::string path = std::string(1, d) + ":\\" + rel;
        std::error_code ec;
        if (fs::is_directory(path, ec)) { found = path; break; }
    }
    SetErrorMode(oldMode);
    return found;
}

struct LoadedSong {
    int pv_id;
    std::string name;
    std::string mod_root;
    bool selected = true;

    // Direct UTF-8 / MultiByte strings matching project config
    std::string search_label_lower;
    std::string display_label;
};

std::vector<LoadedSong> g_Songs;
std::vector<size_t> g_FilteredIndices; // Maps Virtual ListView rows to g_Songs indices
bool g_UpdatingList = false;
std::atomic<bool> g_IsRunning = false;
std::atomic<bool> g_CancelRequested = false;

static void CacheSongData(LoadedSong& song) {
    char label_utf8[512];
    snprintf(label_utf8, sizeof(label_utf8), "[pv_%04d]  %s  (%s)",
        song.pv_id, song.name.c_str(), fs::path(song.mod_root).filename().string().c_str());

    song.display_label = label_utf8;
    song.search_label_lower = label_utf8;
    to_lower(song.search_label_lower);
}

static std::string BrowseFolder(HWND owner, const char* title) {
    IFileOpenDialog* pfd;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        pfd->GetOptions(&dwOptions);
        pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);

        int titleLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
        std::wstring wTitle(titleLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, title, -1, &wTitle[0], titleLen);
        pfd->SetTitle(wTitle.c_str());

        if (SUCCEEDED(pfd->Show(owner))) {
            IShellItem* psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    int size = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, NULL, 0, NULL, NULL);
                    std::string result(size - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, &result[0], size, NULL, NULL);
                    CoTaskMemFree(pszPath);
                    psi->Release();
                    pfd->Release();
                    return result;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return "";
}

static std::string find_pv_db(const std::string& mod_root) {
    fs::path root(mod_root);
    for (const auto& rel : { "rom/mod_pv_db.txt", "mod_pv_db.txt", "rom/pv_db.txt" }) {
        fs::path p = root / rel;
        if (fs::exists(p) && !fs::is_directory(p)) return p.string();
    }
    return "";
}

static std::vector<std::string> collect_mod_roots(const std::string& path) {
    if (!find_pv_db(path).empty()) return { path };
    std::vector<std::string> roots;
    try {
        for (auto& entry : fs::directory_iterator(path)) {
            if (entry.is_directory() && !find_pv_db(entry.path().string()).empty()) {
                roots.push_back(entry.path().string());
            }
        }
    }
    catch (...) {}
    return roots;
}

static void ApplyFilter() {
    g_UpdatingList = true;

    // Freeze repaints
    SendMessage(g_hList, WM_SETREDRAW, FALSE, 0);

    char query[256] = {};
    GetWindowTextA(g_hSearch, query, 256);

    std::string lower_filter(query);
    to_lower(lower_filter);

    g_FilteredIndices.clear();
    g_FilteredIndices.reserve(g_Songs.size());

    for (size_t i = 0; i < g_Songs.size(); i++) {
        if (lower_filter.empty() || g_Songs[i].search_label_lower.find(lower_filter) != std::string::npos) {
            g_FilteredIndices.push_back(i);
        }
    }

    // Allocate virtual count instantaneously
    ListView_SetItemCountEx(g_hList, g_FilteredIndices.size(), LVSICF_NOINVALIDATEALL);

    // Unfreeze and repaint once
    SendMessage(g_hList, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_hList, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);

    g_UpdatingList = false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HBRUSH hBrush = CreateSolidBrush(RGB(240, 240, 240));
        SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)hBrush);

        HFONT hFont = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

        HFONT hLogFont = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Segoe UI");

        auto createCtrl = [&](const char* cls, const char* txt, DWORD style, int x, int y, int width, int height, int id) -> HWND {
            HWND handle = CreateWindowExA(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, width, height, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
            SendMessageA(handle, WM_SETFONT, (WPARAM)(id == 100 ? hLogFont : hFont), TRUE);
            return handle;
            };

        createCtrl("STATIC", "Mods Folder:", 0, 10, 15, 90, 20, 0);
        g_hEditMod = createCtrl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, 10, 400, 25, 1);
        createCtrl("BUTTON", "Browse...", 0, 510, 10, 80, 25, 2);
        createCtrl("STATIC", "Output Folder:", 0, 10, 45, 90, 20, 0);
        g_hEditOut = createCtrl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, 40, 400, 25, 3);
        createCtrl("BUTTON", "Browse...", 0, 510, 40, 80, 25, 4);

        createCtrl("STATIC", "Search:", 0, 10, 77, 50, 20, 0);
        g_hSearch = createCtrl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 60, 75, 200, 25, 6);
        createCtrl("BUTTON", "Select All", 0, 270, 75, 80, 25, 7);
        createCtrl("BUTTON", "Deselect All", 0, 360, 75, 80, 25, 8);

        g_hChkSkipVideo = createCtrl("BUTTON", "Skip movies", BS_AUTOCHECKBOX, 450, 77, 150, 20, 5);

        // Virtual mode ANSI ListView creation
        g_hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
            10, 130, 580, 200, hwnd, (HMENU)9, NULL, NULL);

        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessage(g_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        LVCOLUMNA col = { LVCF_WIDTH, 0, 550 };
        ListView_InsertColumn(g_hList, 0, &col);

        g_hRunBtn = createCtrl("BUTTON", "Convert Selected", 0, 10, 340, 150, 30, 10);
        g_hCancelBtn = createCtrl("BUTTON", "Cancel", 0, 170, 340, 100, 30, 11);
        EnableWindow(g_hCancelBtn, FALSE);

        g_hLog = createCtrl("EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 10, 380, 580, 130, 100);
        SendMessageA(g_hLog, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFFE, 0);
        return 0;
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == 6) ApplyFilter();

        switch (LOWORD(wParam)) {
        case 2: {
            std::string path = BrowseFolder(hwnd, "Select Parent Mods Folder");
            if (!path.empty()) {
                SetWindowTextA(g_hEditMod, path.c_str());
                g_Songs.clear();
                auto roots = collect_mod_roots(path);
                if (roots.empty()) MessageBoxA(hwnd, "No mods found.", "Warning", MB_ICONWARNING);
                for (auto& root : roots) {
                    auto pvs = parse_pv_db(find_pv_db(root));
                    for (auto& [id, pv] : pvs) {
                        std::string name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
                        if (name.empty()) name = "pv_" + std::to_string(id);

                        LoadedSong song{ id, name, root, true };
                        CacheSongData(song);
                        g_Songs.push_back(song);
                    }
                }
                GuiLog("Loaded %zu songs from %zu mod(s).\r\n", g_Songs.size(), roots.size());
                ApplyFilter();
            }
            break;
        }
        case 4: {
            std::string path = BrowseFolder(hwnd, "Select Output Folder");
            if (!path.empty()) SetWindowTextA(g_hEditOut, path.c_str());
            break;
        }
        case 7:
        case 8: {
            bool state = (LOWORD(wParam) == 7);
            for (size_t idx : g_FilteredIndices) {
                g_Songs[idx].selected = state;
            }
            ListView_RedrawItems(g_hList, 0, g_FilteredIndices.size() - 1);
            break;
        }
        case 10: {
            if (g_IsRunning) break;

            char outPath[512];
            GetWindowTextA(g_hEditOut, outPath, 512);
            if (strlen(outPath) == 0) {
                MessageBoxA(hwnd, "Specify output folder first.", "Error", MB_ICONERROR);
                break;
            }

            bool skipVideo = SendMessage(g_hChkSkipVideo, BM_GETCHECK, 0, 0) == BST_CHECKED;

            std::vector<LoadedSong> jobs;
            for (auto& s : g_Songs) if (s.selected) jobs.push_back(s);
            if (jobs.empty()) {
                MessageBoxA(hwnd, "Select at least one song.", "Info", MB_ICONINFORMATION);
                break;
            }

            g_IsRunning = true;
            g_CancelRequested = false;
            EnableWindow(g_hRunBtn, FALSE);
            EnableWindow(g_hCancelBtn, TRUE);
            std::string outStr = outPath;

            std::thread([jobs, outStr, skipVideo]() {
                ConvOptions opts;
                opts.skip_video = skipVideo;

                int done = 0;
                for (const auto& job : jobs) {
                    if (g_CancelRequested) {
                        GuiLog("\r\nCancelled.\r\n");
                        break;
                    }
                    GuiLog("\r\n--- Processing [pv_%04d] %s ---\r\n", job.pv_id, job.name.c_str());
                    auto pvs = parse_pv_db(find_pv_db(job.mod_root));
                    if (pvs.count(job.pv_id)) {
                        convert_song(job.mod_root, pvs[job.pv_id], outStr, opts);
                    }
                    done++;
                    GuiLog("Progress: %d / %zu songs processed.\r\n", done, jobs.size());
                }
                if (!g_CancelRequested)
                    GuiLog("\r\nAll selected tasks finished.\r\n");
                g_IsRunning = false;
                g_CancelRequested = false;
                EnableWindow(g_hRunBtn, TRUE);
                EnableWindow(g_hCancelBtn, FALSE);
                }).detach();
            break;
        }
        case 11: {
            if (!g_IsRunning) break;
            g_CancelRequested = true;
            EnableWindow(g_hCancelBtn, FALSE);
            break;
        }
        }
        break;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        MoveWindow(g_hEditMod, 100, 10, w - 200, 25, TRUE);
        MoveWindow(GetDlgItem(hwnd, 2), w - 90, 10, 80, 25, TRUE);
        MoveWindow(g_hEditOut, 100, 40, w - 200, 25, TRUE);
        MoveWindow(GetDlgItem(hwnd, 4), w - 90, 40, 80, 25, TRUE);
        MoveWindow(g_hChkSkipVideo, 450, 77, 150, 20, TRUE);
        MoveWindow(g_hList, 10, 110, w - 20, h - 260, TRUE);
        ListView_SetColumnWidth(g_hList, 0, w - 40);
        MoveWindow(g_hRunBtn, 10, h - 145, 150, 30, TRUE);
        MoveWindow(g_hCancelBtn, 170, h - 145, 100, 30, TRUE);
        MoveWindow(g_hLog, 10, h - 105, w - 20, 95, TRUE);
        break;
    }
    case WM_NOTIFY: {
        LPNMHDR pnmhdr = (LPNMHDR)lParam;

        // Matches ANSI CreateWindowExA / MultiByte character setting
        if (pnmhdr->hwndFrom == g_hList && pnmhdr->code == LVN_GETDISPINFOA) {
            NMLVDISPINFOA* pdi = (NMLVDISPINFOA*)lParam;
            if (pdi->item.iItem >= 0 && pdi->item.iItem < (int)g_FilteredIndices.size()) {
                size_t songIndex = g_FilteredIndices[pdi->item.iItem];
                auto& song = g_Songs[songIndex];

                if (pdi->item.mask & LVIF_TEXT) {
                    pdi->item.pszText = const_cast<char*>(song.display_label.c_str());
                }
                if (pdi->item.mask & LVIF_STATE) {
                    pdi->item.state = song.selected ? INDEXTOSTATEIMAGEMASK(2) : INDEXTOSTATEIMAGEMASK(1);
                    pdi->item.stateMask = LVIS_STATEIMAGEMASK;
                }
            }
            return 0;
        }

        // Mouse click detection for virtual checkboxes using standard Windows macros
        if (pnmhdr->hwndFrom == g_hList && pnmhdr->code == NM_CLICK) {
            LVHITTESTINFO ht = { 0 };
            DWORD pos = GetMessagePos();
            ht.pt.x = (int)(short)LOWORD(pos);
            ht.pt.y = (int)(short)HIWORD(pos);
            MapWindowPoints(HWND_DESKTOP, g_hList, &ht.pt, 1);

            if (ListView_HitTest(g_hList, &ht) != -1) {
                if ((ht.flags & LVHT_ONITEMSTATEICON) && ht.iItem < (int)g_FilteredIndices.size()) {
                    size_t songIndex = g_FilteredIndices[ht.iItem];
                    g_Songs[songIndex].selected = !g_Songs[songIndex].selected;
                    ListView_RedrawItems(g_hList, ht.iItem, ht.iItem);
                }
            }
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow
) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "Failed to initialize COM library.", "Error", MB_ICONERROR);
        return -1;
    }

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    const char* clsName = "MmConverterGuiClass";
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = clsName;
    RegisterClassExA(&wc);

    g_hMain = CreateWindowExA(0, clsName, "mm_converter", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 560, NULL, NULL, hInstance, NULL);

    {
        std::string detected = AutoDetectModsFolder();
        if (!detected.empty()) {
            SetWindowTextA(g_hEditMod, detected.c_str());
            g_Songs.clear();
            auto roots = collect_mod_roots(detected);
            if (roots.empty()) {
                MessageBoxA(g_hMain, "No mods found.", "Warning", MB_ICONWARNING);
            }
            else {
                for (auto& root : roots) {
                    auto pvs = parse_pv_db(find_pv_db(root));
                    for (auto& [id, pv] : pvs) {
                        std::string name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
                        if (name.empty()) name = "pv_" + std::to_string(id);

                        LoadedSong song{ id, name, root, true };
                        CacheSongData(song);
                        g_Songs.push_back(song);
                    }
                }
                GuiLog("Loaded %zu songs from %zu mod(s).\r\n", g_Songs.size(), roots.size());
                ApplyFilter();
            }
        }
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}