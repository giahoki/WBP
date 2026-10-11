// SPDX-License-Identifier: GPL-2.0-or-later
// HTTP(S) through WinHTTP and a plain TCP connection test.
#pragma once
#include <functional>
#include <string>

#include "util.h"

namespace net {

struct Reply {
    int status = 0;        // 0: no answer (then `error` says why)
    std::string body, error;
    bool Ok() const { return status >= 200 && status < 300; }
};

// method GET or POST; `limit` stops reading after that many bytes (0: everything).
Reply Request(const std::string& method, const std::string& url, const std::string& body = {},
              int timeout_ms = 15000, size_t limit = 0, const std::string& accept = {});
inline Reply Get(const std::string& url, int timeout_ms = 15000, size_t limit = 0) {
    return Request("GET", url, {}, timeout_ms, limit);
}

// Downloads `url` into `path` (through a .part file); progress(done, total) may be called often.
// Throws Failure.
void Download(const std::string& url, const fs::path& path, const std::function<void(uint64_t, uint64_t)>& progress = {},
              int timeout_ms = 60000);

// "" when host:port accepts a TCP connection, else why not.
std::string TcpCheck(const std::string& host, int port, int timeout_ms);

}  // namespace net
