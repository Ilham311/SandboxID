#pragma once
//
// A minimal, dependency-free ELF symbol resolver for LSPlant.
//
// LSPlant needs to find ART internals inside libart.so at runtime and cannot do
// it itself: the addresses are not exported to the app's namespace, and the
// symbol table is not reachable through dlsym(). The library therefore takes a
// callback that must resolve a symbol name to an address in the loaded image.
//
// This replaces the out-of-tree `lsparself.hpp` the previous L3 attempt pulled
// from an unpinned location (and which the fetch script could only locate by
// guessing at a path inside the LSPlant checkout). Everything needed is here,
// it is testable on the host, and it has no Android-only dependency.
//
// Two symbol sources are consulted, in this order:
//
//   1. .dynsym — always present. On a release ROM libart.so is stripped, but the
//      symbols the linker exports are still here. This is what dlsym() sees.
//   2. .symtab — present on userdebug/eng builds and on some OEM ROMs. It is NOT
//      mapped into memory, so it is read from the file on disk through the
//      section header table. This is what makes the same module work on a
//      debuggable device and on a stock one.
//
// Both are loaded once, lazily, on the first query.

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef SBXELF_MAX_SYMBOLS
// Sanity ceiling for the symbol-count derivation. A real libart.so has tens of
// thousands of symbols; anything past this means the hash table was misread and
// walking it would run off the end of the mapping.
#define SBXELF_MAX_SYMBOLS (4u * 1024u * 1024u)
#endif

namespace sbxelf {

namespace detail {

struct FindState {
    const char* needle = nullptr;
    uintptr_t   base   = 0;
    const ElfW(Phdr)* phdr = nullptr;
    size_t      phnum  = 0;
    std::string path;
    bool        found  = false;
};

inline int find_cb(struct dl_phdr_info* info, size_t, void* data) {
    FindState* st = static_cast<FindState*>(data);
    if (!info || !info->dlpi_name || info->dlpi_name[0] == '\0') return 0;
    if (!std::strstr(info->dlpi_name, st->needle)) return 0;
    st->base  = static_cast<uintptr_t>(info->dlpi_addr);
    st->phdr  = info->dlpi_phdr;
    st->phnum = info->dlpi_phnum;
    st->path  = info->dlpi_name;
    st->found = true;
    return 1;   // stop iterating
}

// Dynamic entries that hold pointers are relocated by the linker in the loaded
// image, but a few toolchains leave them as link-time offsets. Normalise both:
// a value below the load bias cannot be an address in this mapping.
inline const void* reloc(const void* p, uintptr_t base) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return reinterpret_cast<const void*>(v < base ? v + base : v);
}

}  // namespace detail

class Elf {
public:
    /// @param soname_substring part of the loaded file name, e.g. "libart.so"
    explicit Elf(const char* soname_substring) : needle_(soname_substring) {}

    Elf(const Elf&) = delete;
    Elf& operator=(const Elf&) = delete;

    /// True when the image was found and at least one symbol source parsed.
    bool ok() { return load() && !symbols_.empty(); }

    /// Address of `name`, or nullptr. Empty names never match.
    void* getSymbAddress(std::string_view name) {
        if (name.empty() || !load()) return nullptr;
        auto it = symbols_.find(std::string(name));
        return it == symbols_.end() ? nullptr : it->second;
    }

    /// Address of the first symbol whose name starts with `prefix`.
    ///
    /// LSPlant uses this for names whose suffix encodes a build-specific type
    /// signature. "First" means first in .dynsym, then .symtab — stable for a
    /// given image, which is all the caller needs.
    void* getSymbPrefixFirstAddress(std::string_view prefix) {
        if (prefix.empty() || !load()) return nullptr;
        for (const auto& e : order_) {
            if (e.first.size() >= prefix.size() &&
                std::memcmp(e.first.data(), prefix.data(), prefix.size()) == 0) {
                return e.second;
            }
        }
        return nullptr;
    }

