// Project Gauntlet installer. Ships no game code or data: it installs the build kit (our lifter, runtime source,
// embedded Python) and builds the game on this PC from the player's own Gauntlet: Dark Legacy (Xbox, USA) disc image.
//   1. unpack the embedded payload.zip (System32\tar.exe) into the install folder
//   2. install the build tools if missing (installer\install_toolchain.ps1, elevated: VS Build Tools C++, LLVM)
//   3. python\python.exe -I tools\gt_build.py --root <dir> --iso <image> (extract, check, lift, compile)
//   4. shortcuts, per-user record HKCU\Software\ProjectGauntlet, uninstall entry
// Over an existing install it offers an update: new kit + rebuild from the installed game data (no disc needed).
// Command line: --update [--dir <folder>] (what the game's updater runs), --uninstall [--dir <folder>],
// --iso <image> [--dir <folder>] (full install, starts at once: testing).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"

#ifndef GT_SETUP_VERSION
#define GT_SETUP_VERSION "0.0.0"
#endif
#define W2(x) L##x
#define W1(x) W2(x)
static const wchar_t kVersion[] = W1(GT_SETUP_VERSION);
static const wchar_t kAppName[] = L"Project Gauntlet";
static const wchar_t kRegKey[] = L"Software\\ProjectGauntlet";
static const wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ProjectGauntlet";
static const wchar_t kShortcut[] = L"Project Gauntlet.lnk";

enum { ID_ISO_EDIT = 200, ID_ISO_BROWSE, ID_DIR_EDIT, ID_DIR_BROWSE, ID_DESKTOP, ID_STARTMENU, ID_INSTALL, ID_PROGRESS,
       ID_STATUS };
#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)

static HWND g_wnd, g_status, g_progress, g_iso, g_dir, g_install;
static bool g_updating, g_running, g_ok;
static std::wstring g_error, g_doneDir;
static std::thread g_worker;

// ---- small helpers ----
static void PostStatus(const std::wstring& s) { if (g_wnd) PostMessageW(g_wnd, WM_APP_STATUS, 0, (LPARAM) new std::wstring(s)); }
static void PostProgress(int pct) { if (g_wnd) PostMessageW(g_wnd, WM_APP_PROGRESS, pct, 0); }
static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
static void SetReg(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * 2));
        RegCloseKey(k);
    }
}
static std::wstring GetReg(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS) return buf;
    return L"";
}
static bool WriteResource(int id, const std::wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    if (!g) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0, n = SizeofResource(nullptr, r);
    bool ok = WriteFile(f, LockResource(g), n, &w, nullptr) && w == n;
    CloseHandle(f);
    return ok;
}
static void MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}
// Runs cmd (hidden), feeding each output line to onLine. Returns the exit code (or -1).
template <class F> static int RunLines(std::wstring cmd, const std::wstring& cwd, F onLine) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    std::string buf;
    char chunk[4096];
    DWORD n;
    while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t p;
        while ((p = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line);
            buf.erase(0, p + 1);
        }
    }
    if (!buf.empty()) onLine(buf);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// ---- build tools ----
static bool HaveToolchain() {
    wchar_t pf86[MAX_PATH], pf[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%", pf86, MAX_PATH);
    ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH);
    if (!Exists(std::wstring(pf) + L"\\LLVM\\bin\\clang-cl.exe")) return false;
    std::wstring vswhere = std::wstring(pf86) + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!Exists(vswhere)) return false;
    std::string out;
    RunLines(L"\"" + vswhere + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
             L"-property installationPath", L"", [&](const std::string& l) { out += l; });
    return !out.empty();
}
static bool InstallToolchain(const std::wstring& dir) {
    PostStatus(L"Installing the build tools (Visual Studio Build Tools C++ and LLVM, about 3 GB from Microsoft and "
               L"llvm.org). Accept the administrator prompt; this takes 5-20 minutes.");
    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + dir + L"\\installer\\install_toolchain.ps1\"";
    SHELLEXECUTEINFOW se = {sizeof se};
    se.fMask = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb = L"runas";
    se.lpFile = L"powershell.exe";
    se.lpParameters = args.c_str();
    se.nShow = SW_SHOWMINNOACTIVE;
    if (!ShellExecuteExW(&se) || !se.hProcess) return false;
    WaitForSingleObject(se.hProcess, INFINITE);
    CloseHandle(se.hProcess);
    return HaveToolchain();
}

