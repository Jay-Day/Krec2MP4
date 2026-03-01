#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>

#include "converter.h"
#include "emulator.h"
#include "ffmpeg_encoder.h"
#include "gui_resources.h"

// Enable visual styles (common controls v6) + DPI awareness via embedded manifest
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' " \
    "processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

// --- Globals ---
static HWND g_hwnd = nullptr;
static HFONT g_font = nullptr;

// Controls
static HWND g_rom_path = nullptr;
static HWND g_input_path = nullptr;
static HWND g_batch_check = nullptr;
static HWND g_output_path = nullptr;
static HWND g_resolution_combo = nullptr;
static HWND g_quality_combo = nullptr;
static HWND g_fps_edit = nullptr;
static HWND g_msaa_slider = nullptr;
static HWND g_msaa_value = nullptr;
static HWND g_aniso_slider = nullptr;
static HWND g_aniso_value = nullptr;
static HWND g_encoder_combo = nullptr;
static HWND g_verbose_check = nullptr;
static HWND g_convert_btn = nullptr;
static HWND g_cancel_btn = nullptr;
static HWND g_open_folder_btn = nullptr;
static HWND g_progress_bar = nullptr;
static HWND g_progress_text = nullptr;
static HWND g_log_edit = nullptr;

// Worker state
static std::thread g_worker_thread;
static std::atomic<bool> g_cancel_flag{false};
static bool g_converting = false;
static LARGE_INTEGER g_start_time = {};      // batch start (for total elapsed)
static LARGE_INTEGER g_file_start_time = {};  // per-file start (for fps/ETA)
static LARGE_INTEGER g_perf_freq = {};

// Resolution presets
struct ResPreset {
    const wchar_t* label;
    int w, h;
};
static const ResPreset g_res_presets[] = {
    { L"320x240",   320,  240 },
    { L"640x480",   640,  480 },
    { L"960x720",   960,  720 },
    { L"1280x960", 1280,  960 },
    { L"1920x1440",1920, 1440 },
};
static const int g_num_res_presets = sizeof(g_res_presets) / sizeof(g_res_presets[0]);

// MSAA / Aniso presets: slider position -> value
static const int g_msaa_values[] = { 0, 2, 4, 8 };
static const wchar_t* g_msaa_labels[] = { L"Off", L"2x", L"4x", L"8x" };
static const int g_aniso_values[] = { 0, 2, 4, 8, 16 };
static const wchar_t* g_aniso_labels[] = { L"Off", L"2x", L"4x", L"8x", L"16x" };

// Available encoders (probed at startup)
static std::vector<EncoderInfo> g_encoders;

// --- Helpers ---

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring out(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &out[0], len);
    return out;
}

static std::string GetEditText(HWND edit) {
    int len = GetWindowTextLengthW(edit);
    if (len <= 0) return {};
    std::wstring buf(len + 1, 0);
    GetWindowTextW(edit, &buf[0], len + 1);
    buf.resize(len);
    return WideToUtf8(buf);
}

static void SetEditText(HWND edit, const std::string& text) {
    SetWindowTextW(edit, Utf8ToWide(text).c_str());
}

static void AppendLog(const wchar_t* text) {
    int len = GetWindowTextLengthW(g_log_edit);
    SendMessageW(g_log_edit, EM_SETSEL, len, len);
    SendMessageW(g_log_edit, EM_REPLACESEL, FALSE, (LPARAM)text);
}

// Get the parent directory of a path currently in an edit control (for initial browse dir)
static std::wstring GetEditDir(HWND edit) {
    std::string text = GetEditText(edit);
    if (text.empty()) return {};
    fs::path p(text);
    fs::path dir = fs::is_directory(p) ? p : p.parent_path();
    if (fs::is_directory(dir)) return dir.wstring();
    return {};
}

static EncoderFamily GetSelectedEncoderFamily() {
    int sel = (int)SendMessageW(g_encoder_combo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)g_encoders.size())
        return g_encoders[sel].family;
    return EncoderFamily::X264_X265;
}

static void PopulateQualityCombo(EncoderFamily family) {
    SendMessageW(g_quality_combo, CB_RESETCONTENT, 0, 0);
    const QualityFamily& qf = get_quality_family(family);
    for (int i = 0; i < qf.num_presets; i++) {
        char label[64];
        snprintf(label, sizeof(label), "%s (%s %d)",
                 qf.presets[i].name, qf.param_name, qf.presets[i].value);
        SendMessageW(g_quality_combo, CB_ADDSTRING, 0,
                     (LPARAM)Utf8ToWide(label).c_str());
    }
    SendMessageW(g_quality_combo, CB_SETCURSEL, qf.default_index, 0);
}

