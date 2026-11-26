#include "stdafx.h"
#include "WhisparrWatched.h"
#include "mplayerc.h"
#include <PathUtils.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
    std::string Utf8(const CString& s)
    {
        if (s.IsEmpty()) {
            return {};
        }
        const int len = WideCharToMultiByte(CP_UTF8, 0, s, s.GetLength(), nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
            return {};
        }
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s, s.GetLength(), &out[0], len, nullptr, nullptr);
        return out;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        sprintf_s(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

    CString FileNameOf(const CString& path)
    {
        const int pos = std::max(path.ReverseFind(L'\\'), path.ReverseFind(L'/'));
        return (pos >= 0) ? path.Mid(pos + 1) : path;
    }

    std::string BuildDeletedRequestBody(const CString& filePath)
    {
        std::string body = "{\n";
        body += "  \"_type\": \"deleted\",\n";
        body += "  \"fileName\": \"" + JsonEscape(Utf8(FileNameOf(filePath))) + "\",\n";
        body += "  \"filePath\": \"" + JsonEscape(Utf8(filePath)) + "\"\n";
        body += "}\n";
        return body;
    }

    // Pretty-printed on purpose: queued events are meant to be readable.
    std::string BuildRequestBody(const CString& filePath)
    {
        std::string body = "{\n";
        body += "  \"fileName\": \"" + JsonEscape(Utf8(FileNameOf(filePath))) + "\",\n";
        body += "  \"filePath\": \"" + JsonEscape(Utf8(filePath)) + "\",\n";
        body += "  \"viewed\": true\n";
        body += "}\n";
        return body;
    }

    uint64_t HashFileIdentity(const CString& path)
    {
        std::string key = Utf8(path);
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
            const uint64_t size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            const uint64_t mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32)
                                   | fad.ftLastWriteTime.dwLowDateTime;
            key += "|" + std::to_string(size) + "|" + std::to_string(mtime);
        }
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : key) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    std::string ReadAllBytes(const CString& path)
    {
        std::string data;
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return data;
        }
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024) {
            data.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (ReadFile(h, &data[0], static_cast<DWORD>(data.size()), &read, nullptr)) {
                data.resize(read);
            } else {
                data.clear();
            }
        }
        CloseHandle(h);
        return data;
    }

    void WriteAllBytes(const CString& path, const std::string& data)
    {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            CloseHandle(h);
        }
    }

    void WriteFailureInfo(const CString& path, const CString& url, DWORD status, const std::string& response)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char when[64];
        sprintf_s(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        std::string text;
        text += "status=" + std::to_string(status) + "\n";
        text += "url=" + Utf8(url) + "\n";
        text += "time=";
        text += when;
        text += "\n";
        text += "response:\n";
        text += response;
        text += "\n";
        WriteAllBytes(path, text);
    }

    void TouchNow(const CString& path)
    {
        HANDLE h = CreateFileW(path, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            SetFileTime(h, nullptr, nullptr, &now);
            CloseHandle(h);
        }
    }
}

CWhisparrWatched::~CWhisparrWatched()
{
    Stop();
}

CString CWhisparrWatched::GetConfigPath() const
{
    CString dir;
    if (!AfxGetMyApp() || !AfxGetMyApp()->GetAppDataPath(dir) || dir.IsEmpty()) {
        return CString();
    }
    CreateDirectoryW(dir, nullptr);
    return dir + L"\\whisparr.ini";
}

CString CWhisparrWatched::GetOutboxDir() const
{
    CString cfg = GetConfigPath();
    if (cfg.IsEmpty()) {
        return CString();
    }
    const int pos = cfg.ReverseFind(L'\\');
    CString dir = (pos > 0) ? cfg.Left(pos) : cfg;
    CString outbox = dir + L"\\whisparr-outbox";
    CreateDirectoryW(outbox, nullptr);
    return outbox;
}

CString CWhisparrWatched::GetStatusLogPath() const
{
    CString dir;
    if (!AfxGetMyApp() || !AfxGetMyApp()->GetAppDataPath(dir) || dir.IsEmpty()) {
        return CString();
    }
    CreateDirectoryW(dir, nullptr);
    return dir + L"\\whisparr-status.log";
}

