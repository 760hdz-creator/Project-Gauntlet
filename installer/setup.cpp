// Project Gauntlet installer. Ships no game code or data: it installs the build kit (our lifter, runtime source,
// embedded Python) and builds the game on this PC from the player's own Gauntlet: Dark Legacy (Xbox, USA) disc image.
//   1. unpack the embedded payload.zip (System32\tar.exe) into the install folder
//   2. install the build tools if missing (installer\install_toolchain.ps1, elevated: VS Build Tools C++, LLVM)
//   3. python\python.exe -I tools\gt_build.py --root <dir> --iso <image> (extract, check, lift, compile)
//   4. shortcuts, per-user record HKCU\Software\ProjectGauntlet, uninstall entry
// Over an existing install it offers an update: new kit + rebuild from the installed game data (no disc needed).
// Command line: --update [--dir <folder>] (what the game's updater runs), --uninstall [--dir <folder>],
// --iso <image> [--dir <folder>] [--hd <pack folder>] (full install, starts at once: testing).
// The install keeps a copy of this exe as <install>\launcher.exe; "--launcher" opens the game launcher (Play, update
// check, display settings, HD texture pack) instead of the setup. Settings go to <install>\settings.ini [settings] as
// GT_* environment variable names, which the game reads at start (runtime/src/main.c load_settings).
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
#include <winhttp.h>
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
       ID_STATUS, ID_HD_EDIT, ID_HD_BROWSE, ID_HD_USE, ID_HD_STATUS, ID_PLAY, ID_UPDATE, ID_UNINSTALL, ID_FULLSCREEN,
       ID_WIDESCREEN, ID_SCALE, ID_MSAA, ID_ANISO, ID_PADS, ID_SHARPEN, ID_FXAA, ID_CRT, ID_CONTROLS, ID_CTL_BACK,
       ID_CTL_DEFAULTS, ID_FPS60, ID_KEY0 = 2000, ID_PADM0 = 2100 };
#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)
#define WM_APP_UPDATE (WM_APP + 4)   // launcher: update check finished (wp 1 = newer version in g_upd)
#define WM_APP_UPDATING (WM_APP + 5) // launcher: wp 1 = new setup started (close), 0 = download failed

static HWND g_wnd, g_status, g_progress, g_iso, g_dir, g_install, g_hd, g_hdStatus, g_hdUse, g_updBtn;
static bool g_updating, g_running, g_ok, g_launcher;
static std::wstring g_instDir;   // launcher: the install folder (the exe's own folder)
// Buttons are owner-drawn (dark, on the art). GWLP_USERDATA: 1 push button, 2 toggle (checkbox), | 4 when ticked.
static bool IsOn(int id) { return (GetWindowLongPtrW(GetDlgItem(g_wnd, id), GWLP_USERDATA) & 4) != 0; }
static void SetOn(int id, bool on) {
    HWND c = GetDlgItem(g_wnd, id);
    LONG_PTR v = GetWindowLongPtrW(c, GWLP_USERDATA);
    SetWindowLongPtrW(c, GWLP_USERDATA, on ? v | 4 : v & ~4);
    InvalidateRect(c, nullptr, FALSE);
}
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
static void MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir,
                         const wchar_t* args = L"") {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(target.c_str());
    sl->SetArguments(args);
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}
// settings.ini next to the game: [settings] holds GT_* variables for the game, [launcher] the launcher's own state.
// An empty value deletes the key (the game then uses its default, so a later default change reaches the player).
static std::wstring IniGet(const std::wstring& dir, const wchar_t* sec, const wchar_t* key) {
    wchar_t b[1024];
    GetPrivateProfileStringW(sec, key, L"", b, 1024, (dir + L"\\settings.ini").c_str());
    return b;
}
static void IniSet(const std::wstring& dir, const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.empty() ? nullptr : v.c_str(), (dir + L"\\settings.ini").c_str());
}
// PNG files under d (the game's HD texture loader scans the folder recursively).
static int CountPngs(const std::wstring& d, int depth = 0) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8) n += CountPngs(d + L"\\" + fd.cFileName, depth + 1);
        } else {
            size_t l = wcslen(fd.cFileName);
            if (l > 4 && !_wcsicmp(fd.cFileName + l - 4, L".png")) n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
static std::wstring ExeDir() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring d = self;
    return d.substr(0, d.find_last_of(L'\\'));
}
// Copies this exe to <dir>\launcher.exe. Retries for 15 s: the launcher that started this update may still be closing.
static bool InstallLauncher(const std::wstring& dir) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring l = dir + L"\\launcher.exe";
    if (!_wcsicmp(self, l.c_str())) return true;
    for (int i = 0; i < 30; i++) {
        if (CopyFileW(self, l.c_str(), FALSE)) return true;
        Sleep(500);
    }
    return false;
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

