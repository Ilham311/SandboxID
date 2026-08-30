#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(SBX_LSPARSELF_NO_XZ)
#  define LSPARSELF_WITH_GNU_DEBUGDATA 0
#elif __has_include(<xz.h>)
#  define LSPARSELF_WITH_GNU_DEBUGDATA 1
#  include <vector>
extern "C" {
#  include <xz.h>
}
#else
#  define LSPARSELF_WITH_GNU_DEBUGDATA 0
#endif

namespace lsparself {

class Elf {
public:
    explicit Elf(std::string_view name) { parse(name); }

    ~Elf() {
        if (image_ && image_ != MAP_FAILED) munmap(image_, image_size_);
    }

    Elf(const Elf&) = delete;
    Elf& operator=(const Elf&) = delete;

    bool valid() const {
        return image_ && load_base_ && (symtab_.syms || dynsym_.syms || debugdata_.syms);
    }

    uintptr_t getSymbAddress(std::string_view name) const {
        ElfW(Addr) off = symbolOffset(name);
        return off ? runtimeAddr(off) : 0;
    }

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
        if (symtab_.syms)          v = scanExact(symtab_, name);
        if (!v && debugdata_.syms) v = scanExact(debugdata_, name);
        if (!v && dynsym_.syms)    v = scanExact(dynsym_, name);
        return v;
    }

    ElfW(Addr) symbolPrefixOffset(std::string_view p) const {
        ElfW(Addr) v = 0;
        if (symtab_.syms)          v = scanPrefix(symtab_, p);
        if (!v && debugdata_.syms) v = scanPrefix(debugdata_, p);
        if (!v && dynsym_.syms)    v = scanPrefix(dynsym_, p);
        return v;
    }

    void parse(std::string_view name) {
        std::string path;
        if (!findInMaps(name, path, load_base_)) return;
        mapFileAndIndex(path);
    }

    bool findInMaps(std::string_view name, std::string& outPath, uintptr_t& outBase) {

        FILE* fp = fopen("/proc/self/maps", "re");
        if (!fp) return false;
        uintptr_t          best_base = 0;
        unsigned long long best_off  = ~0ULL;
        std::string        best_path;
        char*   cbuf = nullptr;
        size_t  cap  = 0;
        ssize_t n;
        while ((n = getline(&cbuf, &cap, fp)) != -1) {

            unsigned long long start = 0, end = 0, off = 0;
            char perms[8] = {0};
            int  pos = 0;
            if (sscanf(cbuf, "%llx-%llx %7s %llx %*x:%*x %*u %n",
                       &start, &end, perms, &off, &pos) < 4)
                continue;
            const char* p = cbuf + pos;
            while (*p == ' ') ++p;
            size_t len = std::strlen(p);
            while (len && (p[len - 1] == '\n' || p[len - 1] == ' ' || p[len - 1] == '\t')) --len;
            if (len < name.size()) continue;
            std::string_view pv(p, len);
            if (pv.compare(pv.size() - name.size(), name.size(), name) != 0) continue;
            (void)end;
            if (off < best_off) {
                best_off  = off;
                best_base = static_cast<uintptr_t>(start) - static_cast<uintptr_t>(off);
                best_path.assign(p, len);
            }
        }
        free(cbuf);
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

            if (symsh->sh_offset + symsh->sh_size > image_size_) return;
            if (strsh.sh_offset  + strsh.sh_size  > image_size_) return;
            out.syms  = reinterpret_cast<const ElfW(Sym)*>(base + symsh->sh_offset);
            out.count = static_cast<size_t>(symsh->sh_size / symsh->sh_entsize);
            out.str   = reinterpret_cast<const char*>(base + strsh.sh_offset);
            out.strsz = static_cast<size_t>(strsh.sh_size);
        };
        fill(dynsym_sh, dynsym_);
        fill(symtab_sh, symtab_);

#if LSPARSELF_WITH_GNU_DEBUGDATA