// ---- install / update worker ----
static bool Unpack(const std::wstring& dir) {
    PostStatus(L"Copying files...");
    CreateDirectoryW(dir.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring zip = dir + L"\\payload.zip";
    if (!WriteResource(IDR_PAYLOAD, zip)) return false;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    int code = RunLines(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + dir + L"\"", L"",
                        [](const std::string&) {});
    DeleteFileW(zip.c_str());
    WriteResource(IDR_SETUP_ICON, dir + L"\\gauntlet.ico");
    return code == 0;
}

static void Work(std::wstring iso, std::wstring dir, bool desktop, bool startMenu, bool update) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_error.clear();
    g_ok = false;
    auto done = [&](bool ok) { g_ok = ok; g_doneDir = dir; PostMessageW(g_wnd, WM_APP_DONE, ok, 0); CoUninitialize(); };
    PostProgress(2);
    if (!Unpack(dir)) { g_error = L"Could not write the files to " + dir + L"."; return done(false); }
    PostProgress(5);
    if (!HaveToolchain() && !InstallToolchain(dir)) {
        g_error = L"The build tools are not installed. Log: %TEMP%\\ProjectGauntlet_toolchain.log";
        return done(false);
    }
    PostProgress(10);
    std::wstring cmd = L"\"" + dir + L"\\python\\python.exe\" -I \"" + dir + L"\\tools\\gt_build.py\" --root \"" + dir +
                       L"\" --version " + kVersion;
    if (!update) cmd += L" --iso \"" + iso + L"\"";
    // weights: extract 10-30, match 30-38, lift 38-55, compile 55-97
    int base = 10;
    HANDLE log = CreateFileW((dir + L"\\build\\setup.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        CreateDirectoryW((dir + L"\\build").c_str(), nullptr);
        log = CreateFileW((dir + L"\\build\\setup.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, 0, nullptr);
    }
    std::wstring err;
    int code = RunLines(cmd, dir, [&](const std::string& l) {
        DWORD w;
        if (log != INVALID_HANDLE_VALUE) { WriteFile(log, l.data(), (DWORD)l.size(), &w, nullptr); WriteFile(log, "\r\n", 2, &w, nullptr); }
        if (!l.compare(0, 3, "## ")) {
            std::wstring s = Widen(l.substr(3));
            PostStatus(s + L"...");
            if (s.rfind(L"Extracting", 0) == 0) base = 12;
            else if (s.rfind(L"Checking", 0) == 0) base = 30;
            else if (s.rfind(L"Matching", 0) == 0) base = 32;
            else if (s.rfind(L"Translating", 0) == 0) base = 40;
            else if (s.rfind(L"Compiling", 0) == 0) base = 55;
            else if (s.rfind(L"Linking", 0) == 0) base = 97;
            PostProgress(base);
        } else if (!l.compare(0, 7, "ERROR: ")) {
            err = Widen(l.substr(7));
        } else if (!l.compare(0, 9, "compiled ")) {
            int a = 0, b = 0;
            size_t p = l.rfind('(');
            if (p != std::string::npos && sscanf(l.c_str() + p, "(%d/%d)", &a, &b) == 2 && b)
                PostProgress(55 + 42 * a / b);
        }
    });
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (code != 0 || !Exists(dir + L"\\gauntlet.exe")) {
        g_error = (err.empty() ? L"The build failed." : err) + L"\n\nLog: " + dir + L"\\build\\setup.log";
        return done(false);
    }
    std::wstring exe = dir + L"\\gauntlet.exe";
    if (desktop) MakeShortcut(KnownDir(FOLDERID_Desktop) + L"\\" + kShortcut, exe, dir);
    if (startMenu) MakeShortcut(KnownDir(FOLDERID_Programs) + L"\\" + kShortcut, exe, dir);
    SetReg(HKEY_CURRENT_USER, kRegKey, L"InstallLocation", dir);
    SetReg(HKEY_CURRENT_USER, kRegKey, L"Version", kVersion);
    // uninstall entry: a copy of this setup runs --uninstall
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring un = dir + L"\\uninstall.exe";
    if (_wcsicmp(self, un.c_str())) CopyFileW(self, un.c_str(), FALSE);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"DisplayName", kAppName);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"DisplayVersion", kVersion);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"Publisher", L"8gram Labs");
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"DisplayIcon", dir + L"\\gauntlet.ico");
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", dir);
    SetReg(HKEY_CURRENT_USER, kUninstKey, L"UninstallString", L"\"" + un + L"\" --uninstall --dir \"" + dir + L"\"");
    PostProgress(100);
    done(true);
}