static void Work(std::wstring iso, std::wstring dir, std::wstring hd, bool desktop, bool startMenu, bool update) {
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
    // the launcher (a copy of this setup) is what the shortcuts and the uninstall entry run
    if (!InstallLauncher(dir)) {
        g_error = L"Could not write " + dir + L"\\launcher.exe (is the launcher still open?).";
        return done(false);
    }
    std::wstring un = dir + L"\\launcher.exe";
    DeleteFileW((dir + L"\\uninstall.exe").c_str());   // 0.1.0 kept the uninstaller there
    if (!hd.empty()) {
        IniSet(dir, L"launcher", L"HdFolder", hd);
        IniSet(dir, L"settings", L"GT_HDTEX", hd);
    }
    // shortcuts: the ticked ones, and on an update the existing ones (older ones pointed at gauntlet.exe)
    std::wstring desk = KnownDir(FOLDERID_Desktop) + L"\\" + kShortcut, sm = KnownDir(FOLDERID_Programs) + L"\\" + kShortcut;
    if (desktop || Exists(desk)) MakeShortcut(desk, un, dir, L"--launcher");
    if (startMenu || Exists(sm)) MakeShortcut(sm, un, dir, L"--launcher");
    SetReg(HKEY_CURRENT_USER, kRegKey, L"InstallLocation", dir);
    SetReg(HKEY_CURRENT_USER, kRegKey, L"Version", kVersion);
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
    if (MessageBoxW(nullptr, (L"Remove Project Gauntlet from " + dir + L"?\n\nYour saves (out\\hdd) are kept. An HD "
                              L"texture pack in its own folder is not touched.").c_str(),
                    kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 1;
    // shortcuts and registry entries are shared: only remove them when they belong to this folder
    wchar_t a[MAX_PATH] = L"", b[MAX_PATH] = L"";
    std::wstring reg = GetReg(L"InstallLocation");
    bool registered = reg.empty() || (GetFullPathNameW(reg.c_str(), MAX_PATH, a, nullptr) &&
                                      GetFullPathNameW(dir.c_str(), MAX_PATH, b, nullptr) && !_wcsicmp(a, b));
    if (registered) {
        DeleteFileW((KnownDir(FOLDERID_Desktop) + L"\\" + kShortcut).c_str());
        DeleteFileW((KnownDir(FOLDERID_Programs) + L"\\" + kShortcut).c_str());
    }
    const wchar_t* parts[] = {L"build", L"game", L"python", L"recomp", L"runtime", L"tools", L"installer", L"licenses"};
    for (auto p : parts) {
        std::wstring from = dir + L"\\" + p + std::wstring(1, L'\0');
        SHFILEOPSTRUCTW op = {nullptr, FO_DELETE, from.c_str(), nullptr, FOF_NO_UI};
        SHFileOperationW(&op);
    }
    for (auto f : {L"gauntlet.exe", L"gauntlet.ico", L"README.md", L"VERSION", L"settings.ini"})
        DeleteFileW((dir + L"\\" + f).c_str());
    if (registered) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kRegKey);
        RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    }
    MessageBoxW(nullptr, L"Project Gauntlet was removed. Your saves are kept in the install folder's out\\hdd.", kAppName,
                MB_OK);
    // the launcher (or an old uninstall.exe) is deleted once this process has exited
    std::wstring cmd = L"cmd /c ping -n 3 127.0.0.1 >nul & del \"" + dir + L"\\launcher.exe\" & del \"" + dir +
                       L"\\uninstall.exe\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return 0;
}

