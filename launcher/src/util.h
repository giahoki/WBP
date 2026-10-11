// SPDX-License-Identifier: GPL-2.0-or-later
// Small helpers shared by the launcher: UTF-8/UTF-16 strings, files, paths, logging.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// Thrown by the preparation steps; the message is shown to the player.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::wstring Wide(std::string_view text);
std::string Utf8(std::wstring_view text);
std::string PathText(const fs::path& path);          // UTF-8, native separators
fs::path PathFrom(std::string_view utf8);

Bytes ReadFile(const fs::path& path);                 // throws Failure
std::optional<std::string> ReadText(const fs::path& path);
void WriteFile(const fs::path& path, const void* data, size_t size); // via a .part file, throws
inline void WriteFile(const fs::path& path, const Bytes& data) { WriteFile(path, data.data(), data.size()); }
inline void WriteText(const fs::path& path, std::string_view text) { WriteFile(path, text.data(), text.size()); }

std::string Trim(std::string_view text);
std::string Lower(std::string_view text);             // ASCII
bool IEquals(std::string_view a, std::string_view b); // case-insensitive, Unicode (Windows)
std::vector<std::string> Split(std::string_view text, char separator);
bool StartsWith(std::string_view text, std::string_view prefix);
bool EndsWith(std::string_view text, std::string_view suffix);
std::string Replace(std::string text, std::string_view from, std::string_view to);
std::string Hex(uint64_t value);
std::string Format(std::string_view pattern, const std::vector<std::string>& args); // {} and {0} {1}
template <typename... T>
std::string Fmt(std::string_view pattern, const T&... args) {
    return Format(pattern, {std::string(args)...});
}

// Little-endian reads with bounds checks (throw Failure).
uint16_t Le16(const Bytes& data, size_t at);
uint32_t Le32(const Bytes& data, size_t at);
uint64_t Le64(const Bytes& data, size_t at);
void Put16(Bytes& data, size_t at, uint16_t value);
void Put32(Bytes& data, size_t at, uint32_t value);
void Put64(Bytes& data, size_t at, uint64_t value);
void Append64(Bytes& out, uint64_t value);
void AppendBytes(Bytes& out, const void* data, size_t size);

// Output of the launcher's roles (--run, --play): the inherited stdout, else nothing. Line
// buffered; the game's log in the launcher reads it through a pipe.
void Say(std::string_view line);
void SayError(std::string_view line);

// The folder of Bloodborne.exe (the port), the data folder (BB_DATA_DIR or the port) and the
// launcher's own settings folder (%APPDATA%/wbp-launcher).
const fs::path& PortDir();
const fs::path& DataDir();
fs::path ConfigDir();
// Before the rename to WBP the settings were bbport.ini and %APPDATA%/bbport-launcher: moves
// them to the new names once, when only the old ones exist.
void MigrateOldNames();
std::string Env(const char* name, std::string_view fallback = {});
bool HasEnv(const char* name);

// Environment block for child processes: name -> value (case-insensitive names on Windows).
struct EnvBlock {
    std::map<std::wstring, std::wstring> values;  // upper-case key -> "Name=Value"
    static EnvBlock Current();
    void Set(std::string_view name, std::string_view value);
    void Unset(std::string_view name);
    std::optional<std::string> Get(std::string_view name) const;
    std::wstring Build() const;  // double-NUL terminated, sorted
};

std::wstring QuoteArg(std::wstring_view arg);
std::string Sha256Hex(const void* data, size_t size);
void Sha1(const void* data, size_t size, uint8_t out[20]);
std::string Base64(const uint8_t* data, size_t size);