void CWhisparrWatched::AppendStatus(const CString& line)
{
    static std::mutex logMutex;
    const CString path = GetStatusLogPath();
    if (path.IsEmpty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(logMutex);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER size;
        size.LowPart = fad.nFileSizeLow;
        size.HighPart = fad.nFileSizeHigh;
        if (size.QuadPart > 512 * 1024) {
            const CString old = path + L".old";
            MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        }
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64];
    swprintf_s(head, _countof(head), L"%04d-%02d-%02d %02d:%02d:%02d ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const CString text = CString(head) + line + L"\r\n";
    const std::string utf8 = Utf8(text);

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(h);
    }
}

void CWhisparrWatched::EnsureConfigTemplate() const
{
    CString cfg = GetConfigPath();
    if (cfg.IsEmpty() || GetFileAttributesW(cfg) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    WritePrivateProfileStringW(L"whisparr", L"enabled", L"0", cfg);
    WritePrivateProfileStringW(L"whisparr", L"url", L"https://whisparr", cfg);
    WritePrivateProfileStringW(L"whisparr", L"apikey", L"", cfg);
    WritePrivateProfileStringW(L"whisparr", L"verifytls", L"0", cfg);
}

CWhisparrWatched::Config CWhisparrWatched::LoadConfig() const
{
    Config c;
    CString cfg = GetConfigPath();
    if (cfg.IsEmpty() || GetFileAttributesW(cfg) == INVALID_FILE_ATTRIBUTES) {
        return c;
    }
    c.enabled = GetPrivateProfileIntW(L"whisparr", L"enabled", 0, cfg) != 0;
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"whisparr", L"url", L"https://whisparr", buf, _countof(buf), cfg);
    c.url = buf;
    GetPrivateProfileStringW(L"whisparr", L"apikey", L"", buf, _countof(buf), cfg);
    c.apiKey = buf;
    c.verifyTls = GetPrivateProfileIntW(L"whisparr", L"verifytls", 0, cfg) != 0;
    c.url.Trim();
    c.apiKey.Trim();
    return c;
}

void CWhisparrWatched::Start()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_thread.joinable()) {
        return;
    }
    EnsureConfigTemplate();
    GetOutboxDir();
    m_stop = false;
    m_wake = true;
    m_thread = std::thread([this]() { WorkerLoop(); });
    AppendStatus(L"WORKER START");
}

void CWhisparrWatched::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_thread.joinable()) {
            return;
        }
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void CWhisparrWatched::Enqueue(const CString& filePath)
{
    if (filePath.IsEmpty()) {
        return;
    }
    CString check = filePath;
    if (PathUtils::IsURL(check)) {
        return;
    }

    const Config c = LoadConfig();
    if (!c.enabled || c.url.IsEmpty() || c.apiKey.IsEmpty()) {
        return;
    }

    const CString outbox = GetOutboxDir();
    if (outbox.IsEmpty()) {
        return;
    }

    const uint64_t hash = HashFileIdentity(filePath);
    CString base;
    base.Format(L"%016llx", static_cast<unsigned long long>(hash));

    // Shard by the first two hash digits so a single directory never holds all
    // pending events (256 buckets).
    const CString shardDir = outbox + L"\\" + base.Left(2);
    CreateDirectoryW(shardDir, nullptr);

    const CString jsonPath = shardDir + L"\\" + base + L".json";
    const CString sendingPath = shardDir + L"\\" + base + L".sending";
    const CString failedPath = shardDir + L"\\" + base + L".failed";
    if (GetFileAttributesW(jsonPath) != INVALID_FILE_ATTRIBUTES
        || GetFileAttributesW(sendingPath) != INVALID_FILE_ATTRIBUTES
        || GetFileAttributesW(failedPath) != INVALID_FILE_ATTRIBUTES) {
        return; // already queued or already handled
    }

    const std::string body = BuildRequestBody(filePath);
    HANDLE h = CreateFileW(jsonPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
    CloseHandle(h);

    AppendStatus(L"QUEUED " + base + L" " + FileNameOf(filePath));

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake = true;
    }
    m_cv.notify_one();
}