// ---- update check (forge releases, as in runtime/src/updater.c) ----
// GET https url; body to *out, or to the file `to`.
static bool HttpGet(const std::wstring& url, std::string* out, const wchar_t* to, DWORD* status = nullptr) {
    URL_COMPONENTS uc = {sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen((L"ProjectGauntlet/" + std::wstring(kVersion)).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              nullptr, nullptr, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = r && WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);
    DWORD code = 0, cl = sizeof code;
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &cl, nullptr);
    if (status) *status = code;
    ok = ok && code == 200;
    FILE* f = nullptr;
    if (ok && to) ok = (f = _wfopen(to, L"wb")) != nullptr;
    char chunk[65536];
    DWORD got;
    while (ok && WinHttpReadData(r, chunk, sizeof chunk, &got) && got) {
        if (f) ok = fwrite(chunk, 1, got, f) == got;
        else if (out) out->append(chunk, got);
    }
    if (f) fclose(f);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
// The JSON string value after "key": from pos (advanced past it); unescapes \x to x and \n.
static bool JsonStr(const std::string& j, const char* key, size_t& pos, std::string& v) {
    size_t p = j.find(std::string("\"") + key + "\"", pos);
    if (p == std::string::npos) return false;
    p = j.find('"', p + strlen(key) + 2);
    if (p == std::string::npos) return false;
    v.clear();
    for (p++; p < j.size() && j[p] != '"'; p++) {
        char ch = j[p];
        if (ch == '\\' && p + 1 < j.size()) { p++; ch = j[p] == 'n' ? '\n' : j[p]; }
        v += ch;
    }
    pos = p;
    return true;
}
static unsigned long long VerNum(const char* v) {
    unsigned a = 0, b = 0, c = 0;
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%u.%u.%u", &a, &b, &c);
    return ((unsigned long long)a << 40) | ((unsigned long long)b << 20) | c;
}
struct UpdInfo { std::string tag, url, notes; };
static UpdInfo g_upd;
static void CheckUpdates() {
    const char *repo = getenv("GT_UPDATE_REPO"), *asset = getenv("GT_UPDATE_ASSET");   // testing overrides
    if (!asset) asset = "ProjectGauntlet-Setup";
    std::wstring api = L"https://git.8gramlabs.com/api/v1/repos/" +
                       (repo ? Widen(repo) : std::wstring(L"8gramlabs/Project-Gauntlet")) + L"/releases/latest";
    std::string body, name;
    DWORD status = 0;
    bool ok = HttpGet(api, &body, nullptr, &status), newer = false;
    if (ok) {
        size_t pos = 0;
        JsonStr(body, "tag_name", pos, g_upd.tag);
        pos = 0;
        JsonStr(body, "body", pos, g_upd.notes);
        pos = 0;
        while (JsonStr(body, "name", pos, name))   // assets: "name" ... "browser_download_url"
            if (!name.compare(0, strlen(asset), asset) && name.find(".exe") != std::string::npos) {
                JsonStr(body, "browser_download_url", pos, g_upd.url);
                break;
            }
        newer = !g_upd.tag.empty() && !g_upd.url.empty() && VerNum(g_upd.tag.c_str()) > VerNum(GT_SETUP_VERSION);
    }
    PostMessageW(g_wnd, WM_APP_UPDATE, newer, ok || status == 404);   // 404: no release published yet
}
// Downloads the new setup to %TEMP% and starts it as "--update --dir <install>"; the launcher then closes.
static void DownloadUpdate() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring setup = std::wstring(tmp) + L"ProjectGauntlet-Setup-" + Widen(g_upd.tag) + L".exe";
    bool ok = HttpGet(Widen(g_upd.url), nullptr, setup.c_str());
    if (ok) {
        std::wstring cmd = L"\"" + setup + L"\" --update --dir \"" + g_instDir + L"\"";
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        ok = CreateProcessW(setup.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
        if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    PostMessageW(g_wnd, WM_APP_UPDATING, ok, 0);
}

// ---- launcher settings (settings.ini [settings], GT_* names the game reads) ----
struct Choice { const wchar_t* label; const wchar_t* value; };   // value nullptr: the game's default (key removed)
static const Choice kScale[] = {{L"Auto (fits your screen)", nullptr}, {L"2x  (960 lines)", L"2"}, {L"3x  (1440 lines)", L"3"},
                                {L"4x  (1920 lines)", L"4"}, {L"5x  (2400 lines)", L"5"}, {L"6x  (2880 lines)", L"6"}};
static const Choice kMsaa[] = {{L"Off", L"1"}, {L"2x MSAA", L"2"}, {L"4x MSAA (default)", nullptr}, {L"8x MSAA", L"8"}};
static const Choice kAniso[] = {{L"Trilinear", L"1"}, {L"2x anisotropic", L"2"}, {L"4x anisotropic", L"4"},
                                {L"8x anisotropic", L"8"}, {L"16x anisotropic (default)", nullptr}};
static const Choice kPads[] = {{L"1 (default)", nullptr}, {L"2", L"2"}, {L"3", L"3"}, {L"4", L"4"}};
static const Choice kSharpen[] = {{L"Off (default)", nullptr}, {L"Low", L"30"}, {L"Medium", L"60"}, {L"High", L"100"}};
struct ComboSetting { int id; const wchar_t* key; const Choice* c; int n; };
static const ComboSetting kCombos[] = {{ID_SCALE, L"GT_SCALE", kScale, 6}, {ID_MSAA, L"GT_MSAA", kMsaa, 4},
                                       {ID_ANISO, L"GT_ANISO", kAniso, 5}, {ID_PADS, L"GT_PADS", kPads, 4},
                                       {ID_SHARPEN, L"GT_SHARPEN", kSharpen, 4}};

static void UpdateHdStatus() {
    if (!g_hdStatus) return;
    std::wstring d = GetText(g_hd);
    if (d.empty()) {
        SetWindowTextW(g_hdStatus, L"Pick the pack's folder, for example ...\\SLUS-20047\\replacements.");
        return;
    }
    int n = CountPngs(d);
    SetWindowTextW(g_hdStatus, n ? (std::to_wstring(n) + L" HD textures found.").c_str()
                                 : L"No PNG files in this folder. Pick the PCSX2 pack's folder.");
}
static void LoadSettings() {
    for (auto& cs : kCombos) {
        std::wstring v = IniGet(g_instDir, L"settings", cs.key);
        int sel = 0;
        for (int i = 0; i < cs.n; i++)
            if (cs.c[i].value ? v == cs.c[i].value : v.empty()) sel = i;
        SendDlgItemMessageW(g_wnd, cs.id, CB_SETCURSEL, sel, 0);
    }
    SetOn(ID_FULLSCREEN, IniGet(g_instDir, L"settings", L"GT_FULLSCREEN") == L"1");
    SetOn(ID_WIDESCREEN, IniGet(g_instDir, L"settings", L"GT_WIDESCREEN") != L"0");
    SetOn(ID_FXAA, IniGet(g_instDir, L"settings", L"GT_FXAA") == L"1");
    SetOn(ID_CRT, IniGet(g_instDir, L"settings", L"GT_CRT") == L"1");
    SetOn(ID_FPS60, IniGet(g_instDir, L"settings", L"GT_FPS") == L"60");
    std::wstring tex = IniGet(g_instDir, L"settings", L"GT_HDTEX"), folder = IniGet(g_instDir, L"launcher", L"HdFolder");
    if (folder.empty() && tex != L"0") folder = tex;
    SetOn(ID_HD_USE, tex != L"0");
    SetWindowTextW(g_hd, folder.c_str());
    UpdateHdStatus();
}
static void SaveSettings() {
    for (auto& cs : kCombos) {
        int sel = (int)SendDlgItemMessageW(g_wnd, cs.id, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < cs.n) IniSet(g_instDir, L"settings", cs.key, cs.c[sel].value ? cs.c[sel].value : L"");
    }
    IniSet(g_instDir, L"settings", L"GT_FULLSCREEN", IsOn(ID_FULLSCREEN) ? L"1" : L"");
    IniSet(g_instDir, L"settings", L"GT_WIDESCREEN", IsOn(ID_WIDESCREEN) ? L"" : L"0");
    IniSet(g_instDir, L"settings", L"GT_FXAA", IsOn(ID_FXAA) ? L"1" : L"");
    IniSet(g_instDir, L"settings", L"GT_CRT", IsOn(ID_CRT) ? L"1" : L"");
    IniSet(g_instDir, L"settings", L"GT_FPS", IsOn(ID_FPS60) ? L"60" : L"");
    std::wstring folder = GetText(g_hd);
    IniSet(g_instDir, L"launcher", L"HdFolder", folder);
    IniSet(g_instDir, L"settings", L"GT_HDTEX", !IsOn(ID_HD_USE) ? L"0" : folder);
}
static bool PlayGame() {
    SetEnvironmentVariableW(L"GT_NOUPDATE", L"1");   // the launcher already checked
    std::wstring exe = g_instDir + L"\\gauntlet.exe", cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_instDir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---- window ----
static const int kArtW = 1600, kArtH = 893;
static RECT kPanel = {60, 140, 760, 860};   // art pixels: the translucent control panel (dark left side)
static double g_scale = 1;
static HDC g_artDC;
static HBITMAP g_artBmp;
static HFONT g_font, g_titleFont, g_smallFont;
static HBRUSH g_fieldBrush, g_panelBrush, g_artBrush;   // g_artBrush: the panel art, behind labels
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


// ---- controls page (settings.ini GT_KEYS / GT_PADMAP, read by the game's hle.c) ----
static const wchar_t* kActNames[18] = {L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right", L"START", L"BACK",
                                       L"A", L"B", L"X", L"Y", L"Black", L"White", L"Left trigger", L"Right trigger",
                                       L"Move up", L"Move down", L"Move left", L"Move right"};
static const int kKeyDef[18] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK, 'Z', 'X', 'C', 'V', 'F', 'G',
                                'Q', 'E', 'W', 'S', 'A', 'D'};
static const int kPadDef[14] = {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 9, 8, 16, 17};
struct PadSrc { const wchar_t* label; int v; };   // v: XInput wButtons bit, 16 LT, 17 RT, 18 none
static const PadSrc kSrc[] = {{L"D-pad up", 0}, {L"D-pad down", 1}, {L"D-pad left", 2}, {L"D-pad right", 3},
                              {L"START", 4}, {L"BACK", 5}, {L"Left stick click", 6}, {L"Right stick click", 7},
                              {L"LB", 8}, {L"RB", 9}, {L"A", 12}, {L"B", 13}, {L"X", 14}, {L"Y", 15}, {L"LT", 16},
                              {L"RT", 17}, {L"(none)", 18}};
static const int kNumSrc = sizeof kSrc / sizeof kSrc[0];
static int g_keys[18], g_padm[14], g_capture = -1, g_page;
static std::vector<HWND> g_pageCtl[2];
static std::vector<bool> g_mainVis;

static std::wstring KeyName(int vk) {
    if (!vk) return L"(none)";
    LONG l = (LONG)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU: l |= 1 << 24;
    }
    wchar_t b[64];
    if (GetKeyNameTextW(l, b, 64) > 0) return b;
    swprintf(b, 64, L"Key %02X", vk);
    return b;
}
static void LoadMaps() {
    std::copy(kKeyDef, kKeyDef + 18, g_keys);
    std::copy(kPadDef, kPadDef + 14, g_padm);
    auto parse = [](const std::wstring& s, int* dst, int n, int base, int max) {
        if (s.empty()) return;
        std::vector<int> v;
        const wchar_t* p = s.c_str();
        while (*p) {
            wchar_t* e;
            long x = wcstol(p, &e, base);
            if (e == p || x < 0 || x > max) return;
            v.push_back((int)x);
            if (*e != L',') break;
            p = e + 1;
        }
        if ((int)v.size() == n) std::copy(v.begin(), v.end(), dst);
    };
    parse(IniGet(g_instDir, L"settings", L"GT_KEYS"), g_keys, 18, 16, 255);
    parse(IniGet(g_instDir, L"settings", L"GT_PADMAP"), g_padm, 14, 10, 18);
}
static void SaveMaps() {
    std::wstring k, p;
    for (int i = 0; i < 18; i++) { wchar_t b[8]; swprintf(b, 8, i ? L",%X" : L"%X", g_keys[i]); k += b; }
    for (int i = 0; i < 14; i++) p += (i ? L"," : L"") + std::to_wstring(g_padm[i]);
    IniSet(g_instDir, L"settings", L"GT_KEYS", std::equal(g_keys, g_keys + 18, kKeyDef) ? L"" : k);
    IniSet(g_instDir, L"settings", L"GT_PADMAP", std::equal(g_padm, g_padm + 14, kPadDef) ? L"" : p);
}
static void RefreshMaps() {
    for (int i = 0; i < 18; i++)
        SetDlgItemTextW(g_wnd, ID_KEY0 + i, i == g_capture ? L"Press a key..." : KeyName(g_keys[i]).c_str());
    for (int i = 0; i < 14; i++) {
        int sel = kNumSrc - 1;
        for (int k = 0; k < kNumSrc; k++) if (kSrc[k].v == g_padm[i]) sel = k;
        SendDlgItemMessageW(g_wnd, ID_PADM0 + i, CB_SETCURSEL, sel, 0);
    }
}
// Esc cancels; F8 / F11 are the game's own keys. A key bound elsewhere moves to this action.
static void CaptureKey(int vk) {
    int row = g_capture;
    g_capture = -1;
    if (vk != VK_ESCAPE && vk != VK_F8 && vk != VK_F11) {
        for (int i = 0; i < 18; i++) if (g_keys[i] == vk) g_keys[i] = 0;
        g_keys[row] = vk;
        SaveMaps();
    }
    RefreshMaps();
}
static void RebuildArt();
static void ShowPage(int page) {
    if (page == g_page) return;
    if (page == 1) {
        g_mainVis.clear();
        for (HWND c : g_pageCtl[0]) { g_mainVis.push_back(IsWindowVisible(c) != 0); ShowWindow(c, SW_HIDE); }
        for (HWND c : g_pageCtl[1]) ShowWindow(c, SW_SHOW);
    } else {
        for (HWND c : g_pageCtl[1]) ShowWindow(c, SW_HIDE);
        for (size_t i = 0; i < g_pageCtl[0].size(); i++) ShowWindow(g_pageCtl[0][i], g_mainVis[i] ? SW_SHOW : SW_HIDE);
    }
    g_page = page;
    g_capture = -1;
    RebuildArt();
    SetFocus(GetDlgItem(g_wnd, page ? ID_CTL_BACK : ID_PLAY));
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
    int x = kPanel.left + 30, t = kPanel.top;
    if (g_launcher && g_page == 1) {
        text(x, t + 24, g_titleFont, kAccent, L"Controls");
        text(x, t + 84, g_smallFont, kDim, L"Click a key, then press the new one (Esc cancels). Saved at once.");
        text(x, t + 124, g_font, kAccent, L"Action");
        text(x + 250, t + 124, g_font, kAccent, L"Keyboard");
        text(x + 450, t + 124, g_font, kAccent, L"Gamepad button");
        for (int i = 0; i < 18; i++) {
            text(x, t + 168 + i * 32, g_smallFont, kText, kActNames[i]);
            if (i >= 14) text(x + 450, t + 168 + i * 32, g_smallFont, kDim, L"Left stick");
        }
        return;
    }
    if (g_launcher) {
        text(x, t + 24, g_titleFont, kAccent, L"Project Gauntlet");
        {   // long install paths end in an ellipsis inside the panel
            std::wstring v = L"Version " + std::wstring(kVersion) + L"   " + g_instDir;
            SelectObject(g_artDC, g_smallFont);
            SetTextColor(g_artDC, kDim);
            RECT r = {S(x), S(t + 84), S(kPanel.right - 20), S(t + 120)};
            DrawTextW(g_artDC, v.data(), (int)v.size(), &r, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        }
        text(x, t + 262, g_font, kAccent, L"Display");
        text(x, t + 382, g_font, kText, L"Resolution");
        text(x, t + 426, g_font, kText, L"Anti-aliasing");
        text(x, t + 470, g_font, kText, L"Texture filtering");
        text(x, t + 514, g_font, kText, L"Sharpening");
        text(x, t + 558, g_font, kText, L"Players (gamepads)");
        text(x, t + 608, g_font, kAccent, L"HD textures");
        return;
    }
    text(x, t + 24, g_titleFont, kAccent, g_updating ? L"Update Project Gauntlet" : L"Install Project Gauntlet");
    text(x, t + 84, g_smallFont, kDim, L"Builds the game on this PC from your own disc image. Version " + std::wstring(kVersion) + L".");
    if (!g_updating) text(x, t + 130, g_font, kText, L"Gauntlet: Dark Legacy disc image (Xbox, USA)");
    text(x, t + 230, g_font, kText, L"Install folder");
    if (!g_updating) text(x, t + 330, g_font, kText, L"HD texture pack folder (optional, PCSX2 format)");
}

static HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h,
                 HFONT font = nullptr) {
    LONG_PTR kind = 0;
    if (!wcscmp(cls, L"BUTTON")) {
        kind = (style & BS_TYPEMASK) == BS_AUTOCHECKBOX ? 2 : 1;
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
    }
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    if (kind) SetWindowLongPtrW(c, GWLP_USERDATA, kind);
    return c;
}
// Lines the art brush up with control c, so its background is the art behind it.
static HBRUSH ArtBrushFor(HDC dc, HWND c) {
    RECT rc;
    GetWindowRect(c, &rc);
    MapWindowPoints(nullptr, g_wnd, (POINT*)&rc, 2);
    SetBrushOrgEx(dc, -rc.left, -rc.top, nullptr);
    return g_artBrush;
}
static void DrawButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, ArtBrushFor(dc, di->hwndItem));
    LONG_PTR kind = GetWindowLongPtrW(di->hwndItem, GWLP_USERDATA);
    bool dis = di->itemState & ODS_DISABLED, focus = di->itemState & ODS_FOCUS, down = di->itemState & ODS_SELECTED;
    wchar_t text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dis ? kDim : kText);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), focus ? kAccent : RGB(80, 150, 45));
    HGDIOBJ op = SelectObject(dc, pen);
    if (kind & 2) {   // toggle: box + label
        int b = S(24), y0 = (r.top + r.bottom - b) / 2;
        HBRUSH fill = CreateSolidBrush(RGB(18, 26, 18));
        HGDIOBJ ob = SelectObject(dc, fill);
        Rectangle(dc, r.left + S(2), y0, r.left + S(2) + b, y0 + b);
        SelectObject(dc, ob);
        DeleteObject(fill);
        if (kind & 4) {
            RECT in = {r.left + S(2) + S(6), y0 + S(6), r.left + S(2) + b - S(6), y0 + b - S(6)};
            HBRUSH on = CreateSolidBrush(kAccent);
            FillRect(dc, &in, on);
            DeleteObject(on);
        }
        RECT tr = {r.left + b + S(14), r.top, r.right, r.bottom};
        DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else {          // push button
        HBRUSH fill = CreateSolidBrush(dis ? RGB(16, 22, 16) : down ? RGB(60, 120, 35) : RGB(26, 50, 20));
        HGDIOBJ ob = SelectObject(dc, fill);
        RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, S(10), S(10));
        SelectObject(dc, ob);
        DeleteObject(fill);
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
}
static HWND MakeCheck(const wchar_t* text, int id, int x, int y, int w) {
    return Make(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y, w, 34);
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
static std::wstring BrowseFolder() {
    std::wstring r;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    DWORD o;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS);
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(g_wnd)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) r = p;
        CoTaskMemFree(p);
        it->Release();
    }
    d->Release();
    return r;
}
static void BrowseDir() {
    std::wstring s = BrowseFolder();
    if (s.empty()) return;
    if (s.size() < 16 || _wcsicmp(s.c_str() + s.size() - 16, L"Project Gauntlet")) s += L"\\Project Gauntlet";
    SetWindowTextW(g_dir, s.c_str());
}
static void BrowseHd() {
    std::wstring s = BrowseFolder();
    if (s.empty()) return;
    SetWindowTextW(g_hd, s.c_str());
    UpdateHdStatus();
    if (g_launcher) {
        SetOn(ID_HD_USE, true);
        SaveSettings();
    }
}