static std::wstring BrowseFile(HWND owner, const wchar_t* title, const wchar_t* filter, bool save,
                               const wchar_t* defExt = nullptr,
                               const wchar_t* initialDir = nullptr) {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defExt;
    ofn.lpstrInitialDir = initialDir;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&ofn)) return buf;
    } else {
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) return buf;
    }
    return {};
}

static std::wstring BrowseFolder(HWND owner, const wchar_t* title,
                                 const wchar_t* initialDir = nullptr) {
    std::wstring result;
    IFileDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileDialog, (void**)&pfd);
    if (SUCCEEDED(hr)) {
        DWORD opts;
        pfd->GetOptions(&opts);
        pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->SetTitle(title);
        if (initialDir && initialDir[0]) {
            IShellItem* psi_dir = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initialDir, nullptr,
                          IID_IShellItem, (void**)&psi_dir))) {
                pfd->SetFolder(psi_dir);
                psi_dir->Release();
            }
        }
        hr = pfd->Show(owner);
        if (SUCCEEDED(hr)) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = path;
                    CoTaskMemFree(path);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return result;
}

// --- Settings persistence (INI file next to exe) ---

static std::wstring GetIniPath() {
    std::wstring path = Utf8ToWide(get_exe_dir());
    path += L"Krec2MP4.ini";
    return path;
}