    size_t symbol_count() {
        return load() ? symbols_.size() : 0;
    }

private:
    bool load() {
        if (loaded_) return true;
        loaded_ = true;   // one attempt only, success or not

        detail::FindState st;
        st.needle = needle_;
        dl_iterate_phdr(&detail::find_cb, &st);
        if (!st.found || !st.phdr) return false;
        base_ = st.base;
        path_ = st.path;

        const ElfW(Dyn)* dyn = nullptr;
        for (size_t i = 0; i < st.phnum; ++i) {
            if (st.phdr[i].p_type == PT_DYNAMIC) {
                dyn = reinterpret_cast<const ElfW(Dyn)*>(
                    base_ + st.phdr[i].p_vaddr);
                break;
            }
        }
        if (dyn) load_dynamic(dyn);
        // .symtab first in the map would shadow .dynsym; insert .dynsym first
        // and let the file table only fill gaps, so the exported symbol — the
        // one the runtime actually uses — always wins.
        load_symtab_from_file();
        return !symbols_.empty();
    }

    void add(std::string_view name, void* addr) {
        if (name.empty() || addr == nullptr) return;
        std::string key(name);
        if (symbols_.find(key) != symbols_.end()) return;
        symbols_.emplace(key, addr);
        order_.emplace_back(std::move(key), addr);
    }

    void load_dynamic(const ElfW(Dyn)* dyn) {
        const void* symtab  = nullptr;
        const void* strtab  = nullptr;
        const void* hash    = nullptr;
        const void* gnu     = nullptr;
        size_t      strsz   = 0;
        size_t      syment  = sizeof(ElfW(Sym));

        for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
            switch (d->d_tag) {
                case DT_SYMTAB:  symtab = detail::reloc(reinterpret_cast<const void*>(d->d_un.d_ptr), base_); break;
                case DT_STRTAB:  strtab = detail::reloc(reinterpret_cast<const void*>(d->d_un.d_ptr), base_); break;
                case DT_HASH:    hash   = detail::reloc(reinterpret_cast<const void*>(d->d_un.d_ptr), base_); break;
                case DT_GNU_HASH:gnu    = detail::reloc(reinterpret_cast<const void*>(d->d_un.d_ptr), base_); break;
                case DT_STRSZ:   strsz  = static_cast<size_t>(d->d_un.d_val); break;
                case DT_SYMENT:  syment = static_cast<size_t>(d->d_un.d_val); break;
                default: break;
            }
        }
        if (!symtab || !strtab || strsz == 0 || syment == 0) return;

        size_t count = 0;
        if (hash) {
            // SysV hash: [nbucket, nchain, buckets..., chains...]
            const uint32_t* h = static_cast<const uint32_t*>(hash);
            count = h[1];
        } else if (gnu) {
            count = gnu_hash_symbol_count(gnu);
        }
        if (count == 0 || count > SBXELF_MAX_SYMBOLS) return;

