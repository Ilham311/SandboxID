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
// Symbol sources, in priority order: unstripped .symtab, then the
// .gnu_debugdata (MiniDebugInfo) mini-symtab, then exported .dynsym. On a
// stripped retail libart.so the internal ART symbols LSPlant needs live ONLY
// in .gnu_debugdata — an xz-compressed mini-ELF — so we decompress it with
// xz-embedded (see the LSPARSELF_WITH_GNU_DEBUGDATA block below). That support
// is compiled in automatically whenever <xz.h> is on the include path; without
// it (e.g. the host syntax-check) we fall back to .symtab/.dynsym only, which
// is enough on GSI/userdebug images that ship an unstripped .symtab.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <elf.h>
#include <fcntl.h>
#include <link.h>   // ElfW(), Elf{32,64}_* structs
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- Optional .gnu_debugdata (MiniDebugInfo) support -----------------------
// A stripped retail libart.so exports almost nothing in .dynsym and carries no
// .symtab; its internal ART symbols (art::ArtMethod internals, class-linker,
// jit, ...) survive ONLY inside a .gnu_debugdata section — an LZMA/xz stream
// wrapping a mini-ELF whose .symtab lists them. If we don't decompress it,
// LSPlant's art_symbol_resolver resolves those to 0 and lsplant::Init() fails,
// silently disabling every L3 hook. We decode it with xz-embedded when its
// public header is on the include path (wired by jni/CMakeLists.txt +
// fetch_lsplant_deps.sh). When it is absent (e.g. the host syntax-check, which
// has no xz), we degrade to .symtab/.dynsym exactly as before.
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

    // Holds an mmap + raw pointers into it; copying/moving would dangle.
    Elf(const Elf&) = delete;
    Elf& operator=(const Elf&) = delete;

    bool valid() const {
        return image_ && load_base_ && (symtab_.syms || dynsym_.syms || debugdata_.syms);
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

    // Scan /proc/self/maps for the mapping backing `name`; return its on-disk
    // path plus the load base (the address at which file offset 0 is mapped).
    bool findInMaps(std::string_view name, std::string& outPath, uintptr_t& outBase) {
        // A maps line's pathname is last and can be long (deep app-lib paths,
        // long package names). A fixed buffer would truncate it, the suffix
        // match would fail, and lsplant::Init() would silently disable every L3
        // hook. getline(3) grows cbuf to fit any line. Opened with fopen("re")
        // rather than std::ifstream so the fd keeps O_CLOEXEC (std::ifstream
        // gives no portable way to set it, and a leaked /proc/self/maps fd
        // surviving into a forked/exec'd child is an avoidable info leak).
        FILE* fp = fopen("/proc/self/maps", "re");
        if (!fp) return false;
        uintptr_t          best_base = 0;
        unsigned long long best_off  = ~0ULL;
        std::string        best_path;
        char*   cbuf = nullptr;
        size_t  cap  = 0;
        ssize_t n;
        while ((n = getline(&cbuf, &cap, fp)) != -1) {
            // start-end perms offset dev(maj:min) inode pathname
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
            if (off < best_off) {  // prefer the lowest file offset (ideally 0)
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
            // libart.so is a trusted system file, but a corrupt or truncated
            // image must not make us point syms/str past the mmap (OOB read /
            // crash in the hosting app). Same guard indexInnerElf() already has.
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
        // Only needed when the real .symtab is stripped (retail). Located by
        // NAME (.gnu_debugdata is SHT_PROGBITS), so consult the section-header
        // string table.
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
                if (!sz || off > image_size_ || sz > image_size_ - off) break;  // out of bounds
                loadGnuDebugData(reinterpret_cast<const uint8_t*>(base + off),
                                 static_cast<size_t>(sz));
                break;
            }
        }
#endif
    }

#if LSPARSELF_WITH_GNU_DEBUGDATA
    // Decompress the .xz blob into debugbuf_, then index the inner mini-ELF's
    // symbol table into debugdata_ (whose pointers alias debugbuf_).
    void loadGnuDebugData(const uint8_t* xz_in, size_t xz_in_sz) {
        if (!xz_in || xz_in_sz < 6) return;                 // shorter than an xz magic
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

        const ElfW(Shdr)* sym_sh = nullptr;              // prefer .symtab, fall back to .dynsym
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

    // Multi-call xz-embedded decode. Returns false on any decoder error.
    static bool xzInflate(const uint8_t* in, size_t in_sz, std::string& out) {
        out.clear();
        xz_crc32_init();
#ifdef XZ_USE_CRC64
        xz_crc64_init();
#endif
        struct xz_dec* dec = xz_dec_init(XZ_DYNALLOC, 1u << 26);  // 64 MiB dict ceiling
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
            if (r != XZ_OK) break;                              // decoder error
            if (b.out_pos == 0 && b.in_pos == b.in_size) break; // truncated stream
            if (out.size() > (256u << 20)) break;               // runaway guard
        }
        xz_dec_end(dec);
        return ok && !out.empty();
    }
#endif  // LSPARSELF_WITH_GNU_DEBUGDATA

    void*     image_      = nullptr;
    size_t    image_size_ = 0;
    uintptr_t load_base_  = 0;
    uintptr_t vaddr_bias_ = 0;
    SymTab    dynsym_;
    SymTab    symtab_;
#if LSPARSELF_WITH_GNU_DEBUGDATA
    std::string debugbuf_;   // owns the decompressed .gnu_debugdata mini-ELF
#endif
    SymTab    debugdata_;    // symbols recovered from it (alias into debugbuf_)
};

}  // namespace lsparself