void CWhisparrWatched::NotifyDeleted(const CString& filePath)
{
    if (filePath.IsEmpty()) {
        return;
    }
    CString check = filePath;
    if (PathUtils::IsURL(check)) {
        return;
    }

    const Config c = LoadConfig();
    if (!c.enabled || c.url.IsEmpty() || c.apiKey.IsEmpty()) {
        return;
    }

    const CString outbox = GetOutboxDir();
    if (outbox.IsEmpty()) {
        return;
    }

    // The file is already gone, so the identity hash is path-only and does not
    // collide with the "viewed" event (which included size/mtime).
    const uint64_t hash = HashFileIdentity(filePath);
    CString base;
    base.Format(L"%016llx", static_cast<unsigned long long>(hash));
    const CString shardDir = outbox + L"\\" + base.Left(2);
    CreateDirectoryW(shardDir, nullptr);

    const CString jsonPath = shardDir + L"\\" + base + L".json";
    const CString sendingPath = shardDir + L"\\" + base + L".sending";
    const CString failedPath = shardDir + L"\\" + base + L".failed";
    if (GetFileAttributesW(jsonPath) != INVALID_FILE_ATTRIBUTES
        || GetFileAttributesW(sendingPath) != INVALID_FILE_ATTRIBUTES
        || GetFileAttributesW(failedPath) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    const std::string body = BuildDeletedRequestBody(filePath);
    HANDLE h = CreateFileW(jsonPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
    CloseHandle(h);

    AppendStatus(L"QUEUED-DEL " + base + L" " + FileNameOf(filePath));

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wake = true;
    }
    m_cv.notify_one();
}

void CWhisparrWatched::WorkerLoop()
{
    const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_cv.wait_for(lock, std::chrono::seconds(30), [this]() { return m_stop || m_wake; });
        if (m_stop) {
            break;
        }
        m_wake = false;
        lock.unlock();
        ProcessOutbox();
        lock.lock();
    }
    if (SUCCEEDED(hrCom)) {
        ::CoUninitialize();
    }
}

