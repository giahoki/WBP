// SPDX-License-Identifier: GPL-2.0-or-later
// Successor of scripts/prepare.py and scripts/link_modules.py: the same checks and the same
// BBPROBE5 output. All offsets are checked before use (Le*, Span throw Failure).
#include "image.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <libdeflate.h>

namespace prep {
namespace {

constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;
constexpr uint32_t PT_SCE_DYNLIBDATA = 0x61000000, PT_SCE_PROCPARAM = 0x61000001,
                   PT_SCE_RELRO = 0x61000010;
constexpr uint64_t MAX_IMAGE = 512ull * 1024 * 1024;
constexpr uint64_t IMAGE_VERSION = 1;  // bump when the output changes: invalidates the cache

struct Ph {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Elf {
    Bytes elf;
    uint64_t entry = 0;
    std::vector<Ph> ph;
};

Bytes Span(const Bytes& data, uint64_t offset, uint64_t size) {
    if (offset > data.size() || size > data.size() - offset)
        throw Failure("out of bounds: " + Hex(offset) + "+" + Hex(size) + " / " + Hex(data.size()));
    return Bytes(data.begin() + ptrdiff_t(offset), data.begin() + ptrdiff_t(offset + size));
}

bool IsLoad(const Ph& p) { return p.type == PT_LOAD || p.type == PT_SCE_RELRO; }

Elf ParseSelf(const Bytes& data) {
    static const uint8_t magic[4] = {'O', 0x15, '=', 0x1d};
    if (data.size() < 32 || std::memcmp(data.data(), magic, 4))
        throw Failure("expected PS4 SELF");
    const uint32_t count = Le16(data, 24);
    const size_t base = 32 + size_t(count) * 32;
    static const uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    Bytes head = Span(data, base, 64);
    if (std::memcmp(head.data(), ident, 7) || Le16(head, 18) != 62)
        throw Failure("expected little-endian x86-64 ELF");
    const uint64_t phoff = Le64(head, 32);
    const uint32_t phentsize = Le16(head, 54), phnum = Le16(head, 56);
    if (phentsize != 56 || !(phnum > 0 && phnum < 256)) throw Failure("unsupported program headers");
    Elf result;
    result.entry = Le64(head, 24);
    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t at = base + phoff + i * 56;
        Ph p{Le32(data, at), Le32(data, at + 4), Le64(data, at + 8), Le64(data, at + 16),
             Le64(data, at + 24), Le64(data, at + 32), Le64(data, at + 40), Le64(data, at + 48)};
        result.ph.push_back(p);
    }
    uint64_t end = 0;
    for (const Ph& p : result.ph) end = std::max(end, p.offset + p.filesz);
    if (end > MAX_IMAGE) throw Failure("probe image exceeds 512 MiB limit");
    result.elf.assign(size_t(end), 0);
    const uint64_t header_end = phoff + uint64_t(phnum) * 56;
    Bytes header = Span(data, base, header_end);
    if (header_end > result.elf.size()) result.elf.resize(size_t(header_end));
    std::memcpy(result.elf.data(), header.data(), header.size());
    std::vector<std::pair<uint64_t, uint64_t>> covered{{0, header_end}};
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 32 + size_t(i) * 32;
        const uint64_t flags = Le64(data, at), off = Le64(data, at + 8), size = Le64(data, at + 16),
                       mem = Le64(data, at + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) throw Failure("encrypted/compressed SELF segment is unsupported");
        const uint64_t index = (flags >> 20) & 4095;
        if (index >= result.ph.size()) throw Failure("invalid segment index");
        const Ph& p = result.ph[index];
        if (size != p.filesz || size != mem) throw Failure("unsupported blocked segment layout");
        Bytes chunk = Span(data, off, size);
        if (p.offset + size > result.elf.size()) throw Failure("segment outside the ELF");
        std::memcpy(result.elf.data() + p.offset, chunk.data(), chunk.size());
        covered.push_back({p.offset, p.offset + size});
    }
    for (size_t i = 0; i < result.ph.size(); ++i) {
        const Ph& p = result.ph[i];
        if (!p.filesz) continue;
        bool inside = std::any_of(covered.begin(), covered.end(), [&](auto& c) {
            return c.first <= p.offset && p.offset + p.filesz <= c.second;
        });
        if (!inside && (p.type == PT_LOAD || p.type == PT_DYNAMIC || p.type == PT_SCE_DYNLIBDATA ||
                        p.type == PT_SCE_RELRO))
            throw Failure("required segment " + std::to_string(i) + " is unavailable");
    }
    return result;
}

const Ph& FindPh(const Elf& e, uint32_t type) {
    for (const Ph& p : e.ph)
        if (p.type == type) return p;
    throw Failure("missing program header " + Hex(type));
}
const Ph* MaybePh(const Elf& e, uint32_t type) {
    for (const Ph& p : e.ph)
        if (p.type == type) return &p;
    return nullptr;
}

std::string Nid(const std::string& name) {
    static const uint8_t salt[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
                                     0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    Bytes input(name.begin(), name.end());
    input.insert(input.end(), salt, salt + 16);
    uint8_t digest[20];
    Sha1(input.data(), input.size(), digest);
    uint8_t first[8];
    for (int i = 0; i < 8; ++i) first[i] = digest[7 - i];
    std::string text = Base64(first, 8);
    while (!text.empty() && text.back() == '=') text.pop_back();
    return Replace(text, "/", "-");
}

std::string EncodeId(uint64_t value) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result(1, alphabet[value & 63]);
    value >>= 6;
    while (value) {
        result.insert(result.begin(), alphabet[value & 63]);
        value >>= 6;
    }
    return result;
}

struct Symbol {
    std::string name;
    uint32_t type, binding, section;
    uint64_t value, size;
    std::string identity;  // "nid|library|version|module|version", empty when not linkable
    std::string nid, library;
};

struct Reloc {
    uint64_t target;
    uint32_t kind;
    uint32_t symbol;
    int64_t addend;
};

struct Module {
    Elf elf;
    std::map<uint64_t, uint64_t> tags;  // until DT_NULL
    std::vector<std::pair<uint64_t, uint64_t>> dynamic;
    std::vector<Symbol> symbols;
    std::vector<Reloc> relocs;
    std::map<std::string, std::string> libraries, modules;  // key -> "name|version"
    std::string Identity(const std::string& name, std::string* nid = nullptr, std::string* library = nullptr) const {
        auto parts = Split(name, '#');
        if (parts.size() != 3) return {};
        auto lib = libraries.find(parts[1]);
        auto mod = modules.find(parts[2]);
        if (lib == libraries.end() || mod == modules.end()) throw Failure("unknown library or module id in " + name);
        if (nid) *nid = parts[0];
        if (library) *library = Split(lib->second, '|')[0];
        return parts[0] + "|" + lib->second + "|" + mod->second;
    }
};

uint64_t Tag(const std::map<uint64_t, uint64_t>& tags, uint64_t tag) {
    auto it = tags.find(tag);
    if (it == tags.end()) throw Failure("missing dynamic tag " + Hex(tag));
    return it->second;
}

std::string CString(const Bytes& strings, uint64_t offset) {
    if (offset >= strings.size()) throw Failure("string outside the string table");
    const char* start = reinterpret_cast<const char*>(strings.data() + offset);
    const void* end = std::memchr(start, 0, strings.size() - offset);
    if (!end) throw Failure("unterminated string");
    return std::string(start, static_cast<const char*>(end));
}

Module LoadModule(const fs::path& path) {
    Module m;
    m.elf = ParseSelf(ReadFile(path));
    const Ph& dp = FindPh(m.elf, PT_DYNAMIC);
    for (uint64_t pos = dp.offset; pos < dp.offset + dp.filesz; pos += 16) {
        const uint64_t tag = Le64(m.elf.elf, pos), value = Le64(m.elf.elf, pos + 8);
        if (!tag) break;
        m.dynamic.push_back({tag, value});
        m.tags[tag] = value;
    }
    const Ph& lp = FindPh(m.elf, PT_SCE_DYNLIBDATA);
    const Bytes blob = Span(m.elf.elf, lp.offset, lp.filesz);
    const Bytes strings = Span(blob, Tag(m.tags, 0x61000035), Tag(m.tags, 0x61000037));
    for (auto [tag, value] : m.dynamic) {
        if (tag != 0x61000013 && tag != 0x61000015 && tag != 0x6100000d && tag != 0x6100000f) continue;
        auto& table = (tag == 0x61000013 || tag == 0x61000015) ? m.libraries : m.modules;
        const std::string key = EncodeId(value >> 48);
        const std::string identity = CString(strings, value & 0xffffffff) + "|" + std::to_string((value >> 32) & 65535);
        auto it = table.find(key);
        if (it != table.end() && it->second != identity) throw Failure("conflicting module/library IDs");
        table[key] = identity;
    }
    const Bytes table = Span(blob, Tag(m.tags, 0x61000039), Tag(m.tags, 0x6100003f));
    if (table.size() % 24) throw Failure("bad symbol table size");
    for (size_t pos = 0; pos < table.size(); pos += 24) {
        Symbol s;
        s.name = CString(strings, Le32(table, pos));
        const uint8_t info = table[pos + 4];
        s.type = info & 15;
        s.binding = info >> 4;
        s.section = Le16(table, pos + 6);
        s.value = Le64(table, pos + 8);
        s.size = Le64(table, pos + 16);
        if (!s.name.empty()) s.identity = m.Identity(s.name, &s.nid, &s.library);
        m.symbols.push_back(std::move(s));
    }
    for (auto [ot, st] : {std::pair{0x61000029ull, 0x6100002dull}, std::pair{0x6100002full, 0x61000031ull}}) {
        const Bytes relocations = Span(blob, Tag(m.tags, ot), Tag(m.tags, st));
        if (relocations.size() % 24) throw Failure("bad relocation table size");
        for (size_t pos = 0; pos < relocations.size(); pos += 24) {
            const uint64_t info = Le64(relocations, pos + 8);
            m.relocs.push_back({Le64(relocations, pos), uint32_t(info & 0xffffffff), uint32_t(info >> 32),
                                int64_t(Le64(relocations, pos + 16))});
        }
    }
    return m;
}

// prepare.py: proves that this dump's _init_env is exactly RET; never assumes it.
bool InitEnvIsRet(const Module& libc) {
    const std::string wanted = Nid("_init_env");
    for (const Symbol& s : libc.symbols) {
        if (Split(s.name, '#')[0] != wanted || !s.section || s.type != 2) continue;
        for (const Ph& p : libc.elf.ph) {
            if (p.type == PT_LOAD && (p.flags & 1) && p.vaddr <= s.value && s.value < p.vaddr + p.filesz) {
                Bytes code = Span(libc.elf.elf, p.offset + s.value - p.vaddr, s.size);
                return code.size() == 1 && code[0] == 0xc3;
            }
        }
        throw Failure("_init_env outside the executable segments");
    }
    return false;
}

size_t PatchFsLoads(Bytes& image, const std::vector<Ph>& ph, uint64_t base) {
    static const uint8_t fs_load[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};  // mov rax, fs:[0]
    size_t patched = 0;
    for (const Ph& p : ph) {
        if (p.type != PT_LOAD || !(p.flags & 1)) continue;
        const uint64_t start = base + p.vaddr, end = std::min<uint64_t>(base + p.vaddr + p.filesz, image.size());
        for (uint64_t at = start; at + 9 <= end;) {
            auto found = std::search(image.begin() + ptrdiff_t(at), image.begin() + ptrdiff_t(end),
                                     fs_load, fs_load + 9);
            if (found == image.begin() + ptrdiff_t(end)) break;
            *found = 0x65;  // gs:
            ++patched;
            at = uint64_t(found - image.begin()) + 9;
        }
    }
    return patched;
}

struct OutReloc {
    uint64_t target, kind;
    int64_t value, addend;
};

std::string CacheKey(const fs::path& game) {
    std::string key = "v" + std::to_string(IMAGE_VERSION);
    for (const fs::path& file : {game / L"eboot.bin", game / L"sce_module" / L"libc.prx",
                                 game / L"sce_module" / L"libSceFios2.prx"}) {
        std::error_code ec;
        auto size = fs::file_size(file, ec);
        auto time = fs::last_write_time(file, ec).time_since_epoch().count();
        key += "|" + PathText(fs::weakly_canonical(file, ec)) + "|" + std::to_string((unsigned long long)size) + "|" + std::to_string((long long)time);
    }
    return key;
}

}  // namespace

