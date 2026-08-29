#pragma once
// lsparself.hpp — minimal self-process ELF symbol resolver.
//
// Provides lsparself::Elf, constructed from a loaded-library name/suffix
// (e.g. "/libart.so"). It locates the module in /proc/self/maps, maps the
// backing file read-only, and resolves symbol names to their *runtime*
// addresses by scanning the ELF symbol tables. It exists to back LSPlant's
// InitInfo.art_symbol_resolver / art_symbol_prefix_resolver callbacks in
// jni/sbx_lsplant.hpp.
//
// Design constraints:
//   * header-only (included by more than one TU -> everything is inline);
//   * no C++ exceptions / RTTI (the module is built -fno-exceptions -fno-rtti);
//   * ILP32 + LP64 clean via ElfW();
//   * failures are reported as a 0 address, never thrown.
//
// Limitation: symbols that live ONLY in a compressed .gnu_debugdata mini
// symtab (fully stripped libart internals) are not resolved here; exported
// (.dynsym) and unstripped (.symtab) symbols are. If the on-device test shows
// a required libart symbol failing to resolve, add .gnu_debugdata (LZMA)
// support here.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <elf.h>
#include <fcntl.h>
#include <link.h>   // ElfW(), Elf{32,64}_* structs
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lsparself {

class Elf {
public:
    explicit Elf(std::string_view name) { parse(name); }

    ~Elf() {
        if (image_ && image_ != MAP_FAILED) munmap(image_, image_size_);
    }

    // Holds an mmap + raw pointers into it; copying/moving would dangle.
    Elf(const Elf&) = delete;
    Elf& operator=(const Elf&) = delete;

    bool valid() const {
        return image_ && load_base_ && (symtab_.syms || dynsym_.syms);
    }

    // Absolute runtime address of the symbol named `name`, or 0 if not found.
    uintptr_t getSymbAddress(std::string_view name) const {
        ElfW(Addr) off = symbolOffset(name);
        return off ? runtimeAddr(off) : 0;
    }

    // Absolute runtime address of the first symbol whose name starts with
    // `prefix`, or 0 if none match.
    uintptr_t getSymbPrefixFirstAddress(std::string_view prefix) const {
        ElfW(Addr) off = symbolPrefixOffset(prefix);
        return off ? runtimeAddr(off) : 0;
    }

private:
    struct SymTab {
        const ElfW(Sym)* syms = nullptr;
        const char*      str  = nullptr;
        size_t           count = 0;
        size_t           strsz = 0;
    };

    uintptr_t runtimeAddr(ElfW(Addr) st_value) const {
        return load_base_ + static_cast<uintptr_t>(st_value) - vaddr_bias_;
    }

    const char* symName(const SymTab& t, ElfW(Word) st_name) const {
        if (!t.str || static_cast<size_t>(st_name) >= t.strsz) return nullptr;
        return t.str + st_name;
    }

    ElfW(Addr) scanExact(const SymTab& t, std::string_view name) const {
        for (size_t i = 0; i < t.count; ++i) {
            const ElfW(Sym)& s = t.syms[i];
            if (s.st_name == 0 || s.st_value == 0) continue;
            const char* n = symName(t, s.st_name);
            if (n && name.compare(n) == 0) return s.st_value;
        }
        return 0;
    }

    ElfW(Addr) scanPrefix(const SymTab& t, std::string_view p) const {
        for (size_t i = 0; i < t.count; ++i) {
            const ElfW(Sym)& s = t.syms[i];
            if (s.st_name == 0 || s.st_value == 0) continue;
            const char* n = symName(t, s.st_name);
            if (n && std::strncmp(n, p.data(), p.size()) == 0) return s.st_value;
        }
        return 0;
    }

    ElfW(Addr) symbolOffset(std::string_view name) const {
        ElfW(Addr) v = 0;
        if (symtab_.syms) v = scanExact(symtab_, name);
        if (!v && dynsym_.syms) v = scanExact(dynsym_, name);
        return v;
    }

    ElfW(Addr) symbolPrefixOffset(std::string_view p) const {
        ElfW(Addr) v = 0;
        if (symtab_.syms) v = scanPrefix(symtab_, p);
        if (!v && dynsym_.syms) v = scanPrefix(dynsym_, p);
        return v;
    }

    void parse(std::string_view name) {
        std::string path;
        if (!findInMaps(name, path, load_base_)) return;
        mapFileAndIndex(path);
    }

    // Scan /proc/self/maps for the mapping backing `name`; return its on-disk
    // path plus the load base (the address at which file offset 0 is mapped).
    bool findInMaps(std::string_view name, std::string& outPath, uintptr_t& outBase) {
        FILE* fp = fopen("/proc/self/maps", "re");
        if (!fp) return false;
        char line[512];
        uintptr_t          best_base = 0;
        unsigned long long best_off  = ~0ULL;
        std::string        best_path;
        while (fgets(line, sizeof(line), fp)) {
            // start-end perms offset dev(maj:min) inode pathname
            unsigned long long start = 0, end = 0, off = 0;
            char perms[8] = {0};
            int  pos = 0;
            if (sscanf(line, "%llx-%llx %7s %llx %*x:%*x %*u %n",
                       &start, &end, perms, &off, &pos) < 4)
                continue;
            const char* p = line + pos;
            while (*p == ' ') ++p;
            size_t len = std::strlen(p);
            while (len && (p[len - 1] == '\n' || p[len - 1] == ' ' || p[len - 1] == '\t')) --len;
            if (len < name.size()) continue;
            std::string_view pv(p, len);
            if (pv.compare(pv.size() - name.size(), name.size(), name) != 0) continue;
            (void)end;
            if (off < best_off) {  // prefer the lowest file offset (ideally 0)
                best_off  = off;
                best_base = static_cast<uintptr_t>(start) - static_cast<uintptr_t>(off);
                best_path.assign(p, len);
            }
        }
        fclose(fp);
        if (best_path.empty()) return false;
        outPath = best_path;
        outBase = best_base;
        return true;
    }

    void mapFileAndIndex(const std::string& path) {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ElfW(Ehdr)))) {
            close(fd);
            return;
        }
        image_size_ = static_cast<size_t>(st.st_size);
        image_ = mmap(nullptr, image_size_, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (image_ == MAP_FAILED) { image_ = nullptr; return; }

        const auto base = reinterpret_cast<uintptr_t>(image_);
        auto* eh = reinterpret_cast<ElfW(Ehdr)*>(base);
        if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return;

        // Load bias = min p_vaddr among PT_LOAD segments (0 on typical .so).
        vaddr_bias_ = 0;
        bool have_bias = false;
        const size_t phnum = eh->e_phnum;
        if (eh->e_phoff && phnum) {
            for (size_t i = 0; i < phnum; ++i) {
                auto* ph = reinterpret_cast<ElfW(Phdr)*>(
                    base + eh->e_phoff + i * eh->e_phentsize);
                if (ph->p_type == PT_LOAD && (!have_bias || ph->p_vaddr < vaddr_bias_)) {
                    vaddr_bias_ = ph->p_vaddr;
                    have_bias = true;
                }
            }
        }

        const size_t shnum = eh->e_shnum;
        if (!eh->e_shoff || !shnum) return;
        auto* sh = reinterpret_cast<ElfW(Shdr)*>(base + eh->e_shoff);

        const ElfW(Shdr)* dynsym_sh = nullptr;
        const ElfW(Shdr)* symtab_sh = nullptr;
        for (size_t i = 0; i < shnum; ++i) {
            if (sh[i].sh_type == SHT_DYNSYM) dynsym_sh = &sh[i];
            else if (sh[i].sh_type == SHT_SYMTAB) symtab_sh = &sh[i];
        }

        auto fill = [&](const ElfW(Shdr)* symsh, SymTab& out) {
            if (!symsh || symsh->sh_entsize == 0) return;
            if (static_cast<size_t>(symsh->sh_link) >= shnum) return;
            const ElfW(Shdr)& strsh = sh[symsh->sh_link];
            out.syms  = reinterpret_cast<const ElfW(Sym)*>(base + symsh->sh_offset);
            out.count = static_cast<size_t>(symsh->sh_size / symsh->sh_entsize);
            out.str   = reinterpret_cast<const char*>(base + strsh.sh_offset);
            out.strsz = static_cast<size_t>(strsh.sh_size);
        };
        fill(dynsym_sh, dynsym_);
        fill(symtab_sh, symtab_);
    }

    void*     image_      = nullptr;
    size_t    image_size_ = 0;
    uintptr_t load_base_  = 0;
    uintptr_t vaddr_bias_ = 0;
    SymTab    dynsym_;
    SymTab    symtab_;
};

}  // namespace lsparself