static int Uninstall(const std::wstring& dir) {
    if (dir.empty() || !Exists(dir + L"\\gauntlet.ico")) {
        MessageBoxW(nullptr, L"Project Gauntlet is not installed.", kAppName, MB_OK);
        return 1;
    }
    if (MessageBoxW(nullptr, (L"Remove Project Gauntlet from " + dir + L"?\n\nYour saves (out\\hdd) are kept.").c_str(),
                    kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    DeleteFileW((KnownDir(FOLDERID_Desktop) + L"\\" + kShortcut).c_str());
    DeleteFileW((KnownDir(FOLDERID_Programs) + L"\\" + kShortcut).c_str());
    const wchar_t* parts[] = {L"build", L"game", L"python", L"recomp", L"runtime", L"tools", L"installer", L"licenses"};
    for (auto p : parts) {
        std::wstring from = dir + L"\\" + p + std::wstring(1, L'\0');
        SHFILEOPSTRUCTW op = {nullptr, FO_DELETE, from.c_str(), nullptr, FOF_NO_UI};
        SHFileOperationW(&op);
    }
    for (auto f : {L"gauntlet.exe", L"gauntlet.ico", L"README.md", L"VERSION"}) DeleteFileW((dir + L"\\" + f).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kRegKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    MessageBoxW(nullptr, L"Project Gauntlet was removed. Your saves are kept in the install folder's out\\hdd.", kAppName,
                MB_OK);
    // uninstall.exe deletes itself once this process has exited
    std::wstring cmd = L"cmd /c ping -n 3 127.0.0.1 >nul & del \"" + dir + L"\\uninstall.exe\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return 0;
}

// ---- window ----
static const int kArtW = 1600, kArtH = 893;
static const RECT kPanel = {60, 250, 760, 840};   // art pixels: the translucent control panel (dark left side)
static double g_scale = 1;
static HDC g_artDC;
static HBITMAP g_artBmp;
static HFONT g_font, g_titleFont, g_smallFont;
static HBRUSH g_fieldBrush, g_panelBrush;
static const COLORREF kText = RGB(232, 240, 220), kDim = RGB(160, 180, 150), kAccent = RGB(140, 255, 60);
static int S(int v) { return (int)lround(v * g_scale); }

static HBITMAP LoadArt(int w, int h) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_INSTALLER_BG), RT_RCDATA);
    if (!r) return nullptr;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    IWICStream* s = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    HBITMAP bmp = nullptr;
    f->CreateStream(&s);
    if (s && SUCCEEDED(s->InitializeFromMemory((BYTE*)LockResource(LoadResource(nullptr, r)), SizeofResource(nullptr, r))) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &d)) &&
        SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
        SUCCEEDED(sc->Initialize(fr, w, h, WICBitmapInterpolationModeFant)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)bits);
    }
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (fr) fr->Release();
    if (d) d->Release();
    if (s) s->Release();
    f->Release();
    return bmp;
}