static void SaveSettings() {
    std::wstring ini = GetIniPath();
    const wchar_t* file = ini.c_str();
    const wchar_t* sec = L"Settings";

    // Paths
    wchar_t buf[MAX_PATH] = {};
    GetWindowTextW(g_rom_path, buf, MAX_PATH);
    WritePrivateProfileStringW(sec, L"RomPath", buf, file);
    GetWindowTextW(g_input_path, buf, MAX_PATH);
    WritePrivateProfileStringW(sec, L"InputPath", buf, file);
    GetWindowTextW(g_output_path, buf, MAX_PATH);
    WritePrivateProfileStringW(sec, L"OutputPath", buf, file);

    // Checkboxes
    bool batch = (SendMessageW(g_batch_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
    WritePrivateProfileStringW(sec, L"Batch", batch ? L"1" : L"0", file);
    bool verbose = (SendMessageW(g_verbose_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
    WritePrivateProfileStringW(sec, L"Verbose", verbose ? L"1" : L"0", file);

    // Resolution (save index)
    int res_sel = (int)SendMessageW(g_resolution_combo, CB_GETCURSEL, 0, 0);
    wchar_t num[16];
    swprintf(num, 16, L"%d", res_sel);
    WritePrivateProfileStringW(sec, L"Resolution", num, file);

    // Encoder (save codec name so it survives encoder list changes)
    int enc_sel = (int)SendMessageW(g_encoder_combo, CB_GETCURSEL, 0, 0);
    if (enc_sel >= 0 && enc_sel < (int)g_encoders.size()) {
        WritePrivateProfileStringW(sec, L"Encoder",
            Utf8ToWide(g_encoders[enc_sel].codec).c_str(), file);
    }

    // Quality preset
    {
        int qsel = (int)SendMessageW(g_quality_combo, CB_GETCURSEL, 0, 0);
        const QualityFamily& qf = get_quality_family(GetSelectedEncoderFamily());
        if (qsel >= 0 && qsel < qf.num_presets) {
            WritePrivateProfileStringW(sec, L"Quality",
                Utf8ToWide(qf.presets[qsel].name).c_str(), file);
        }
    }

    // FPS
    GetWindowTextW(g_fps_edit, buf, MAX_PATH);
    WritePrivateProfileStringW(sec, L"FPS", buf, file);

    // MSAA
    int msaa = (int)SendMessageW(g_msaa_slider, TBM_GETPOS, 0, 0);
    swprintf(num, 16, L"%d", msaa);
    WritePrivateProfileStringW(sec, L"MSAA", num, file);

    // Aniso
    int aniso = (int)SendMessageW(g_aniso_slider, TBM_GETPOS, 0, 0);
    swprintf(num, 16, L"%d", aniso);
    WritePrivateProfileStringW(sec, L"Aniso", num, file);
}

static void LoadSettings() {
    std::wstring ini = GetIniPath();
    const wchar_t* file = ini.c_str();
    const wchar_t* sec = L"Settings";

    // Check if INI exists
    if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t buf[MAX_PATH] = {};

    // Paths
    GetPrivateProfileStringW(sec, L"RomPath", L"", buf, MAX_PATH, file);
    if (buf[0]) SetWindowTextW(g_rom_path, buf);
    GetPrivateProfileStringW(sec, L"InputPath", L"", buf, MAX_PATH, file);
    if (buf[0]) SetWindowTextW(g_input_path, buf);
    GetPrivateProfileStringW(sec, L"OutputPath", L"", buf, MAX_PATH, file);
    if (buf[0]) SetWindowTextW(g_output_path, buf);

    // Checkboxes
    int batch = GetPrivateProfileIntW(sec, L"Batch", 0, file);
    SendMessageW(g_batch_check, BM_SETCHECK, batch ? BST_CHECKED : BST_UNCHECKED, 0);
    int verbose = GetPrivateProfileIntW(sec, L"Verbose", 0, file);
    SendMessageW(g_verbose_check, BM_SETCHECK, verbose ? BST_CHECKED : BST_UNCHECKED, 0);

    // Resolution
    int res_sel = GetPrivateProfileIntW(sec, L"Resolution", 1, file);
    if (res_sel >= 0 && res_sel < g_num_res_presets)
        SendMessageW(g_resolution_combo, CB_SETCURSEL, res_sel, 0);

    // Encoder (match by codec name)
    GetPrivateProfileStringW(sec, L"Encoder", L"", buf, MAX_PATH, file);
    if (buf[0]) {
        std::string saved_codec = WideToUtf8(buf);
        for (size_t i = 0; i < g_encoders.size(); i++) {
            if (saved_codec == g_encoders[i].codec) {
                SendMessageW(g_encoder_combo, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }

    // Quality preset (repopulate combo for restored encoder, then match saved name)
    PopulateQualityCombo(GetSelectedEncoderFamily());
    GetPrivateProfileStringW(sec, L"Quality", L"Medium", buf, MAX_PATH, file);
    {
        std::string saved_name = WideToUtf8(buf);
        const QualityFamily& qf = get_quality_family(GetSelectedEncoderFamily());
        for (int i = 0; i < qf.num_presets; i++) {
            if (saved_name == qf.presets[i].name) {
                SendMessageW(g_quality_combo, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }

    // FPS
    GetPrivateProfileStringW(sec, L"FPS", L"0", buf, MAX_PATH, file);
    SetWindowTextW(g_fps_edit, buf);

    // MSAA
    int msaa = GetPrivateProfileIntW(sec, L"MSAA", 0, file);
    if (msaa >= 0 && msaa <= 3) {
        SendMessageW(g_msaa_slider, TBM_SETPOS, TRUE, msaa);
        SetWindowTextW(g_msaa_value, g_msaa_labels[msaa]);
    }

    // Aniso
    int aniso = GetPrivateProfileIntW(sec, L"Aniso", 0, file);
    if (aniso >= 0 && aniso <= 4) {
        SendMessageW(g_aniso_slider, TBM_SETPOS, TRUE, aniso);
        SetWindowTextW(g_aniso_value, g_aniso_labels[aniso]);
    }
}

// --- Create Controls ---

static HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        x, y, w, h, parent, nullptr, nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hwnd;
}

static HWND CreateEdit(HWND parent, int id, int x, int y, int w, int h, DWORD style = 0) {
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hwnd;
}

static HWND CreateBtn(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hwnd;
}

static HWND CreateCheck(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hwnd;
}

static void CreateControls(HWND hwnd) {
    const int LBL_W = 80;
    const int MARGIN = 12;
    const int ROW_H = 24;
    const int GAP = 6;
    const int EDIT_X = MARGIN + LBL_W + GAP;
    const int BTN_W = 70;
    const int CLIENT_W = 620;
    const int EDIT_W = CLIENT_W - EDIT_X - BTN_W - GAP - MARGIN;

    int y = MARGIN;

    // --- File paths ---
    CreateLabel(hwnd, L"ROM Path:", MARGIN, y + 2, LBL_W, ROW_H);
    g_rom_path = CreateEdit(hwnd, IDC_ROM_PATH, EDIT_X, y, EDIT_W, ROW_H);
    CreateBtn(hwnd, L"Browse...", IDC_ROM_BROWSE, EDIT_X + EDIT_W + GAP, y, BTN_W, ROW_H);
    y += ROW_H + GAP;

    CreateLabel(hwnd, L"Input:", MARGIN, y + 2, LBL_W, ROW_H);
    g_input_path = CreateEdit(hwnd, IDC_INPUT_PATH, EDIT_X, y, EDIT_W, ROW_H);
    CreateBtn(hwnd, L"Browse...", IDC_INPUT_BROWSE, EDIT_X + EDIT_W + GAP, y, BTN_W, ROW_H);
    y += ROW_H + GAP;

    g_batch_check = CreateCheck(hwnd, L"Batch mode (process all .krec in folder)",
                                IDC_BATCH_CHECK, EDIT_X, y, EDIT_W, ROW_H);
    y += ROW_H + GAP;

    CreateLabel(hwnd, L"Output:", MARGIN, y + 2, LBL_W, ROW_H);
    g_output_path = CreateEdit(hwnd, IDC_OUTPUT_PATH, EDIT_X, y, EDIT_W, ROW_H);
    CreateBtn(hwnd, L"Browse...", IDC_OUTPUT_BROWSE, EDIT_X + EDIT_W + GAP, y, BTN_W, ROW_H);
    y += ROW_H + GAP + 4;

    // --- Video Settings group ---
    {
        HWND sep = CreateWindowExW(0, L"STATIC", L"Video Settings",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            MARGIN, y, CLIENT_W - 2 * MARGIN, ROW_H, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(sep, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    y += ROW_H + 2;

    CreateLabel(hwnd, L"Resolution:", MARGIN, y + 2, LBL_W, ROW_H);
    g_resolution_combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        EDIT_X, y, 140, 200, hwnd, (HMENU)(INT_PTR)IDC_RESOLUTION, nullptr, nullptr);
    SendMessageW(g_resolution_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (int i = 0; i < g_num_res_presets; i++) {
        SendMessageW(g_resolution_combo, CB_ADDSTRING, 0, (LPARAM)g_res_presets[i].label);
    }
    SendMessageW(g_resolution_combo, CB_SETCURSEL, 1, 0); // 640x480
    y += ROW_H + GAP;

    CreateLabel(hwnd, L"Encoder:", MARGIN, y + 2, LBL_W, ROW_H);
    g_encoder_combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        EDIT_X, y, 200, 200, hwnd, (HMENU)(INT_PTR)IDC_ENCODER, nullptr, nullptr);
    SendMessageW(g_encoder_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (size_t i = 0; i < g_encoders.size(); i++) {
        SendMessageW(g_encoder_combo, CB_ADDSTRING, 0, (LPARAM)g_encoders[i].label);
    }
    SendMessageW(g_encoder_combo, CB_SETCURSEL, 0, 0); // H.264 (CPU)
    y += ROW_H + GAP;

    CreateLabel(hwnd, L"Quality:", MARGIN, y + 2, LBL_W, ROW_H);
    g_quality_combo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        EDIT_X, y, 200, 200, hwnd, (HMENU)(INT_PTR)IDC_QUALITY, nullptr, nullptr);
    SendMessageW(g_quality_combo, WM_SETFONT, (WPARAM)g_font, TRUE);
    PopulateQualityCombo(GetSelectedEncoderFamily());
    y += ROW_H + GAP + 4;

    CreateLabel(hwnd, L"FPS Override:", MARGIN, y + 2, LBL_W, ROW_H);
    g_fps_edit = CreateEdit(hwnd, IDC_FPS_EDIT, EDIT_X, y, 60, ROW_H);
    SetWindowTextW(g_fps_edit, L"0");
    {
        HWND hint = CreateWindowExW(0, L"STATIC", L"(0 = auto)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            EDIT_X + 66, y + 2, 80, ROW_H, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hint, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    y += ROW_H + GAP;

    // Anti-aliasing (MSAA) slider: positions 0-3 -> Off, 2x, 4x, 8x
    CreateLabel(hwnd, L"Anti-Alias:", MARGIN, y + 2, LBL_W, ROW_H);
    g_msaa_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        EDIT_X, y, 200, ROW_H + 6, hwnd, (HMENU)(INT_PTR)IDC_MSAA_SLIDER, nullptr, nullptr);
    SendMessageW(g_msaa_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 3));
    SendMessageW(g_msaa_slider, TBM_SETPOS, TRUE, 0);
    SendMessageW(g_msaa_slider, TBM_SETTICFREQ, 1, 0);
    g_msaa_value = CreateWindowExW(0, L"STATIC", L"Off",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        EDIT_X + 206, y + 4, 50, ROW_H, hwnd, (HMENU)(INT_PTR)IDC_MSAA_VALUE, nullptr, nullptr);
    SendMessageW(g_msaa_value, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += ROW_H + GAP;

    // Anisotropic filtering slider: positions 0-4 -> Off, 2x, 4x, 8x, 16x
    CreateLabel(hwnd, L"Anisotropic:", MARGIN, y + 2, LBL_W, ROW_H);
    g_aniso_slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        EDIT_X, y, 200, ROW_H + 6, hwnd, (HMENU)(INT_PTR)IDC_ANISO_SLIDER, nullptr, nullptr);
    SendMessageW(g_aniso_slider, TBM_SETRANGE, TRUE, MAKELONG(0, 4));
    SendMessageW(g_aniso_slider, TBM_SETPOS, TRUE, 0);
    SendMessageW(g_aniso_slider, TBM_SETTICFREQ, 1, 0);
    g_aniso_value = CreateWindowExW(0, L"STATIC", L"Off",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        EDIT_X + 206, y + 4, 50, ROW_H, hwnd, (HMENU)(INT_PTR)IDC_ANISO_VALUE, nullptr, nullptr);
    SendMessageW(g_aniso_value, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += ROW_H + GAP + 4;

    // --- Verbose + buttons ---
    g_verbose_check = CreateCheck(hwnd, L"Verbose logging", IDC_VERBOSE_CHECK,
                                  EDIT_X, y, 160, ROW_H);
    y += ROW_H + GAP + 2;

    g_convert_btn = CreateBtn(hwnd, L"Convert", IDC_CONVERT_BTN,
                              MARGIN + 120, y, 120, 32);
    g_cancel_btn = CreateBtn(hwnd, L"Cancel", IDC_CANCEL_BTN,
                             MARGIN + 250, y, 100, 32);
    g_open_folder_btn = CreateBtn(hwnd, L"Open Folder", IDC_OPEN_FOLDER_BTN,
                                  MARGIN + 360, y, 110, 32);
    EnableWindow(g_cancel_btn, FALSE);
    EnableWindow(g_open_folder_btn, FALSE);
    y += 32 + GAP + 4;

    // --- Progress ---
    g_progress_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        MARGIN, y, CLIENT_W - 2 * MARGIN, 20, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_BAR, nullptr, nullptr);
    y += 20 + 2;

    g_progress_text = CreateWindowExW(0, L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        MARGIN, y, CLIENT_W - 2 * MARGIN, ROW_H, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_TEXT, nullptr, nullptr);
    SendMessageW(g_progress_text, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += ROW_H + GAP;

    // --- Log ---
    {
        HWND lbl = CreateWindowExW(0, L"STATIC", L"Log",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            MARGIN, y, 60, ROW_H, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    y += ROW_H;

    g_log_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        MARGIN, y, CLIENT_W - 2 * MARGIN, 140,
        hwnd, (HMENU)(INT_PTR)IDC_LOG_EDIT, nullptr, nullptr);
    SendMessageW(g_log_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
}

// --- Worker thread ---

static void WorkerThread(AppConfig config) {
    // Install log callback that posts to UI thread
    converter_set_log_callback([](int level, const char* msg) {
        char* copy = _strdup(msg);
        PostMessageW(g_hwnd, WM_APP_LOG, (WPARAM)level, (LPARAM)copy);
    });

    // Install emulator log callback
    emulator_set_log_callback([](int level, const char* msg) {
        char* copy = _strdup(msg);
        PostMessageW(g_hwnd, WM_APP_LOG, (WPARAM)level, (LPARAM)copy);
    });

    // Install progress callback
    converter_set_progress_callback([](int current, int total) {
        PostMessageW(g_hwnd, WM_APP_PROGRESS, (WPARAM)current, (LPARAM)total);
    });

    // Set cancel flag
    converter_set_cancel_flag(&g_cancel_flag);

    // Collect krec files
    std::vector<std::string> krec_files;

    if (config.batch) {
        for (auto& entry : fs::directory_iterator(config.input_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".krec") {
                krec_files.push_back(entry.path().string());
            }
        }
    } else {
        krec_files.push_back(config.input_path);
    }

    int success = 0;
    int failed = 0;

    for (size_t i = 0; i < krec_files.size(); i++) {
        if (g_cancel_flag.load()) break;

        // Report batch progress
        if (krec_files.size() > 1) {
            PostMessageW(g_hwnd, WM_APP_BATCH, (WPARAM)(i + 1), (LPARAM)krec_files.size());
        }

        std::string output;
        if (config.batch) {
            std::string out_dir = config.output_path.empty()
                ? fs::path(krec_files[i]).parent_path().string()
                : config.output_path;
            fs::path out_file = fs::path(out_dir) / fs::path(krec_files[i]).stem();
            out_file.replace_extension(".mp4");
            output = out_file.string();
        } else {
            output = make_output_path(krec_files[i], config.output_path);
        }

        if (convert_one(krec_files[i], output, config)) {
            success++;
        } else {
            failed++;
        }
    }

    // Reset callbacks
    converter_set_log_callback(nullptr);
    converter_set_progress_callback(nullptr);
    converter_set_cancel_flag(nullptr);
    emulator_set_log_callback(nullptr);

    PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)success, (LPARAM)failed);
}

// --- Read UI into AppConfig ---

static AppConfig ReadConfig() {
    AppConfig cfg;
    cfg.rom_path = GetEditText(g_rom_path);
    cfg.input_path = GetEditText(g_input_path);
    cfg.output_path = GetEditText(g_output_path);
    cfg.batch = (SendMessageW(g_batch_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
    cfg.verbose = (SendMessageW(g_verbose_check, BM_GETCHECK, 0, 0) == BST_CHECKED);

    // Default paths relative to exe
    std::string exe_dir = get_exe_dir();
    cfg.core_path = exe_dir + "Core\\mupen64plus.dll";
    cfg.plugin_dir = exe_dir + "Plugin\\";
    cfg.data_dir = exe_dir + "Data\\";
    cfg.ffmpeg_path = exe_dir + "ffmpeg.exe";

    // Resolution
    int sel = (int)SendMessageW(g_resolution_combo, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < g_num_res_presets) {
        cfg.res_width = g_res_presets[sel].w;
        cfg.res_height = g_res_presets[sel].h;
    }

    // Quality (read value from selected preset)
    {
        int qsel = (int)SendMessageW(g_quality_combo, CB_GETCURSEL, 0, 0);
        const QualityFamily& qf = get_quality_family(GetSelectedEncoderFamily());
        if (qsel >= 0 && qsel < qf.num_presets)
            cfg.crf = qf.presets[qsel].value;
    }

    // FPS
    std::string fps_str = GetEditText(g_fps_edit);
    cfg.fps = atof(fps_str.c_str());

    // Encoder
    int enc_sel = (int)SendMessageW(g_encoder_combo, CB_GETCURSEL, 0, 0);
    if (enc_sel >= 0 && enc_sel < (int)g_encoders.size()) {
        cfg.encoder = g_encoders[enc_sel].codec;
    }

    // Anti-aliasing (MSAA)
    int msaa_pos = (int)SendMessageW(g_msaa_slider, TBM_GETPOS, 0, 0);
    if (msaa_pos >= 0 && msaa_pos <= 3) cfg.msaa = g_msaa_values[msaa_pos];

    // Anisotropic filtering
    int aniso_pos = (int)SendMessageW(g_aniso_slider, TBM_GETPOS, 0, 0);
    if (aniso_pos >= 0 && aniso_pos <= 4) cfg.aniso = g_aniso_values[aniso_pos];

    return cfg;
}

// --- Validate and start conversion ---

static void StartConversion() {
    AppConfig cfg = ReadConfig();

    // Validate
    if (cfg.rom_path.empty()) {
        MessageBoxW(g_hwnd, L"ROM path is required.", L"Validation Error", MB_ICONWARNING);
        return;
    }
    if (cfg.input_path.empty()) {
        MessageBoxW(g_hwnd, L"Input path is required.", L"Validation Error", MB_ICONWARNING);
        return;
    }
    if (cfg.batch && !fs::is_directory(cfg.input_path)) {
        MessageBoxW(g_hwnd, L"In batch mode, input must be a directory.", L"Validation Error", MB_ICONWARNING);
        return;
    }
    if (cfg.batch && cfg.output_path.empty()) {
        MessageBoxW(g_hwnd, L"In batch mode, an output directory is required.", L"Validation Error", MB_ICONWARNING);
        return;
    }
    if (cfg.batch && !fs::is_directory(cfg.output_path)) {
        MessageBoxW(g_hwnd, L"In batch mode, output must be an existing directory.", L"Validation Error", MB_ICONWARNING);
        return;
    }
    if (!cfg.batch && !fs::is_regular_file(cfg.input_path)) {
        MessageBoxW(g_hwnd, L"Input file does not exist.", L"Validation Error", MB_ICONWARNING);
        return;
    }

    // Clear log
    SetWindowTextW(g_log_edit, L"");
    SetWindowTextW(g_progress_text, L"Starting...");
    SendMessageW(g_progress_bar, PBM_SETPOS, 0, 0);

    // Toggle buttons
    EnableWindow(g_convert_btn, FALSE);
    EnableWindow(g_cancel_btn, TRUE);
    EnableWindow(g_open_folder_btn, FALSE);
    g_converting = true;
    g_cancel_flag.store(false);
    QueryPerformanceFrequency(&g_perf_freq);
    QueryPerformanceCounter(&g_start_time);
    g_file_start_time = g_start_time;

    // Launch worker
    if (g_worker_thread.joinable()) g_worker_thread.join();
    g_worker_thread = std::thread(WorkerThread, std::move(cfg));
}

// --- Window Procedure ---

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        LoadSettings();
        return 0;

    case WM_HSCROLL: {
        HWND slider = (HWND)lParam;
        if (slider == g_msaa_slider) {
            int pos = (int)SendMessageW(g_msaa_slider, TBM_GETPOS, 0, 0);
            if (pos >= 0 && pos <= 3)
                SetWindowTextW(g_msaa_value, g_msaa_labels[pos]);
        } else if (slider == g_aniso_slider) {
            int pos = (int)SendMessageW(g_aniso_slider, TBM_GETPOS, 0, 0);
            if (pos >= 0 && pos <= 4)
                SetWindowTextW(g_aniso_value, g_aniso_labels[pos]);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        // Encoder changed -> repopulate quality combo
        if (id == IDC_ENCODER && code == CBN_SELCHANGE) {
            PopulateQualityCombo(GetSelectedEncoderFamily());
            return 0;
        }

        switch (id) {
        case IDC_ROM_BROWSE: {
            std::wstring dir = GetEditDir(g_rom_path);
            auto f = BrowseFile(hwnd, L"Select ROM",
                L"N64 ROM Files (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All Files\0*.*\0", false,
                nullptr, dir.empty() ? nullptr : dir.c_str());
            if (!f.empty()) SetWindowTextW(g_rom_path, f.c_str());
            break;
        }
        case IDC_INPUT_BROWSE: {
            std::wstring dir = GetEditDir(g_input_path);
            bool batch = (SendMessageW(g_batch_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (batch) {
                auto f = BrowseFolder(hwnd, L"Select input folder",
                    dir.empty() ? nullptr : dir.c_str());
                if (!f.empty()) SetWindowTextW(g_input_path, f.c_str());
            } else {
                auto f = BrowseFile(hwnd, L"Select .krec file",
                    L"Krec Files (*.krec)\0*.krec\0All Files\0*.*\0", false,
                    nullptr, dir.empty() ? nullptr : dir.c_str());
                if (!f.empty()) SetWindowTextW(g_input_path, f.c_str());
            }
            break;
        }
        case IDC_OUTPUT_BROWSE: {
            std::wstring dir = GetEditDir(g_output_path);
            bool batch = (SendMessageW(g_batch_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (batch) {
                auto f = BrowseFolder(hwnd, L"Select output folder",
                    dir.empty() ? nullptr : dir.c_str());
                if (!f.empty()) SetWindowTextW(g_output_path, f.c_str());
            } else {
                auto f = BrowseFile(hwnd, L"Save output .mp4",
                    L"MP4 Video (*.mp4)\0*.mp4\0All Files\0*.*\0", true, L"mp4",
                    dir.empty() ? nullptr : dir.c_str());
                if (!f.empty()) SetWindowTextW(g_output_path, f.c_str());
            }
            break;
        }
        case IDC_CONVERT_BTN:
            StartConversion();
            break;
        case IDC_CANCEL_BTN:
            if (g_converting) {
                g_cancel_flag.store(true);
                SetWindowTextW(g_progress_text, L"Cancelling...");
                EnableWindow(g_cancel_btn, FALSE);
            }
            break;
        case IDC_OPEN_FOLDER_BTN: {
            // Determine output directory
            std::string out = GetEditText(g_output_path);
            if (out.empty()) out = GetEditText(g_input_path);
            if (!out.empty()) {
                fs::path p(out);
                fs::path dir = fs::is_directory(p) ? p : p.parent_path();
                if (fs::is_directory(dir)) {
                    ShellExecuteW(hwnd, L"open", dir.wstring().c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }
        }
        return 0;
    }

    case WM_APP_BATCH: {
        int current_file = (int)wParam;
        int total_files = (int)lParam;
        // Reset per-file timer so fps/ETA are accurate for each file
        QueryPerformanceCounter(&g_file_start_time);
        wchar_t title[256];
        swprintf(title, 256, L"Krec2MP4 \u2014 File %d / %d", current_file, total_files);
        SetWindowTextW(hwnd, title);
        return 0;
    }

    case WM_APP_LOG: {
        char* str = (char*)lParam;
        if (str) {
            std::wstring wide = Utf8ToWide(str);
            wide += L"\r\n";
            AppendLog(wide.c_str());
            free(str);
        }
        return 0;
    }

    case WM_APP_PROGRESS: {
        int current = (int)wParam;
        int total = (int)lParam;
        if (current == -1) {
            // Muxing phase: switch to marquee mode
            LONG style = GetWindowLongW(g_progress_bar, GWL_STYLE);
            SetWindowLongW(g_progress_bar, GWL_STYLE, style | PBS_MARQUEE);
            SendMessageW(g_progress_bar, PBM_SETMARQUEE, TRUE, 30);
            SetWindowTextW(g_progress_text, L"Muxing video + audio...");
        } else if (total > 0) {
            // Normal frame progress: ensure marquee is off
            LONG style = GetWindowLongW(g_progress_bar, GWL_STYLE);
            if (style & PBS_MARQUEE) {
                SendMessageW(g_progress_bar, PBM_SETMARQUEE, FALSE, 0);
                SetWindowLongW(g_progress_bar, GWL_STYLE, style & ~PBS_MARQUEE);
            }
            int pct = (current * 100) / total;
            SendMessageW(g_progress_bar, PBM_SETRANGE32, 0, total);
            SendMessageW(g_progress_bar, PBM_SETPOS, current, 0);

            // Calculate encoding speed and time estimates
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart - g_start_time.QuadPart) / g_perf_freq.QuadPart;
            double file_elapsed = (double)(now.QuadPart - g_file_start_time.QuadPart) / g_perf_freq.QuadPart;
            double enc_fps = (file_elapsed > 0.0) ? current / file_elapsed : 0.0;
            double speed_mult = enc_fps / 60.0;

            int remaining = total - current;
            double eta = (enc_fps > 0.0) ? remaining / enc_fps : 0.0;

            int el_m = (int)elapsed / 60;
            int el_s = (int)elapsed % 60;
            int eta_m = (int)eta / 60;
            int eta_s = (int)eta % 60;

            wchar_t buf[256];
            swprintf(buf, 256, L"Frame %d / %d (%d%%) \u2014 %.0f fps (%.1fx) \u2014 %d:%02d elapsed, %d:%02d remaining",
                     current, total, pct, enc_fps, speed_mult, el_m, el_s, eta_m, eta_s);
            SetWindowTextW(g_progress_text, buf);
        }
        return 0;
    }

    case WM_APP_DONE: {
        int success = (int)wParam;
        int failed = (int)lParam;

        if (g_worker_thread.joinable()) g_worker_thread.join();
        g_converting = false;
        EnableWindow(g_convert_btn, TRUE);
        EnableWindow(g_cancel_btn, FALSE);
        SetWindowTextW(hwnd, L"Krec2MP4 - N64 Replay to Video Converter");

        // Stop marquee mode if active
        LONG pstyle = GetWindowLongW(g_progress_bar, GWL_STYLE);
        if (pstyle & PBS_MARQUEE) {
            SendMessageW(g_progress_bar, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongW(g_progress_bar, GWL_STYLE, pstyle & ~PBS_MARQUEE);
        }

        // Calculate total elapsed time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - g_start_time.QuadPart) / g_perf_freq.QuadPart;
        int el_m = (int)elapsed / 60;
        int el_s = (int)elapsed % 60;

        wchar_t buf[256];
        if (g_cancel_flag.load()) {
            swprintf(buf, 256, L"Cancelled. Success: %d, Failed: %d (%d:%02d)",
                     success, failed, el_m, el_s);
        } else {
            swprintf(buf, 256, L"Done! Success: %d, Failed: %d (%d:%02d)",
                     success, failed, el_m, el_s);
        }
        SetWindowTextW(g_progress_text, buf);

        if (!g_cancel_flag.load() && failed == 0 && success > 0) {
            SendMessageW(g_progress_bar, PBM_SETRANGE32, 0, 100);
            SendMessageW(g_progress_bar, PBM_SETPOS, 100, 0);
        }

        // Enable Open Folder if there's an output path
        std::string out = GetEditText(g_output_path);
        if (out.empty()) out = GetEditText(g_input_path);
        if (!out.empty()) {
            EnableWindow(g_open_folder_btn, TRUE);
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_converting) {
            int ret = MessageBoxW(hwnd,
                L"A conversion is in progress. Cancel and exit?",
                L"Confirm Exit", MB_YESNO | MB_ICONQUESTION);
            if (ret != IDYES) return 0;
            g_cancel_flag.store(true);
            if (g_worker_thread.joinable()) g_worker_thread.join();
        }
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_font) {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- Entry Point ---

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    // Probe available encoders (tests GPU encoders against ffmpeg)
    std::string exe_dir_init = get_exe_dir();
    g_encoders = probe_available_encoders(exe_dir_init + "ffmpeg.exe");

    // Create UI font
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APPICON),
                                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"Krec2MP4_GUI";
    RegisterClassExW(&wc);

    // Calculate window size to fit client area
    const int CLIENT_W = 620;
    const int CLIENT_H = 670;
    RECT rc = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE, 0);

    g_hwnd = CreateWindowExW(0, L"Krec2MP4_GUI",
        L"Krec2MP4 - N64 Replay to Video Converter",
        (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