std::map<std::string, std::string> ReadSfo(const fs::path& path) {
    const Bytes data = ReadFile(path);
    if (data.size() < 20 || std::memcmp(data.data(), "\0PSF", 4)) throw Failure("bad SFO signature");
    const uint32_t keys = Le32(data, 8), values = Le32(data, 12), count = Le32(data, 16);
    std::map<std::string, std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 20 + size_t(i) * 16;
        const uint32_t key = Le16(data, at), fmt = Le16(data, at + 2), size = Le32(data, at + 4),
                       off = Le32(data, at + 12);
        std::string name = CString(data, uint64_t(keys) + key);
        Bytes value = Span(data, uint64_t(values) + off, size);
        if (fmt == 0x404) {
            result[name] = std::to_string(Le32(value, 0));
        } else {
            std::string text(value.begin(), value.end());
            while (!text.empty() && text.back() == '\0') text.pop_back();
            result[name] = text;
        }
    }
    return result;
}

std::string GameVersion(const fs::path& game) {
    try {
        auto sfo = ReadSfo(game / L"sce_sys" / L"param.sfo");
        auto it = sfo.find("APP_VER");
        return it == sfo.end() ? std::string() : it->second;
    } catch (const Failure&) {
        return {};
    }
}

namespace {
// The SHA-256 of the 1.09 eboot.bin's loaded image (its loadable segments at their addresses):
// the same whatever tool dumped it (the SELF headers around it differ).
constexpr const char* SUPPORTED_IMAGE = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
// The 1.09 executable with a 60 FPS patch applied to it, as some dumps ship it: one of its edits
// inverts a branch in the menu code (+0xf6d90d) and the gestures menu crashes the game.
constexpr const char* PATCHED_60FPS_IMAGE = "bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f";

std::string ImageSha256(const fs::path& game) {
    const Elf e = ParseSelf(ReadFile(game / L"eboot.bin"));
    uint64_t size = 0;
    for (const Ph& p : e.ph)
        if (IsLoad(p)) size = std::max(size, p.vaddr + p.memsz);
    if (size > MAX_IMAGE) throw Failure("eboot image too large");
    Bytes image(size_t(size), 0);
    for (const Ph& p : e.ph) {
        if (!IsLoad(p)) continue;
        const Bytes part = Span(e.elf, p.offset, p.filesz);
        if (p.vaddr + part.size() > image.size()) throw Failure("segment outside the image");
        std::memcpy(image.data() + p.vaddr, part.data(), part.size());
    }
    return Sha256Hex(image.data(), image.size());
}

// True when a DFLT .dcx file does not unpack to its stated size or fails zlib's checksum (other
// compressions are not checked). An extraction tool that leaves parts of compressed sectors
// stale damages most files, the shaders the game loads first among them.
bool DcxBroken(const fs::path& path) {
    Bytes data;
    try {
        data = ReadFile(path);
    } catch (const Failure&) {
        return true;
    }
    if (data.size() < 0x4C || std::memcmp(data.data(), "DCX\0", 4) || std::memcmp(data.data() + 0x18, "DCS\0", 4) ||
        std::memcmp(data.data() + 0x28, "DFLT", 4))
        return false;
    const auto be32 = [&](size_t at) {
        return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 | data[at + 3];
    };
    const uint32_t size = be32(0x1C), packed = be32(0x20);
    if (packed > data.size() - 0x4C) return true;
    Bytes out(size_t(size) + 1);
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    const auto result = libdeflate_zlib_decompress(d, data.data() + 0x4C, packed, out.data(), out.size(), &actual);
    libdeflate_free_decompressor(d);
    return result != LIBDEFLATE_SUCCESS || actual != size;
}
}  // namespace

