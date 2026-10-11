// SPDX-License-Identifier: GPL-2.0-or-later
#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <fstream>

namespace net {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    operator HINTERNET() const { return h; }
};

std::string LastError(const char* what) {
    DWORD code = GetLastError();
    const char* reason = code == ERROR_WINHTTP_TIMEOUT                ? "timed out"
                         : code == ERROR_WINHTTP_NAME_NOT_RESOLVED    ? "name not resolved"
                         : code == ERROR_WINHTTP_CANNOT_CONNECT       ? "cannot connect"
                         : code == ERROR_WINHTTP_SECURE_FAILURE       ? "TLS failure"
                         : code == ERROR_WINHTTP_CONNECTION_ERROR     ? "connection reset"
                         : code == ERROR_WINHTTP_UNRECOGNIZED_SCHEME  ? "bad address"
                         : code == ERROR_WINHTTP_INVALID_URL          ? "bad address"
                                                                      : nullptr;
    return reason ? std::string(reason) : std::string(what) + " failed (" + std::to_string(code) + ")";
}

struct Open {
    Handle session, connection, request;
    std::string error;
};

bool Start(Open& o, const std::string& method, const std::string& url, const std::string& body, int timeout_ms,
           const std::string& accept) {
    std::wstring wurl = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host, parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path, parts.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra, parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        o.error = "bad address " + url;
        return false;
    }
    o.session.h = WinHttpOpen(L"wbp-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!o.session) {
        o.error = LastError("WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(o.session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(o.session, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    o.connection.h = WinHttpConnect(o.session, host, parts.nPort, 0);
    if (!o.connection) {
        o.error = LastError("connect");
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    o.request.h = WinHttpOpenRequest(o.connection, Wide(method).c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!o.request) {
        o.error = LastError("request");
        return false;
    }
    std::wstring headers;
    if (!accept.empty()) headers = L"Accept: " + Wide(accept) + L"\r\n";
    if (!WinHttpSendRequest(o.request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                            DWORD(body.size()), DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(o.request, nullptr)) {
        o.error = LastError("request");
        return false;
    }
    return true;
}

int Status(HINTERNET request) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return int(status);
}

uint64_t Length(HINTERNET request) {
    wchar_t buffer[64] = {};
    DWORD size = sizeof(buffer);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, buffer, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return 0;
    return _wcstoui64(buffer, nullptr, 10);
}

}  // namespace

Reply Request(const std::string& method, const std::string& url, const std::string& body, int timeout_ms, size_t limit,
              const std::string& accept) {
    Reply reply;
    Open o;
    if (!Start(o, method, url, body, timeout_ms, accept)) {
        reply.error = o.error;
        return reply;
    }
    reply.status = Status(o.request);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(o.request, &available) || !available) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(o.request, chunk.data(), available, &read) || !read) break;
        reply.body.append(chunk.data(), read);
        if (limit && reply.body.size() >= limit) break;
    }
    return reply;
}

void Download(const std::string& url, const fs::path& path, const std::function<void(uint64_t, uint64_t)>& progress,
              int timeout_ms) {
    Open o;
    if (!Start(o, "GET", url, {}, timeout_ms, {})) throw Failure(o.error);
    const int status = Status(o.request);
    if (status != 200) throw Failure("HTTP " + std::to_string(status) + " for " + url);
    const uint64_t total = Length(o.request);
    fs::path part = path;
    part += L".part";
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file) throw Failure("cannot write " + PathText(part));
        uint64_t done = 0;
        std::vector<char> buffer(1 << 20);
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(o.request, buffer.data(), DWORD(buffer.size()), &read)) throw Failure(LastError("download"));
            if (!read) break;
            file.write(buffer.data(), read);
            done += read;
            if (progress) progress(done, total);
        }
        if (!file) throw Failure("cannot write " + PathText(part));
    }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) throw Failure("cannot write " + PathText(path));
}

std::string TcpCheck(const std::string& host, int port, int timeout_ms) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    addrinfo hints{}, *result = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) return "name not resolved";
    std::string error = "cannot connect";
    for (addrinfo* a = result; a; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        connect(s, a->ai_addr, int(a->ai_addrlen));
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int n = select(0, nullptr, &writable, &failed, &tv);
        closesocket(s);
        if (n > 0 && FD_ISSET(s, &writable)) {
            error.clear();
            break;
        }
        error = n == 0 ? "timed out" : "connection refused";
    }
    freeaddrinfo(result);
    return error;
}

}  // namespace net