void CWhisparrWatched::ReclaimStale(const CString& dir)
{
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(dir + L"\\*.sending", &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        ULARGE_INTEGER written;
        written.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        written.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        const ULONGLONG tenMinutes = 10ULL * 60ULL * 10000000ULL;
        if (now.QuadPart > written.QuadPart && now.QuadPart - written.QuadPart > tenMinutes) {
            const CString sending = dir + L"\\" + fd.cFileName;
            const CString json = sending.Left(sending.GetLength() - 8) + L".json";
            MoveFileExW(sending, json, MOVEFILE_REPLACE_EXISTING);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

void CWhisparrWatched::ProcessEvent(const Config& cfg, const CString& jsonPath)
{
    const CString base = jsonPath.Left(jsonPath.GetLength() - 5); // strip ".json"
    const CString sendingPath = base + L".sending";
    const CString failedPath = base + L".failed";

    // Atomic claim: only one sender (this or another player) gets the file.
    if (!MoveFileW(jsonPath, sendingPath)) {
        return;
    }
    TouchNow(sendingPath);

    const std::string body = ReadAllBytes(sendingPath);
    const bool isDeleted = body.find("\"_type\"") != std::string::npos;
    const LPCWSTR method = isDeleted ? L"POST" : L"PUT";
    const CString relPath = isDeleted ? L"/api/v3/episodefile/deleted" : L"/api/v3/episode/viewed/file";
    DWORD status = 0;
    DWORD lastError = 0;
    std::string response;
    CString requestInfo;
    const bool sent = !body.empty() && SendEvent(cfg, method, relPath, body, status, response, lastError, requestInfo);

    const CString leaf = base.Mid(base.ReverseFind(L'\\') + 1);
    if (sent && (status == 200 || status == 202)) {
        DeleteFileW(sendingPath);
        DeleteFileW(base + L".failed.response.txt");
        CString line;
        line.Format(isDeleted ? L"DELETED %s status=%lu" : L"SENT %s status=%lu", leaf.GetString(), status);
        AppendStatus(line);
    } else if (sent && (status == 400 || status == 404)) {
        // Permanent for this file name (not in the library, or ambiguous).
        MoveFileExW(sendingPath, failedPath, MOVEFILE_REPLACE_EXISTING);
        WriteFailureInfo(base + L".failed.response.txt", cfg.url, status, response);
        CString line;
        line.Format(L"FAILED %s status=%lu", leaf.GetString(), status);
        AppendStatus(line);
    } else {
        // Network error or server problem: keep for a later retry.
        MoveFileExW(sendingPath, jsonPath, MOVEFILE_REPLACE_EXISTING);
        CString line;
        if (sent) {
            line.Format(L"RETRY %s status=%lu", leaf.GetString(), status);
        } else {
            line.Format(L"RETRY %s network err=%lu %s", leaf.GetString(), lastError, requestInfo.GetString());
        }
        AppendStatus(line);
    }
}

void CWhisparrWatched::ProcessOutbox()
{
    const Config cfg = LoadConfig();
    if (!cfg.enabled || cfg.url.IsEmpty() || cfg.apiKey.IsEmpty()) {
        return;
    }

    const CString outbox = GetOutboxDir();
    if (outbox.IsEmpty()) {
        return;
    }

    std::vector<CString> dirs;
    dirs.push_back(outbox); // legacy flat layout

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(outbox + L"\\*", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }
            const CString name = fd.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            dirs.push_back(outbox + L"\\" + name);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    for (const CString& dir : dirs) {
        ReclaimStale(dir);

        std::vector<CString> events;
        hFind = FindFirstFileW(dir + L"\\*.json", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    events.push_back(dir + L"\\" + fd.cFileName);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        for (const CString& jsonPath : events) {
            ProcessEvent(cfg, jsonPath);
        }
    }
}

bool CWhisparrWatched::SendEvent(const Config& cfg, LPCWSTR method, const CString& relPath, const std::string& body, DWORD& status, std::string& response, DWORD& lastError, CString& requestInfo)
{
    status = 0;
    lastError = 0;
    response.clear();
    requestInfo.Empty();

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t urlPath[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = _countof(urlPath);

    if (!WinHttpCrackUrl(cfg.url, 0, 0, &uc)) {
        return false;
    }

    CString path = urlPath;
    if (path.IsEmpty() || path == L"/") {
        path = relPath;
    } else {
        if (path[path.GetLength() - 1] == L'/') {
            path = path.Left(path.GetLength() - 1);
        }
        path += relPath;
    }

    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS)
                       || (cfg.url.Left(8).CompareNoCase(L"https://") == 0);
    const INTERNET_PORT port = uc.nPort ? uc.nPort
                                        : (https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    requestInfo.Format(L"%s url=%s host=%s port=%u https=%d path=%s",
                       method, cfg.url.GetString(), host, static_cast<unsigned>(port), https ? 1 : 0, path.GetString());

    // NO_PROXY: the server is on the LAN; a user proxy from the VPN contour
    // must not be used here (DEFAULT_PROXY does exactly that).
    HINTERNET hSession = WinHttpOpen(L"MPC-HC/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        lastError = GetLastError();
        return false;
    }
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    HINTERNET hRequest = hConnect ? WinHttpOpenRequest(hConnect, method, path, nullptr,
                                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       https ? WINHTTP_FLAG_SECURE : 0)
                                  : nullptr;

    if (!hConnect || !hRequest) {
        lastError = GetLastError();
    }
    bool ok = false;
    if (hRequest) {
        if (!cfg.verifyTls) {
            DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                        | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                        | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                        | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        }

        CString headers;
        headers.Format(L"X-Api-Key: %s\r\nContent-Type: application/json\r\n", cfg.apiKey.GetString());
        WinHttpAddRequestHeaders(hRequest, headers, static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

        bool sentOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                         static_cast<DWORD>(body.size()), 0);
        if (!sentOk) {
            lastError = GetLastError();
        } else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            lastError = GetLastError();
        } else {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

            // Keep the server's error text so a failed match can be understood.
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0 && response.size() < 65536) {
                char buf[8192];
                DWORD toRead = std::min<DWORD>(available, 8192);
                DWORD read = 0;
                if (!WinHttpReadData(hRequest, buf, toRead, &read) || read == 0) {
                    break;
                }
                response.append(buf, read);
            }
            ok = true;
        }
    }

    if (hRequest) {
        WinHttpCloseHandle(hRequest);
    }
    if (hConnect) {
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}