std::string GameProblem(const fs::path& game) {
    if (HasEnv("BB_SKIP_GAME_CHECK") && Env("BB_SKIP_GAME_CHECK") == "1") return {};
    std::string hash;
    try {
        hash = ImageSha256(game);
    } catch (const Failure&) {
        return {};  // unreadable: the image build reports it
    }
    if (hash == PATCHED_60FPS_IMAGE) return "patched_eboot";
    if (hash != SUPPORTED_IMAGE) return {};
    std::error_code ec;
    const fs::path shaders = game / L"dvdroot_ps4" / L"shader";
    for (fs::recursive_directory_iterator it(shaders, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == L".dcx" && DcxBroken(it->path()))
            return "damaged_files";
    }
    return {};
}

void BuildImage(const fs::path& game, const fs::path& out) {
    const fs::path target = out / L"boot-linked.bin", key_file = out / L"boot-linked.key";
    const std::string key = CacheKey(game);
    if (fs::exists(target) && ReadText(key_file) == key) {
        Say("Image: game unchanged, using " + PathText(target));
        return;
    }
    std::error_code ec;
    fs::remove(key_file, ec);

    // ---- prepare.py: the eboot's image, relocations and imports --------------------------
    const Module main = LoadModule(game / L"eboot.bin");
    std::vector<Ph> loads;
    for (const Ph& p : main.elf.ph)
        if (IsLoad(p)) loads.push_back(p);
    auto mapped = [](const std::vector<Ph>& ranges, uint64_t addr, uint64_t size = 8) {
        return std::any_of(ranges.begin(), ranges.end(),
                           [&](const Ph& p) { return p.vaddr <= addr && addr + size <= p.vaddr + p.memsz; });
    };
    uint64_t size = 0;
    for (const Ph& p : loads) size = std::max(size, p.vaddr + p.memsz);
    if (size > MAX_IMAGE || !mapped(loads, main.elf.entry, 1)) throw Failure("invalid memory image");
    Bytes image(size_t(size), 0);
    for (const Ph& p : loads) {
        if (p.filesz > p.memsz) throw Failure("segment file size exceeds memory size");
        Bytes chunk = Span(main.elf.elf, p.offset, p.filesz);
        std::memcpy(image.data() + p.vaddr, chunk.data(), chunk.size());
    }
    std::vector<OutReloc> relocs;
    std::vector<std::string> names;
    std::map<uint32_t, uint64_t> imports;  // symbol index -> import index
    std::vector<uint32_t> import_order;
    size_t relocation_count = 0;
    for (const Reloc& r : main.relocs) {
        ++relocation_count;
        if (!mapped(loads, r.target)) throw Failure("relocation outside mapped segments: " + Hex(r.target));
        if (r.kind == 8) {
            relocs.push_back({r.target, 0, r.addend, 0});
        } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
            if (r.symbol >= main.symbols.size()) throw Failure("bad symbol index");
            const Symbol& s = main.symbols[r.symbol];
            if (s.section) {
                if (s.section == 0xfff1) Put64(image, r.target, s.value + uint64_t(r.addend));
                else relocs.push_back({r.target, 0, int64_t(s.value) + r.addend, 0});
            } else {
                if (!imports.count(r.symbol)) {
                    imports[r.symbol] = imports.size();
                    import_order.push_back(r.symbol);
                }
                const uint64_t kind = s.type == 1 ? 2 : 1;
                if ((kind == 1 && r.addend) || !(0 <= r.addend && r.addend < 4096))
                    throw Failure("unsupported import addend");
                relocs.push_back({r.target, kind, int64_t(imports[r.symbol]), r.addend});
            }
        } else {
            throw Failure("unsupported relocation " + std::to_string(r.kind));
        }
    }
    for (uint32_t index : import_order) names.push_back(main.symbols[index].name);
    std::vector<std::array<uint64_t, 3>> segments;
    for (const Ph& p : loads) segments.push_back({p.vaddr, p.memsz, p.flags});
    const Module libc_for_check = LoadModule(game / L"sce_module" / L"libc.prx");
    const uint64_t flags = InitEnvIsRet(libc_for_check) ? 1 : 0;
    try {
        auto sfo = ReadSfo(game / L"sce_sys" / L"param.sfo");
        Say(sfo["TITLE"] + " | entry=" + Hex(main.elf.entry) + " | image=" + std::to_string(size) + " bytes");
    } catch (const Failure&) {
    }
    Say(std::to_string(names.size()) + " imported symbols; " + std::to_string(relocation_count) + " relocations");
    Say(std::string("libc _init_env verified RET: ") + (flags ? "True" : "False"));

    // ---- link_modules.py: libc.prx and libSceFios2.prx after the eboot ---------------------
    std::vector<std::pair<std::string, uint64_t>> identities;  // identity -> name index, in order
    std::map<std::string, uint64_t> identity_index;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string id = main.Identity(names[i]);
        if (!identity_index.count(id)) {
            identity_index[id] = i;
            identities.push_back({id, i});
        }
    }
    std::map<std::string, std::string> main_libraries, main_modules;  // "name|version" -> key
    for (auto& [k, id] : main.libraries) main_libraries[id] = k;
    for (auto& [k, id] : main.modules) main_modules[id] = k;
    auto imported = [&](const Symbol& s) -> uint64_t {
        if (s.identity.empty()) throw Failure("unlinkable import " + s.name);
        auto it = identity_index.find(s.identity);
        if (it != identity_index.end()) return it->second;
        const auto parts = Split(s.identity, '|');  // nid, lib, ver, mod, ver
        const std::string lib = parts[1] + "|" + parts[2], mod = parts[3] + "|" + parts[4];
        std::string name = (main_libraries.count(lib) && main_modules.count(mod))
                               ? parts[0] + "#" + main_libraries[lib] + "#" + main_modules[mod]
                               : parts[0] + "#" + parts[1];
        const uint64_t index = names.size();
        names.push_back(name);
        identity_index[s.identity] = index;
        identities.push_back({s.identity, index});
        return index;
    };

    struct Export {
        uint64_t address, kind;
    };
    std::map<std::string, Export> exports;
    std::map<std::string, std::vector<Export>> by_nid;  // "nid|library name"
    struct Entry {
        std::string file;
        uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module;
    };
    std::vector<Entry> table;
    uint64_t base = (size + 65535) & ~uint64_t(65535);
    size_t fs_patched = PatchFsLoads(image, main.elf.ph, 0);
    uint64_t tls_module = 2;
    for (const char* filename : {"libc.prx", "libSceFios2.prx"}) {
        const Module m = LoadModule(game / L"sce_module" / Wide(filename));
        std::vector<Ph> mloads;
        for (const Ph& p : m.elf.ph)
            if (IsLoad(p)) mloads.push_back(p);
        uint64_t modsize = 0;
        for (const Ph& p : mloads) modsize = std::max(modsize, p.vaddr + p.memsz);
        if (base + modsize > MAX_IMAGE) throw Failure("linked image exceeds probe limit");
        image.resize(size_t(base + modsize), 0);
        for (const Ph& p : mloads) {
            if (p.filesz > p.memsz) throw Failure(std::string(filename) + ": invalid segment size");
            Bytes chunk = Span(m.elf.elf, p.offset, p.filesz);
            std::memcpy(image.data() + base + p.vaddr, chunk.data(), chunk.size());
            segments.push_back({base + p.vaddr, p.memsz, p.flags});
        }
        const Ph* tls = MaybePh(m.elf, PT_TLS);
        uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(mloads, tls->vaddr, tls->memsz))
                throw Failure(std::string(filename) + ": unsupported TLS layout");
            module_id = tls_module++;
        }
        for (const Reloc& r : m.relocs) {
            if (!mapped(mloads, r.target)) throw Failure(std::string(filename) + ": relocation outside image " + Hex(r.target));
            const uint64_t target = r.target + base;
            if (r.kind == 8) {
                relocs.push_back({target, 0, int64_t(base) + r.addend, 0});
            } else if (r.kind == 16) {
                const Symbol& s = m.symbols.at(r.symbol);
                if (!s.name.empty() || s.type != 3 || r.addend || !module_id)
                    throw Failure(std::string(filename) + ": unsupported external TLS relocation");
                Put64(image, target, module_id);
            } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                const Symbol& s = m.symbols.at(r.symbol);
                if (s.section) {
                    if (s.section == 0xfff1) Put64(image, target, s.value + uint64_t(r.addend));
                    else relocs.push_back({target, 0, int64_t(base + s.value) + r.addend, 0});
                } else if (s.identity.empty() && s.binding == 2) {
                    Put64(image, target, 0);  // weak undefined (module_start/stop)
                } else {
                    const uint64_t kind = s.type == 1 ? 2 : 1;
                    if ((kind == 1 && r.addend) || !(0 <= r.addend && r.addend < 4096))
                        throw Failure(std::string(filename) + ": unsupported import addend");
                    relocs.push_back({target, kind, int64_t(imported(s)), r.addend});
                }
            } else {
                throw Failure(std::string(filename) + ": unsupported relocation type " + std::to_string(r.kind));
            }
        }
        for (const Symbol& s : m.symbols) {
            if (!s.section || (s.binding != 1 && s.binding != 2) || s.identity.empty() || (s.type != 1 && s.type != 2))
                continue;
            if (!mapped(mloads, s.value, std::max<uint64_t>(1, s.size)))
                throw Failure(std::string(filename) + ": export outside load segments");
            if (exports.count(s.identity)) throw Failure(std::string(filename) + ": ambiguous export");
            Export e{base + s.value, s.type == 1 ? 2u : 1u};
            exports[s.identity] = e;
            by_nid[s.nid + "|" + s.library].push_back(e);
        }
        fs_patched += PatchFsLoads(image, m.elf.ph, base);
        auto init = m.tags.find(12);
        table.push_back({filename, base, modsize, base + (init == m.tags.end() ? 0 : init->second),
                         tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0, tls ? tls->filesz : 0, module_id});
        base = (base + modsize + 65535) & ~uint64_t(65535);
    }

    std::vector<std::array<uint64_t, 3>> bindings;
    size_t unresolved = 0;
    for (auto& [id, index] : identities) {
        const Export* found = nullptr;
        auto it = exports.find(id);
        if (it != exports.end()) {
            found = &it->second;
        } else if (!id.empty()) {
            auto parts = Split(id, '|');
            if (parts[1] == "libSceLibcInternal") {
                auto candidates = by_nid.find(parts[0] + "|libc");
                if (candidates != by_nid.end() && candidates->second.size() == 1) found = &candidates->second[0];
            }
        }
        if (found) bindings.push_back({index, found->address, found->kind});
        else ++unresolved;
    }

    uint64_t main_tls[4] = {0, 0, 0, 0};
    if (const Ph* t = MaybePh(main.elf, PT_TLS)) {
        if (t->filesz > t->memsz || t->memsz > 1024 * 1024 || t->vaddr + t->filesz > size)
            throw Failure("unsupported eboot TLS layout");
        main_tls[0] = t->vaddr, main_tls[1] = t->filesz, main_tls[2] = t->memsz, main_tls[3] = t->align;
    }
    const Ph& procparam = FindPh(main.elf, PT_SCE_PROCPARAM);

    Bytes outb;
    outb.reserve(image.size() + relocs.size() * 32 + names.size() * 128 + 4096);
    AppendBytes(outb, "BBPROBE5", 8);
    for (uint64_t v : {uint64_t(image.size()), main.elf.entry, uint64_t(segments.size()), uint64_t(relocs.size()),
                       uint64_t(names.size()), flags})
        Append64(outb, v);
    Append64(outb, procparam.vaddr);
    for (uint64_t v : main_tls) Append64(outb, v);
    Append64(outb, table.size());
    for (const Entry& t : table)
        for (uint64_t v : {t.base, t.size, t.init, t.tls_address, t.tls_memsz, t.tls_filesz, t.tls_module}) Append64(outb, v);
    Append64(outb, bindings.size());
    for (auto& b : bindings)
        for (uint64_t v : b) Append64(outb, v);
    for (auto& s : segments)
        for (uint64_t v : s) Append64(outb, v);
    for (const std::string& name : names) {
        if (name.size() >= 128) throw Failure("name too long");
        char field[128] = {};
        std::memcpy(field, name.data(), name.size());
        AppendBytes(outb, field, 128);
    }
    for (const OutReloc& r : relocs) {
        Append64(outb, r.target);
        Append64(outb, r.kind);
        Append64(outb, uint64_t(r.value));
        Append64(outb, uint64_t(r.addend));
    }
    AppendBytes(outb, image.data(), image.size());
    WriteFile(target, outb);
    WriteText(key_file, key);
    std::string summary;
    for (const Entry& t : table) summary += (summary.empty() ? "" : ", ") + t.file + "@" + Hex(t.base);
    Say("Linked modules: " + summary + "; " + std::to_string(bindings.size()) + " native bindings, " +
        std::to_string(unresolved) + " imports left to the host runtime, fs->gs patched=" + std::to_string(fs_patched));
}