static void Start() {
    std::wstring iso = g_iso ? GetText(g_iso) : L"", dir = GetText(g_dir), hd = g_hd ? GetText(g_hd) : L"";
    if (!g_updating && !Exists(iso)) { MessageBoxW(g_wnd, L"Choose your Gauntlet: Dark Legacy disc image.", kAppName, MB_OK); return; }
    if (dir.empty()) return;
    if (!hd.empty() && !CountPngs(hd) &&
        MessageBoxW(g_wnd, L"The HD texture folder has no PNG files. Install without HD textures?", kAppName,
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (!hd.empty() && !CountPngs(hd)) hd.clear();
    g_running = true;
    EnableWindow(g_install, FALSE);
    bool desk = IsOn(ID_DESKTOP), sm = IsOn(ID_STARTMENU);
    g_worker = std::thread(Work, iso, dir, hd, desk, sm, g_updating);
}

// After an install: open the launcher.
static void Launch(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", (dir + L"\\launcher.exe").c_str(), L"--launcher", dir.c_str(), SW_SHOWNORMAL);
}

static bool LauncherCommand(HWND h, int id, int code) {
    if (id >= ID_KEY0 && id < ID_KEY0 + 18) {
        if (code == BN_CLICKED) { g_capture = id - ID_KEY0; RefreshMaps(); }
        return true;
    }
    if (id >= ID_PADM0 && id < ID_PADM0 + 14) {
        int sel = (int)SendDlgItemMessageW(h, id, CB_GETCURSEL, 0, 0);
        if (code == CBN_SELCHANGE && sel >= 0 && sel < kNumSrc) { g_padm[id - ID_PADM0] = kSrc[sel].v; SaveMaps(); }
        return true;
    }
    switch (id) {
    case ID_CONTROLS: ShowPage(1); return true;
    case ID_CTL_BACK: ShowPage(0); return true;
    case ID_CTL_DEFAULTS:
        std::copy(kKeyDef, kKeyDef + 18, g_keys);
        std::copy(kPadDef, kPadDef + 14, g_padm);
        g_capture = -1;
        SaveMaps();
        RefreshMaps();
        return true;
    case ID_PLAY:
        SaveSettings();
        if (PlayGame()) DestroyWindow(h);
        else MessageBoxW(h, (L"Could not start " + g_instDir + L"\\gauntlet.exe.").c_str(), kAppName, MB_OK | MB_ICONERROR);
        return true;
    case ID_UPDATE:
        if (g_worker.joinable()) return true;
        EnableWindow(g_updBtn, FALSE);
        SetWindowTextW(g_status, (L"Downloading version " + Widen(g_upd.tag) + L"...").c_str());
        g_worker = std::thread(DownloadUpdate);
        return true;
    case ID_UNINSTALL:
        if (Uninstall(g_instDir) == 0) DestroyWindow(h);
        return true;
    case ID_HD_BROWSE: BrowseHd(); return true;
    case ID_HD_EDIT:
        if (code == EN_KILLFOCUS) { UpdateHdStatus(); SaveSettings(); }
        return true;
    case ID_FULLSCREEN: case ID_WIDESCREEN: case ID_HD_USE: case ID_FXAA: case ID_CRT: case ID_FPS60:
        if (code == BN_CLICKED) SaveSettings();
        return true;
    case ID_SCALE: case ID_MSAA: case ID_ANISO: case ID_PADS: case ID_SHARPEN:
        if (code == CBN_SELCHANGE) SaveSettings();
        return true;
    }
    return false;
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
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, RGB(18, 26, 18));
        return (LRESULT)g_fieldBrush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)wp, (HWND)lp == g_status || (HWND)lp == g_hdStatus ? kDim : kText);
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ArtBrushFor((HDC)wp, (HWND)lp);
    case WM_DRAWITEM: DrawButton((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_COMMAND:
        if ((HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED) && lp &&
            (GetWindowLongPtrW((HWND)lp, GWLP_USERDATA) & 2)) {
            SetOn(LOWORD(wp), !IsOn(LOWORD(wp)));
            wp = MAKEWPARAM(LOWORD(wp), BN_CLICKED);
        }
        if (g_launcher && LauncherCommand(h, LOWORD(wp), HIWORD(wp))) return 0;
        switch (LOWORD(wp)) {
        case ID_ISO_BROWSE: BrowseIso(); break;
        case ID_DIR_BROWSE: BrowseDir(); break;
        case ID_HD_BROWSE: BrowseHd(); break;
        case ID_HD_EDIT: if (HIWORD(wp) == EN_KILLFOCUS) UpdateHdStatus(); break;
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
    case WM_APP_UPDATE:
        if (g_worker.joinable()) g_worker.join();
        if (wp) {
            SetWindowTextW(g_status, (L"Version " + Widen(g_upd.tag) + L" is available (you have " + kVersion +
                                      L"). Update rebuilds the game in a few minutes; saves are kept.").c_str());
            ShowWindow(g_updBtn, SW_SHOW);
            EnableWindow(g_updBtn, TRUE);
        } else {
            SetWindowTextW(g_status, lp ? (std::wstring(L"Up to date (version ") + kVersion + L").").c_str()
                                        : L"Could not check for updates (offline?).");
        }
        return 0;
    case WM_APP_UPDATING:
        if (g_worker.joinable()) g_worker.join();
        if (wp) { DestroyWindow(h); return 0; }
        SetWindowTextW(g_status, L"The update could not be downloaded. Try again later.");
        EnableWindow(g_updBtn, TRUE);
        return 0;
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        g_running = false;
        if (wp) {
            SetWindowTextW(g_status, (std::wstring(g_updating ? L"Updated to version " : L"Installed version ") + kVersion +
                                      L" in " + g_doneDir + L". Play opens the launcher (settings, HD textures).").c_str());
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

static void RebuildArt() {
    DeleteDC(g_artDC);
    DeleteObject(g_artBmp);
    BuildArt(S(kArtW), S(kArtH));
    DeleteObject(g_artBrush);
    g_artBrush = CreatePatternBrush(g_artBmp);
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void MakeCombo(int id, const Choice* c, int n, int x, int y, int w, HFONT font = nullptr) {
    HWND cb = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, x, y, w, 300, font);
    SetWindowTheme(cb, L"", L"");
    for (int i = 0; i < n; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)c[i].label);
}

static void CreateLauncherControls() {
    int x = kPanel.left + 30, t = kPanel.top, fw = kPanel.right - kPanel.left - 60, cx = x + 260, cw = fw - 260;
    HWND play = Make(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_PLAY, x, t + 130, 240, 64, g_titleFont);
    g_updBtn = Make(L"BUTTON", L"Update", BS_PUSHBUTTON | WS_TABSTOP, ID_UPDATE, x + 260, t + 130, 200, 64);
    ShowWindow(g_updBtn, SW_HIDE);
    g_status = Make(L"STATIC", L"Checking for updates...", SS_LEFT, ID_STATUS, x, t + 205, fw, 50, g_smallFont);
    MakeCheck(L"Fullscreen (F11 toggles)", ID_FULLSCREEN, x, t + 296, 300);
    MakeCheck(L"Widescreen", ID_WIDESCREEN, x + 320, t + 296, 160);
    MakeCheck(L"FXAA (smooths edges)", ID_FXAA, x, t + 334, 300);
    MakeCheck(L"CRT scanlines", ID_CRT, x + 320, t + 334, 300);
    MakeCheck(L"60 fps (beta)", ID_FPS60, x + 490, t + 296, 190);
    MakeCombo(ID_SCALE, kScale, 6, cx, t + 376, cw);
    MakeCombo(ID_MSAA, kMsaa, 4, cx, t + 420, cw);
    MakeCombo(ID_ANISO, kAniso, 5, cx, t + 464, cw);
    MakeCombo(ID_SHARPEN, kSharpen, 4, cx, t + 508, cw);
    MakeCombo(ID_PADS, kPads, 4, cx, t + 552, cw);
    g_hdUse = MakeCheck(L"Use the HD texture pack", ID_HD_USE, x, t + 646, 400);
    g_hd = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, ID_HD_EDIT, x, t + 686, fw - 130, 38);
    Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, ID_HD_BROWSE, x + fw - 120, t + 686, 120, 38);
    g_hdStatus = Make(L"STATIC", L"", SS_LEFT, ID_HD_STATUS, x, t + 734, fw - 190, 50, g_smallFont);
    Make(L"BUTTON", L"Uninstall", BS_PUSHBUTTON | WS_TABSTOP, ID_UNINSTALL, x + fw - 170, t + 740, 170, 40, g_smallFont);
    Make(L"BUTTON", L"Controls", BS_PUSHBUTTON | WS_TABSTOP, ID_CONTROLS, x + fw - 200, t + 130, 200, 64);
    LoadSettings();
    // controls page, hidden until Controls is clicked
    auto children = [] {
        std::vector<HWND> v;
        EnumChildWindows(g_wnd, [](HWND c, LPARAM p) { ((std::vector<HWND>*)p)->push_back(c); return TRUE; }, (LPARAM)&v);
        return v;
    };
    g_pageCtl[0] = children();
    for (int i = 0; i < 18; i++)
        Make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, ID_KEY0 + i, x + 250, t + 164 + i * 32, 180, 30, g_smallFont);
    std::vector<Choice> src;
    for (auto& s : kSrc) src.push_back({s.label, nullptr});
    for (int i = 0; i < 14; i++) MakeCombo(ID_PADM0 + i, src.data(), kNumSrc, x + 450, t + 164 + i * 32, fw - 450, g_smallFont);
    Make(L"BUTTON", L"Defaults", BS_PUSHBUTTON | WS_TABSTOP, ID_CTL_DEFAULTS, x, t + 745, 170, 40, g_smallFont);
    Make(L"BUTTON", L"Back", BS_PUSHBUTTON | WS_TABSTOP, ID_CTL_BACK, x + fw - 170, t + 745, 170, 40, g_smallFont);
    for (HWND c : children())
        if (std::find(g_pageCtl[0].begin(), g_pageCtl[0].end(), c) == g_pageCtl[0].end()) {
            g_pageCtl[1].push_back(c);
            ShowWindow(c, SW_HIDE);
        }
    LoadMaps();
    RefreshMaps();
    SetFocus(play);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dirArg, isoArg, hdArg;
    bool update = false, uninstall = false, launcher = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--update")) update = true;
        else if (!wcscmp(argv[i], L"--uninstall")) uninstall = true;
        else if (!wcscmp(argv[i], L"--launcher")) launcher = true;
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dirArg = argv[++i];
        else if (!wcscmp(argv[i], L"--iso") && i + 1 < argc) isoArg = argv[++i];
        else if (!wcscmp(argv[i], L"--hd") && i + 1 < argc) hdArg = argv[++i];
    }
    if (launcher) {   // the installed copy: its own folder is the install
        g_instDir = dirArg.empty() ? ExeDir() : dirArg;
        if (!Exists(g_instDir + L"\\gauntlet.exe")) {
            MessageBoxW(nullptr, (L"The game is not built in " + g_instDir + L". Run the Project Gauntlet setup.").c_str(),
                        kAppName, MB_OK | MB_ICONERROR);
            return 1;
        }
        g_launcher = true;
        kPanel = {60, 50, 800, 850};
    }
    std::wstring dir = dirArg.empty() ? DefaultDir() : dirArg;
    if (uninstall) return Uninstall(dir);
    bool installed = Exists(dir + L"\\game\\Default.xbe") && Exists(dir + L"\\gauntlet.exe");
    if (!launcher && !update && installed) {
        std::wstring q = std::wstring(kAppName) + L" is installed in " + dir + L" (version " + GetReg(L"Version") +
                         L"; this installer " + kVersion + L").\n\nYes: update it (keeps game data, settings and saves; "
                         L"no disc image needed).\nNo: full reinstall from a disc image.";
        int r = MessageBoxW(nullptr, q.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        update = r == IDYES;
    }
    if (!launcher && update && !installed) {
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
    g_artBrush = CreatePatternBrush(g_artBmp);

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
    g_wnd = CreateWindowW(wc.lpszClassName, launcher ? L"Project Gauntlet" : L"Project Gauntlet Setup", style,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (launcher) {
        CreateLauncherControls();
        ShowWindow(g_wnd, SW_SHOW);
        g_worker = std::thread(CheckUpdates);
    } else {
        int x = kPanel.left + 30, t = kPanel.top, fw = kPanel.right - kPanel.left - 60;
        if (!update) {
            g_iso = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, ID_ISO_EDIT, x, t + 165, fw - 130, 38);
            Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_ISO_BROWSE, x + fw - 120, t + 165, 120, 38);
        }
        g_dir = Make(L"EDIT", dir.c_str(), ES_AUTOHSCROLL | WS_BORDER | (update ? ES_READONLY : 0), ID_DIR_EDIT, x,
                     t + 265, fw - 130, 38);
        if (!update) {
            Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_DIR_BROWSE, x + fw - 120, t + 265, 120, 38);
            g_hd = Make(L"EDIT", hdArg.c_str(), ES_AUTOHSCROLL | WS_BORDER, ID_HD_EDIT, x, t + 365, fw - 130, 38);
            Make(L"BUTTON", L"Browse...", BS_PUSHBUTTON, ID_HD_BROWSE, x + fw - 120, t + 365, 120, 38);
            g_hdStatus = Make(L"STATIC", L"", SS_LEFT, ID_HD_STATUS, x, t + 410, fw, 30, g_smallFont);
            UpdateHdStatus();
        }
        HWND d = MakeCheck(L"Desktop shortcut", ID_DESKTOP, x, t + 455, 260);
        HWND s = MakeCheck(L"Start menu shortcut", ID_STARTMENU, x + 280, t + 455, 280);
        (void)d; (void)s;
        SetOn(ID_DESKTOP, !update);
        SetOn(ID_STARTMENU, !update);
        g_install = Make(L"BUTTON", update ? L"Update" : L"Install", BS_DEFPUSHBUTTON, ID_INSTALL, x, t + 505, 200, 52);
        g_progress = Make(PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PROGRESS, x, t + 580, fw, 20);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(g_progress, PBM_SETBARCOLOR, 0, RGB(110, 220, 40));
        SendMessageW(g_progress, PBM_SETBKCOLOR, 0, RGB(18, 26, 18));
        g_status = Make(L"STATIC", update ? L"Ready to update. Your game data, settings and saves are kept."
                                           : L"First install: the build tools (about 3 GB) are installed if missing, then "
                                             L"the game is built (10-20 minutes).",
                        SS_LEFT, ID_STATUS, x, t + 615, fw, 80);
        ShowWindow(g_wnd, SW_SHOW);
        if (g_iso && !isoArg.empty()) SetWindowTextW(g_iso, isoArg.c_str());
        // started by the game's updater (--update --dir) or with --iso: go straight away
        if ((update && !dirArg.empty()) || (!update && !isoArg.empty())) Start();
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_capture >= 0 && (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
            CaptureKey((int)msg.wParam);
            continue;
        }
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    // launcher: an update check still waiting on the network must not keep the process alive
    if (g_worker.joinable()) { if (g_launcher) g_worker.detach(); else g_worker.join(); }
    return 0;
}