// The art with the darkened panel painted in (kept in g_artDC; WM_PAINT copies it).
static void BuildArt(int w, int h) {
    HDC screen = GetDC(nullptr);
    g_artDC = CreateCompatibleDC(screen);
    g_artBmp = LoadArt(w, h);
    if (!g_artBmp) g_artBmp = CreateCompatibleBitmap(screen, w, h);
    SelectObject(g_artDC, g_artBmp);
    ReleaseDC(nullptr, screen);
    RECT p = {S(kPanel.left), S(kPanel.top), S(kPanel.right), S(kPanel.bottom)};
    HDC tmp = CreateCompatibleDC(g_artDC);
    HBITMAP b = CreateCompatibleBitmap(g_artDC, 1, 1);
    HGDIOBJ old = SelectObject(tmp, b);
    SetPixel(tmp, 0, 0, RGB(4, 10, 6));
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 200, 0};
    GdiAlphaBlend(g_artDC, p.left, p.top, p.right - p.left, p.bottom - p.top, tmp, 0, 0, 1, 1, bf);
    SelectObject(tmp, old);
    DeleteObject(b);
    DeleteDC(tmp);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), RGB(70, 140, 40));
    HGDIOBJ op = SelectObject(g_artDC, pen), ob = SelectObject(g_artDC, GetStockObject(NULL_BRUSH));
    Rectangle(g_artDC, p.left, p.top, p.right, p.bottom);
    SelectObject(g_artDC, op); SelectObject(g_artDC, ob);
    DeleteObject(pen);
    // static labels
    SetBkMode(g_artDC, TRANSPARENT);
    auto text = [&](int x, int y, HFONT f, COLORREF c, const std::wstring& s) {
        SelectObject(g_artDC, f);
        SetTextColor(g_artDC, c);
        TextOutW(g_artDC, S(x), S(y), s.c_str(), (int)s.size());
    };
    text(kPanel.left + 30, kPanel.top + 24, g_titleFont, kAccent, g_updating ? L"Update Project Gauntlet" : L"Install Project Gauntlet");
    text(kPanel.left + 30, kPanel.top + 84, g_smallFont, kDim,
         L"Builds the game on this PC from your own disc image. Version " + std::wstring(kVersion) + L".");
    if (!g_updating) text(kPanel.left + 30, kPanel.top + 130, g_font, kText, L"Gauntlet: Dark Legacy disc image (Xbox, USA)");
    text(kPanel.left + 30, kPanel.top + 230, g_font, kText, L"Install folder");
}

static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void BrowseIso() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Xbox disc image (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&of)) SetWindowTextW(g_iso, file);
}
static void BrowseDir() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS);
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(g_wnd)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            std::wstring s = p;
            if (s.size() < 16 || _wcsicmp(s.c_str() + s.size() - 16, L"Project Gauntlet")) s += L"\\Project Gauntlet";
            SetWindowTextW(g_dir, s.c_str());
        }
        CoTaskMemFree(p);
        it->Release();
    }
    d->Release();
}

static void Start() {
    std::wstring iso = g_iso ? GetText(g_iso) : L"", dir = GetText(g_dir);
    if (!g_updating && !Exists(iso)) { MessageBoxW(g_wnd, L"Choose your Gauntlet: Dark Legacy disc image.", kAppName, MB_OK); return; }
    if (dir.empty()) return;
    g_running = true;
    EnableWindow(g_install, FALSE);
    bool desk = IsDlgButtonChecked(g_wnd, ID_DESKTOP) == BST_CHECKED, sm = IsDlgButtonChecked(g_wnd, ID_STARTMENU) == BST_CHECKED;
    g_worker = std::thread(Work, iso, dir, desk, sm, g_updating);
}

