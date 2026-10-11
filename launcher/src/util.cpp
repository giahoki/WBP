// SPDX-License-Identifier: GPL-2.0-or-later
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string PathText(const fs::path& path) { return Utf8(path.native()); }
fs::path PathFrom(std::string_view utf8) { return fs::path(Wide(utf8)); }

Bytes ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Failure("cannot read " + PathText(path));
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    file.seekg(0);
    const std::streamoff length = size;
    Bytes data(size_t(length < 0 ? 0 : length));
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        throw Failure("cannot read " + PathText(path));
    return data;
}

std::optional<std::string> ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF)
        text.erase(0, 3);
    return text;
}

void WriteFile(const fs::path& path, const void* data, size_t size) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path part = path;
    part += L".part";
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(static_cast<const char*>(data), std::streamsize(size)))
            throw Failure("cannot write " + PathText(path));
    }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Failure("cannot write " + PathText(path));
}

std::string Trim(std::string_view text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(uint8_t(text[a]))) ++a;
    while (b > a && std::isspace(uint8_t(text[b - 1]))) --b;
    return std::string(text.substr(a, b - a));
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::tolower(uint8_t(c)));
    return out;
}

bool IEquals(std::string_view a, std::string_view b) {
    const std::wstring wa = Wide(a), wb = Wide(b);
    return CompareStringOrdinal(wa.data(), int(wa.size()), wb.data(), int(wb.size()), TRUE) == CSTR_EQUAL;
}

std::vector<std::string> Split(std::string_view text, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t at = text.find(separator, start);
        out.emplace_back(text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return out;
}

bool StartsWith(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }
bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string Replace(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) return text;
    for (size_t at = 0; (at = text.find(from, at)) != std::string::npos; at += to.size())
        text.replace(at, from.size(), to);
    return text;
}

std::string Hex(uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Format(std::string_view pattern, const std::vector<std::string>& args) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{') {
            size_t close = pattern.find('}', i);
            if (close != std::string_view::npos) {
                std::string_view inside = pattern.substr(i + 1, close - i - 1);
                size_t index = next;
                bool ok = inside.empty();
                if (!inside.empty() && std::all_of(inside.begin(), inside.end(), [](char c) { return std::isdigit(uint8_t(c)); })) {
                    index = size_t(std::stoul(std::string(inside)));
                    ok = true;
                }
                if (ok) {
                    if (inside.empty()) ++next;
                    if (index < args.size()) out += args[index];
                    i = close;
                    continue;
                }
            }
        }
        out += pattern[i];
    }
    return out;
}