        const char*   str = static_cast<const char*>(strtab);
        const uint8_t* sym = static_cast<const uint8_t*>(symtab);
        for (size_t i = 1; i < count; ++i) {   // [0] is the null symbol
            ElfW(Sym) s;
            std::memcpy(&s, sym + i * syment, sizeof(s));
            if (s.st_name == 0 || s.st_name >= strsz) continue;
            const char* name = str + s.st_name;
            // A symbol can be declared but not defined in this image; its value
            // is then 0 and must not be handed to the inline hooker.
            if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
            add(std::string_view(name), reinterpret_cast<void*>(base_ + s.st_value));
        }
    }

    /// Symbol count from a GNU hash table. There is no count field: the highest
    /// bucket index is the lowest symbol covered, and the chain is terminated by
    /// the low bit, so walk to the end.
    static size_t gnu_hash_symbol_count(const void* gnu) {
        const uint32_t* p = static_cast<const uint32_t*>(gnu);
        const uint32_t nbuckets  = p[0];
        const uint32_t symoffset = p[1];
        const uint32_t bloomsz   = p[2];
        if (nbuckets == 0 || nbuckets > SBXELF_MAX_SYMBOLS) return 0;

        const uint8_t* q = reinterpret_cast<const uint8_t*>(p + 4);
        q += static_cast<size_t>(bloomsz) * sizeof(ElfW(Addr));   // bloom filter
        const uint32_t* buckets = reinterpret_cast<const uint32_t*>(q);
        const uint32_t* chain   = buckets + nbuckets;

        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; ++i)
            if (buckets[i] > last) last = buckets[i];
        if (last < symoffset) return symoffset;   // every bucket empty

        uint32_t idx = last;
        while (idx < SBXELF_MAX_SYMBOLS) {
            if (chain[idx - symoffset] & 1u) return static_cast<size_t>(idx) + 1;
            ++idx;
        }
        return 0;
    }

    /// Reads .symtab out of the file on disk.
    ///
    /// Section headers are not mapped into the process, so this is the only way
    /// to see symbols the linker did not export. Everything is bounds-checked
    /// against the file size: the section table is attacker-adjacent input on a
    /// device we do not control, and a bad sh_offset must not read past the map.
    void load_symtab_from_file() {
        if (path_.empty()) return;
        int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;

        struct stat sb {};
        if (::fstat(fd, &sb) != 0 || sb.st_size <= 0) { ::close(fd); return; }
        const size_t fsize = static_cast<size_t>(sb.st_size);

        ElfW(Ehdr) eh {};
        if (::pread(fd, &eh, sizeof(eh), 0) != static_cast<ssize_t>(sizeof(eh))) {
            ::close(fd); return;
        }
        if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) { ::close(fd); return; }
        if (eh.e_shentsize != sizeof(ElfW(Shdr)) || eh.e_shnum == 0) { ::close(fd); return; }
        if (eh.e_shoff == 0 || eh.e_shoff > fsize) { ::close(fd); return; }

        const size_t table_bytes = static_cast<size_t>(eh.e_shnum) * sizeof(ElfW(Shdr));
        if (eh.e_shoff + table_bytes > fsize) { ::close(fd); return; }
        std::vector<ElfW(Shdr)> sh(eh.e_shnum);
        if (::pread(fd, sh.data(), table_bytes, static_cast<off_t>(eh.e_shoff)) !=
            static_cast<ssize_t>(table_bytes)) {
            ::close(fd); return;
        }

        const bool pic = (eh.e_type == ET_DYN);
        for (const ElfW(Shdr)& s : sh) {
            if (s.sh_type != SHT_SYMTAB) continue;
            if (s.sh_link >= eh.e_shnum) continue;
            const ElfW(Shdr)& str = sh[s.sh_link];
            if (str.sh_type != SHT_STRTAB) continue;
            if (s.sh_offset + s.sh_size > fsize || str.sh_offset + str.sh_size > fsize) continue;
            if (s.sh_entsize != sizeof(ElfW(Sym)) || s.sh_size == 0) continue;

            std::vector<char> syms(s.sh_size);
            std::vector<char> strs(str.sh_size);
            if (::pread(fd, syms.data(), syms.size(), static_cast<off_t>(s.sh_offset)) !=
                static_cast<ssize_t>(syms.size())) continue;
            if (::pread(fd, strs.data(), strs.size(), static_cast<off_t>(str.sh_offset)) !=
                static_cast<ssize_t>(strs.size())) continue;

            const size_t n = syms.size() / sizeof(ElfW(Sym));
            for (size_t i = 1; i < n; ++i) {
                ElfW(Sym) sym;
                std::memcpy(&sym, syms.data() + i * sizeof(sym), sizeof(sym));
                if (sym.st_name == 0 || sym.st_name >= strs.size()) continue;
                if (sym.st_shndx == SHN_UNDEF || sym.st_value == 0) continue;
                const char* name = strs.data() + sym.st_name;
                // A malformed table can have no terminator before the end.
                if (std::memchr(name, '\0', strs.size() - sym.st_name) == nullptr) continue;
                const uintptr_t addr = pic ? base_ + sym.st_value : sym.st_value;
                add(std::string_view(name), reinterpret_cast<void*>(addr));
            }
            break;   // one .symtab per image
        }
        ::close(fd);
    }

    const char* needle_ = "";
    bool        loaded_ = false;
    uintptr_t   base_   = 0;
    std::string path_;
    std::unordered_map<std::string, void*> symbols_;
    std::vector<std::pair<std::string, void*>> order_;
};

}  // namespace sbxelf