        if (!symtab_.syms && eh->e_shstrndx != SHN_UNDEF
                && static_cast<size_t>(eh->e_shstrndx) < shnum) {
            const ElfW(Shdr)& shstr = sh[eh->e_shstrndx];
            const char*  shname    = reinterpret_cast<const char*>(base + shstr.sh_offset);
            const size_t shname_sz = static_cast<size_t>(shstr.sh_size);
            for (size_t i = 0; i < shnum; ++i) {
                if (sh[i].sh_type != SHT_PROGBITS) continue;
                if (static_cast<size_t>(sh[i].sh_name) >= shname_sz) continue;
                if (std::strcmp(shname + sh[i].sh_name, ".gnu_debugdata") != 0) continue;
                const uint64_t off = sh[i].sh_offset, sz = sh[i].sh_size;
                if (!sz || off > image_size_ || sz > image_size_ - off) break;
                loadGnuDebugData(reinterpret_cast<const uint8_t*>(base + off),
                                 static_cast<size_t>(sz));
                break;
            }
        }
#endif
    }

#if LSPARSELF_WITH_GNU_DEBUGDATA

    void loadGnuDebugData(const uint8_t* xz_in, size_t xz_in_sz) {
        if (!xz_in || xz_in_sz < 6) return;
        if (!xzInflate(xz_in, xz_in_sz, debugbuf_)) { debugbuf_.clear(); return; }
        indexInnerElf();
    }

    void indexInnerElf() {
        const size_t n = debugbuf_.size();
        if (n < sizeof(ElfW(Ehdr))) return;
        const auto ibase = reinterpret_cast<uintptr_t>(debugbuf_.data());
        auto* eh = reinterpret_cast<const ElfW(Ehdr)*>(ibase);
        if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return;
        if (!eh->e_shoff || !eh->e_shnum || eh->e_shentsize < sizeof(ElfW(Shdr))) return;
        if (eh->e_shoff + static_cast<uint64_t>(eh->e_shnum) * eh->e_shentsize > n) return;
        auto*        sh    = reinterpret_cast<const ElfW(Shdr)*>(ibase + eh->e_shoff);
        const size_t shnum = eh->e_shnum;

        const ElfW(Shdr)* sym_sh = nullptr;
        for (size_t i = 0; i < shnum; ++i)
            if (sh[i].sh_type == SHT_SYMTAB) { sym_sh = &sh[i]; break; }
        if (!sym_sh)
            for (size_t i = 0; i < shnum; ++i)
                if (sh[i].sh_type == SHT_DYNSYM) { sym_sh = &sh[i]; break; }
        if (!sym_sh || sym_sh->sh_entsize == 0) return;
        if (static_cast<size_t>(sym_sh->sh_link) >= shnum) return;
        const ElfW(Shdr)& strsh = sh[sym_sh->sh_link];
        if (sym_sh->sh_offset + sym_sh->sh_size > n) return;
        if (strsh.sh_offset  + strsh.sh_size  > n) return;
        debugdata_.syms  = reinterpret_cast<const ElfW(Sym)*>(ibase + sym_sh->sh_offset);
        debugdata_.count = static_cast<size_t>(sym_sh->sh_size / sym_sh->sh_entsize);
        debugdata_.str   = reinterpret_cast<const char*>(ibase + strsh.sh_offset);
        debugdata_.strsz = static_cast<size_t>(strsh.sh_size);
    }

    static bool xzInflate(const uint8_t* in, size_t in_sz, std::string& out) {
        out.clear();
        xz_crc32_init();
#ifdef XZ_USE_CRC64
        xz_crc64_init();
#endif
        struct xz_dec* dec = xz_dec_init(XZ_DYNALLOC, 1u << 26);
        if (!dec) return false;
        struct xz_buf b;
        std::memset(&b, 0, sizeof(b));
        b.in = in; b.in_pos = 0; b.in_size = in_sz;
        std::vector<uint8_t> chunk(256 * 1024);
        bool ok = false;
        for (;;) {
            b.out = chunk.data(); b.out_pos = 0; b.out_size = chunk.size();
            enum xz_ret r = xz_dec_run(dec, &b);
            if (b.out_pos)
                out.append(reinterpret_cast<const char*>(chunk.data()), b.out_pos);
            if (r == XZ_STREAM_END) { ok = true; break; }
            if (r != XZ_OK) break;
            if (b.out_pos == 0 && b.in_pos == b.in_size) break;
            if (out.size() > (256u << 20)) break;
        }
        xz_dec_end(dec);
        return ok && !out.empty();
    }
#endif

    void*     image_      = nullptr;
    size_t    image_size_ = 0;
    uintptr_t load_base_  = 0;
    uintptr_t vaddr_bias_ = 0;
    SymTab    dynsym_;
    SymTab    symtab_;
#if LSPARSELF_WITH_GNU_DEBUGDATA
    std::string debugbuf_;
#endif
    SymTab    debugdata_;
};

}