static void Check(const Bytes& data, size_t at, size_t size) {
    if (at > data.size() || size > data.size() - at)
        throw Failure("out of bounds: " + Hex(at) + "+" + Hex(size) + " / " + Hex(data.size()));
}
uint16_t Le16(const Bytes& d, size_t at) { Check(d, at, 2); uint16_t v; std::memcpy(&v, d.data() + at, 2); return v; }
uint32_t Le32(const Bytes& d, size_t at) { Check(d, at, 4); uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; }
uint64_t Le64(const Bytes& d, size_t at) { Check(d, at, 8); uint64_t v; std::memcpy(&v, d.data() + at, 8); return v; }
void Put16(Bytes& d, size_t at, uint16_t v) { Check(d, at, 2); std::memcpy(d.data() + at, &v, 2); }
void Put32(Bytes& d, size_t at, uint32_t v) { Check(d, at, 4); std::memcpy(d.data() + at, &v, 4); }
void Put64(Bytes& d, size_t at, uint64_t v) { Check(d, at, 8); std::memcpy(d.data() + at, &v, 8); }
void Append64(Bytes& out, uint64_t value) { AppendBytes(out, &value, 8); }
void AppendBytes(Bytes& out, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

static std::mutex say_lock;
static void Emit(DWORD which, std::string_view line) {
    std::lock_guard lock(say_lock);
    HANDLE handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    std::string text(line);
    text += '\n';
    DWORD written = 0;
    WriteFile(handle, text.data(), DWORD(text.size()), &written, nullptr);
}
void Say(std::string_view line) { Emit(STD_OUTPUT_HANDLE, line); }
void SayError(std::string_view line) { Emit(STD_ERROR_HANDLE, line); }

const fs::path& PortDir() {
    static const fs::path dir = [] {
        std::wstring buffer(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        buffer.resize(n);
        fs::path exe(buffer);
        // A development build runs from out/: the port is the repository root.
        fs::path folder = exe.parent_path();
        if (folder.filename() == L"out" && fs::exists(folder.parent_path() / L"patches"))
            folder = folder.parent_path();
        return folder;
    }();
    return dir;
}

const fs::path& DataDir() {
    static const fs::path dir = HasEnv("BB_DATA_DIR") ? PathFrom(Env("BB_DATA_DIR")) : PortDir();
    return dir;
}

fs::path ConfigDir() {
    PWSTR roaming = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) base = roaming;
    CoTaskMemFree(roaming);
    if (base.empty()) base = PortDir();
    return base / L"wbp-launcher";
}

void MigrateOldNames() {
    std::error_code ec;
    const fs::path ini = DataDir() / L"wbp.ini", old_ini = DataDir() / L"bbport.ini";
    if (!fs::exists(ini, ec) && fs::is_regular_file(old_ini, ec)) {
        fs::rename(old_ini, ini, ec);
        if (!ec) Say("Settings: bbport.ini renamed to wbp.ini");
    }
    const fs::path config = ConfigDir(), old_config = config.parent_path() / L"bbport-launcher";
    if (!fs::exists(config, ec) && fs::is_directory(old_config, ec)) fs::rename(old_config, config, ec);
}

std::string Env(const char* name, std::string_view fallback) {
    const std::wstring wname = Wide(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (!n) return std::string(fallback);
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    value.resize(n);
    return Utf8(value);
}

bool HasEnv(const char* name) {
    const std::wstring wname = Wide(name);
    return GetEnvironmentVariableW(wname.c_str(), nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

static std::wstring Upper(std::wstring text) {
    CharUpperBuffW(text.data(), DWORD(text.size()));
    return text;
}

EnvBlock EnvBlock::Current() {
    EnvBlock block;
    wchar_t* strings = GetEnvironmentStringsW();
    for (const wchar_t* p = strings; *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        size_t eq = entry.find(L'=', 1);  // "=C:=C:\..." entries start with '='
        if (eq == std::wstring::npos) continue;
        block.values[Upper(entry.substr(0, eq))] = entry;
    }
    FreeEnvironmentStringsW(strings);
    return block;
}

void EnvBlock::Set(std::string_view name, std::string_view value) {
    const std::wstring wname = Wide(name);
    values[Upper(wname)] = wname + L"=" + Wide(value);
}

void EnvBlock::Unset(std::string_view name) { values.erase(Upper(Wide(name))); }

std::optional<std::string> EnvBlock::Get(std::string_view name) const {
    auto it = values.find(Upper(Wide(name)));
    if (it == values.end()) return std::nullopt;
    return Utf8(std::wstring_view(it->second).substr(it->second.find(L'=', 1) + 1));
}

std::wstring EnvBlock::Build() const {
    std::wstring out;
    for (const auto& [key, entry] : values) {
        out += entry;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

std::wstring QuoteArg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(arg);
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

static void Hash(LPCWSTR algorithm, const void* data, size_t size, uint8_t* out, ULONG out_size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) != 0) throw Failure("hash provider unavailable");
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), 0);
    BCryptFinishHash(hash, out, out_size, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
}

std::string Sha256Hex(const void* data, size_t size) {
    uint8_t digest[32];
    Hash(BCRYPT_SHA256_ALGORITHM, data, size, digest, 32);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}

void Sha1(const void* data, size_t size, uint8_t out[20]) { Hash(BCRYPT_SHA1_ALGORITHM, data, size, out, 20); }

std::string Base64(const uint8_t* data, size_t size) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < size) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < size) v |= data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < size ? table[(v >> 6) & 63] : '=';
        out += i + 2 < size ? table[v & 63] : '=';
    }
    return out;
}