static void Launch(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", (dir + L"\\gauntlet.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, g_artDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, RGB(18, 26, 18));
        return (LRESULT)g_fieldBrush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, RGB(8, 14, 9));
        return (LRESULT)g_panelBrush;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ISO_BROWSE: BrowseIso(); break;
        case ID_DIR_BROWSE: BrowseDir(); break;
        case ID_INSTALL:
            if (!g_running && !g_doneDir.empty()) { Launch(g_doneDir); DestroyWindow(h); }
            else if (!g_running) Start();
            break;
        }
        return 0;
    case WM_APP_STATUS: {
        auto* s = (std::wstring*)lp;
        SetWindowTextW(g_status, s->c_str());
        delete s;
        return 0;
    }
    case WM_APP_PROGRESS: SendMessageW(g_progress, PBM_SETPOS, wp, 0); return 0;
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        g_running = false;
        if (wp) {
            SetWindowTextW(g_status, (std::wstring(g_updating ? L"Updated to version " : L"Installed version ") + kVersion +
                                      L" in " + g_doneDir + L".").c_str());
            SetWindowTextW(g_install, L"Play");
            EnableWindow(g_install, TRUE);
        } else {
            g_doneDir.clear();
            SetWindowTextW(g_status, L"Failed. See the message.");
            MessageBoxW(h, g_error.c_str(), kAppName, MB_OK | MB_ICONERROR);
            EnableWindow(g_install, TRUE);
        }
        return 0;
    case WM_CLOSE:
        if (g_running && MessageBoxW(h, L"The build is still running. Quit anyway?", kAppName, MB_YESNO) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static std::wstring DefaultDir() {
    std::wstring rec = GetReg(L"InstallLocation");
    if (!rec.empty()) return rec;
    return KnownDir(FOLDERID_LocalAppData) + L"\\Project Gauntlet";
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dirArg, isoArg;
    bool update = false, uninstall = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--update")) update = true;
        else if (!wcscmp(argv[i], L"--uninstall")) uninstall = true;
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dirArg = argv[++i];
        else if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) isoArg = argv[++i];
    }
    std::wstring dir = dirArg.empty() ? DefaultDir() : dirArg;
    if (uninstall) return Uninstall(dir);
    bool installed = Exists(dir + L"\\game\\Default.xbe") && Exists(dir + L"\\gauntlet.exe");
    if (!update && installed) {
        std::wstring q = std::wstring(kAppName) + L" is installed in " + dir + L" (version " + GetReg(L"Version") +
                         L"; this installer " + kVersion + L").\n\nYes: update it (keeps game data and saves; no disc "
                         L"image needed).\nNo: full reinstall from a disc image.";
        int r = MessageBoxW(nullptr, q.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        update = r == IDYES;
    }
    if (update && !installed) {
        MessageBoxW(nullptr, (L"No installed game data in " + dir + L". Run a full install.").c_str(), kAppName, MB_OK);
        update = false;
    }
    g_updating = update;

    // window sized to the art, scaled to ~70% of the work area
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_scale = std::min((wa.right - wa.left) * 0.7 / kArtW, (wa.bottom - wa.top) * 0.8 / kArtH);
    int w = S(kArtW), h = S(kArtH);
    g_font = CreateFontW(-S(22), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_smallFont = CreateFontW(-S(18), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_titleFont = CreateFontW(-S(44), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Georgia");
    g_fieldBrush = CreateSolidBrush(RGB(18, 26, 18));
    g_panelBrush = CreateSolidBrush(RGB(8, 14, 9));
    BuildArt(w, h);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ProjectGauntletSetup";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowW(wc.lpszClassName, L"Project Gauntlet Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    int x = kPanel.left + 30, fw = kPanel.right - kPanel.left - 60;
    if (!update) {
        g_iso = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, ID_ISO_EDIT, x, kPanel.top + 165, fw - 130, 38);
        Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_ISO_BROWSE, x + fw - 120, kPanel.top + 165, 120, 38);
    }
    g_dir = Make(L"EDIT", dir.c_str(), ES_AUTOHSCROLL | WS_BORDER | (update ? ES_READONLY : 0), ID_DIR_EDIT, x,
                 kPanel.top + 265, fw - 130, 38);
    if (!update) Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_DIR_BROWSE, x + fw - 120, kPanel.top + 265, 120, 38);
    HWND d = Make(L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX, ID_DESKTOP, x, kPanel.top + 325, 260, 32);
    HWND s = Make(L"BUTTON", L"Start menu shortcut", BS_AUTOCHECKBOX, ID_STARTMENU, x + 280, kPanel.top + 325, 280, 32);
    SetWindowTheme(d, L"", L"");   // unthemed: the classic checkbox honours WM_CTLCOLORSTATIC's text colour
    SetWindowTheme(s, L"", L"");
    SendMessageW(d, BM_SETCHECK, update ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessageW(s, BM_SETCHECK, update ? BST_UNCHECKED : BST_CHECKED, 0);
    g_install = Make(L"BUTTON", update ? L"Update" : L"Install", BS_DEFPUSHBUTTON, ID_INSTALL, x, kPanel.top + 380, 200, 52);
    g_progress = Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PROGRESS, x, kPanel.top + 455, fw, 20);
    SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_progress, PBM_SETBARCOLOR, 0, RGB(110, 220, 40));
    SendMessageW(g_progress, PBM_SETBKCOLOR, 0, RGB(18, 26, 18));
    g_status = Make(L"STATIC", update ? L"Ready to update. Your game data and saves are kept."
                                       : L"First install: the build tools (about 3 GB) are installed if missing, then "
                                         L"the game is built (10-20 minutes).",
                    SS_LEFT, ID_STATUS, x, kPanel.top + 490, fw, 80);
    ShowWindow(g_wnd, SW_SHOW);
    if (g_iso && !isoArg.empty()) SetWindowTextW(g_iso, isoArg.c_str());
    // started by the game's updater (--update --dir) or with --iso: go straight away
    if ((update && !dirArg.empty()) || (!update && !isoArg.empty())) Start();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (g_worker.joinable()) g_worker.join();
    return 0;
}