std::vector<std::pair<uint64_t, uint64_t>> EbootLoadRanges(const fs::path& game) {
    // The SELF header, its segment table, the ELF header and the program headers come first.
    std::ifstream file(game / L"eboot.bin", std::ios::binary);
    if (!file) throw Failure("cannot read " + PathText(game / L"eboot.bin"));
    Bytes head(1 << 20);
    file.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
    head.resize(size_t(file.gcount()));
    const uint32_t count = Le16(head, 24);
    const size_t base = 32 + size_t(count) * 32;
    const uint64_t phoff = Le64(head, base + 32);
    const uint32_t phnum = Le16(head, base + 56);
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t at = base + phoff + i * 56;
        if (Le32(head, at) == PT_LOAD) ranges.push_back({Le64(head, at + 16), Le64(head, at + 16) + Le64(head, at + 40)});
    }
    return ranges;
}

void WriteContentProfile(const fs::path& game, const fs::path& out, const std::string& sku) {
    auto values = ReadSfo(game / L"sce_sys" / L"param.sfo");
    uint32_t params[4];
    for (int i = 0; i < 4; ++i) {
        auto it = values.find("USER_DEFINED_PARAM_" + std::to_string(i + 1));
        params[i] = 0;
        if (it != values.end()) {
            try {
                params[i] = uint32_t(std::stoul(it->second));
            } catch (...) {
                throw Failure("invalid user-defined parameter");
            }
        }
    }
    if (sku != "full" && sku != "trial") throw Failure("BB_CONTENT_SKU must be full or trial");
    Bytes data;
    AppendBytes(data, "BBCONT01", 8);
    const uint32_t code = sku == "full" ? 3 : 1;
    AppendBytes(data, &code, 4);
    AppendBytes(data, params, 16);
    WriteFile(out / L"content.bin", data);
    Say("AppContent profile: SKU=" + sku + " (probe setting), user params=[" + std::to_string(params[0]) + ", " +
        std::to_string(params[1]) + ", " + std::to_string(params[2]) + ", " + std::to_string(params[3]) +
        "], mounted add-ons=0");
}

}  // namespace prep
