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
// Three symbol sources are consulted, in this order:
//
//   1. .dynsym — always present. On a release ROM libart.so is stripped, but the
//      symbols the linker exports are still here. This is what dlsym() sees.
//   2. .symtab — present on userdebug/eng builds and on some OEM ROMs. It is NOT
//      mapped into memory, so it is read from the file on disk through the
//      section header table. This is what makes the same module work on a
//      debuggable device and on a stock one.
//   3. .gnu_debugdata — a mini-ELF (xz-compressed, .symtab only) that Android
//      builds keep on stripped release libraries. It is what `eu-unstrip` reads
//      to recover names the ROM stripped. On the test device (HyperOS 3.0.3,
//      API 35) this is the ONLY source for GetMethodShorty,
//      ClassLinker::RegisterNative/UnregisterNative and the art_quick_* entry
//      trampolines, all of which LSPlant's Init requires. Without it L3 could
//      never initialise on any release ROM.
//
// All are loaded once, lazily, on the first query.

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

#ifndef SBXELF_MAX_DEBUGDATA
// Ceiling for the decompressed .gnu_debugdata image. The real one on the test
// device is 2,048,048 B (a .symtab of 16,656 names); 16 MiB is 8x headroom and
// bounds what a malformed/hostile stream can make us allocate.
#define SBXELF_MAX_DEBUGDATA (16u * 1024u * 1024u)
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

// True when [off, off+len) lies inside a buffer of `size` bytes.
//
// The plain `off + len > size` test every ELF reader writes is wrong for
// hostile input: both fields are read from the file, and a sh_offset near
// UINTPTR_MAX wraps the sum back below size, so the check passes and the next
// memcpy/pread uses an offset far outside the buffer. Test the offset against
// the size first, then the length against what is left.
inline bool in_range(uint64_t off, uint64_t len, uint64_t size) {
    return off <= size && len <= size - off;
}

}  // namespace detail

class Elf {
public:
    /// @param soname_substring part of the loaded file name, e.g. "libart.so"
    /// @param want_debugdata   read .gnu_debugdata too (see load_debugdata).
    ///        Pass false when resolving a *helper* library, so the helper lookup
    ///        cannot recurse back into this one.
    explicit Elf(const char* soname_substring, bool want_debugdata = true)
        : needle_(soname_substring), want_debugdata_(want_debugdata) {}

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

    /// Address of the first symbol whose name is `name` up to a toolchain-added
    /// suffix, or whose name has `name` as a prefix followed by one.
    ///
    /// A release ROM built with LTO mangles file-local symbols with a
    /// `.__uniq.<hash>` (and sometimes `.llvm.<hash>`) tail, so the name
    /// LSPlant asks for — `_ZN3artL15GetMethodShortyEP7_JNIEnvP10_jmethodID` —
    /// is really `..._jmethodID.__uniq.224004613612541769487030999398936232930`
    /// in .gnu_debugdata. A plain prefix match cannot find it, because the
    /// caller's prefix already contains a *different* trailing type signature
    /// than the ROM's. This matches the requested name first and only then
    /// falls back to a prefix match, which is exactly the two-step Dlsym()
    /// already performs — it just needs the suffix tolerated.
    void* getSymbAddressOrUniq(std::string_view name) {
        if (name.empty() || !load()) return nullptr;
        if (void* exact = getSymbAddress(name)) return exact;
        for (const auto& e : order_) {
            if (e.first.size() <= name.size()) continue;
            if (std::memcmp(e.first.data(), name.data(), name.size()) != 0) continue;
            const char* tail = e.first.data() + name.size();
            if (tail[0] == '.' && (std::strncmp(tail, ".__uniq.", 8) == 0 ||
                                   std::strncmp(tail, ".llvm.", 6) == 0)) {
                return e.second;
            }
        }
        return nullptr;
    }

    size_t symbol_count() {
        // load() is best-effort here: when the image was never found it returns
        // false, but symbols_ can still hold entries fed in directly (the host
        // test does exactly that). Report what we actually have, not what the
        // image lookup happened to decide.
        load();
        return symbols_.size();
    }

    /// Decompresses an xz container. Public only so a host test can drive it
    /// against a real .gnu_debugdata blob; production callers go through
    /// load_debugdata().
    static bool xz_decompress_public(const std::vector<unsigned char>& in,
                                     std::vector<unsigned char>& out) {
        return xz_decompress(in, out);
    }

    /// Feeds an already-decompressed mini-ELF into this image's symbol table.
    /// Also public for the same reason: it is the only way to exercise the
    /// .symtab parser without a device libart.so that can be dlopen()ed.
    void load_mini_elf_public(const std::vector<unsigned char>& elf) { load_mini_elf(elf); }

    /// True when a liblzma with the xz-embedded Lzma2Decode entry point is
    /// mapped. Exposed so a test can skip rather than fail on a host that has
    /// only the public liblzma (which does not export it).
    static bool xz_decompressor_available() { return lzma_decode_entry() != nullptr; }

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
        // one the runtime actually uses — always wins. .gnu_debugdata goes last
        // for the same reason: it is the most complete table but also the one
        // most likely to disagree with what the linker resolved.
        load_symtab_from_file();
        load_debugdata();
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
        if (!detail::in_range(eh.e_shoff, table_bytes, fsize)) { ::close(fd); return; }
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
            if (!detail::in_range(s.sh_offset, s.sh_size, fsize) ||
                !detail::in_range(str.sh_offset, str.sh_size, fsize)) continue;
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

    /// Reads the .gnu_debugdata section: an xz-compressed mini-ELF that carries
    /// only a .symtab (plus a NOBITS .text describing the *real* image's
    /// layout). Its st_value fields are link-time addresses in the main
    /// library's address space, so `base_ + st_value` is the runtime address —
    /// verified on the test device by reading real bytes at
    /// art_quick_to_interpreter_bridge (0x288350) and ExecuteNterpImpl
    /// (0x6aa040) through the main library's PT_LOAD mapping.
    ///
    /// Failure is always silent: an image without the section, without a
    /// decompressor, or with a stream we cannot parse simply contributes
    /// nothing, and every other symbol source keeps working.
    void load_debugdata() {
        if (path_.empty() || !want_debugdata_) return;
        int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;

        struct stat sb {};
        if (::fstat(fd, &sb) != 0 || sb.st_size <= 0) { ::close(fd); return; }
        const size_t fsize = static_cast<size_t>(sb.st_size);

        ElfW(Ehdr) eh {};
        if (::pread(fd, &eh, sizeof(eh), 0) != static_cast<ssize_t>(sizeof(eh)) ||
            std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_shoff == 0 ||
            eh.e_shnum == 0 || eh.e_shentsize != sizeof(ElfW(Shdr))) {
            ::close(fd); return;
        }
        const size_t table_bytes = static_cast<size_t>(eh.e_shnum) * sizeof(ElfW(Shdr));
        if (!detail::in_range(eh.e_shoff, table_bytes, fsize)) { ::close(fd); return; }

        std::vector<ElfW(Shdr)> sh(eh.e_shnum);
        if (::pread(fd, sh.data(), table_bytes, static_cast<off_t>(eh.e_shoff)) !=
            static_cast<ssize_t>(table_bytes)) {
            ::close(fd); return;
        }

        size_t off = 0, size = 0;
        for (const ElfW(Shdr)& s : sh) {
            if (s.sh_type != SHT_PROGBITS || s.sh_size == 0) continue;
            if (!detail::in_range(s.sh_offset, s.sh_size, fsize)) continue;
            // The section name lives in shstrtab; rather than read a third
            // table, identify it by magic. An xz stream starts with
            // fd 37 7a 58 5a 00 and nothing else in an ELF does.
            unsigned char magic[6] {};
            if (::pread(fd, magic, sizeof(magic), static_cast<off_t>(s.sh_offset)) !=
                static_cast<ssize_t>(sizeof(magic))) continue;
            if (std::memcmp(magic, "\xfd" "7zXZ\0", 6) != 0) continue;
            off = static_cast<size_t>(s.sh_offset);
            size = static_cast<size_t>(s.sh_size);
            break;
        }
        if (size == 0) { ::close(fd); return; }

        std::vector<unsigned char> xz(size);
        if (::pread(fd, xz.data(), size, static_cast<off_t>(off)) !=
            static_cast<ssize_t>(size)) {
            ::close(fd); return;
        }
        ::close(fd);

        std::vector<unsigned char> elf;
        if (!xz_decompress(xz, elf) || elf.size() < sizeof(ElfW(Ehdr))) return;
        load_mini_elf(elf);
    }

    /// Decompresses an xz container into `out`.
    ///
    /// Uses the platform liblzma that ART itself already links against
    /// (DT_NEEDED liblzma.so), resolved through this same ELF walker rather
    /// than dlopen(): an app process' default namespace does not expose
    /// /apex/com.android.art/lib64, and /system/lib64/liblzma.so is not on the
    /// public-libraries list, so a dlopen() there can fail where this cannot.
    ///
    /// Only LZMA2-only, single-stream containers are handled, which is what
    /// Android produces and what `.gnu_debugdata` always is. Anything else is
    /// reported as failure (silently), never as a wrong answer.
    static bool xz_decompress(const std::vector<unsigned char>& in,
                              std::vector<unsigned char>& out) {
        // --- container header: 6 magic, 2 flags, 4 crc32 ---
        if (in.size() < 24) return false;
        static const unsigned char kMagic[6] = {0xfd, '7', 'z', 'X', 'Z', 0x00};
        if (std::memcmp(in.data(), kMagic, 6) != 0) return false;
        if (in[6] != 0x00) return false;                 // stream flags must be 0

        const unsigned check = in[7] & 0x0f;
        static const unsigned kCheckSize[16] = {0, 4, 4, 4, 8, 8, 8, 8,
                                                32, 32, 32, 32, 64, 64, 64, 64};
        const size_t check_sz = kCheckSize[check];

        using Lzma2Decode = int (*)(unsigned char*, size_t*, const unsigned char*,
                                    size_t*, unsigned char, int, int*, void*);
        // ISzAlloc: two function pointers. Both must be plain C functions.
        struct Alloc {
            static void* AllocFn(void*, size_t n) { return std::malloc(n); }
            static void  FreeFn(void*, void* p) { std::free(p); }
        };
        struct IszAlloc { void* (*alloc)(void*, size_t); void (*free)(void*, void*); };
        static IszAlloc alloc = {&Alloc::AllocFn, &Alloc::FreeFn};

        auto decode = reinterpret_cast<Lzma2Decode>(lzma_decode_entry());
        if (!decode) return false;

        out.clear();
        out.reserve(1u << 20);

        size_t pos = 12;
        int blocks = 0;
        while (pos + 12 <= in.size()) {
            if (in[pos] == 0) break;                      // index: end of blocks
            const size_t hsz = (static_cast<size_t>(in[pos]) + 1) * 4;
            if (pos + hsz > in.size()) return false;
            const unsigned char flags = in[pos + 1];
            if (flags & 0x3c) return false;               // unknown/reserved bits

            size_t q = pos + 2;
            unsigned char prop = 0;
            const int nfilters = (flags & 0x03) + 1;
            for (int i = 0; i < nfilters; ++i) {
                const long fid = vli(in, q);
                const long psz = vli(in, q);
                if (fid != 0x21 || psz != 1 || q + 1 > in.size()) return false;
                prop = in[q];                             // LZMA2 dictionary prop
                q += static_cast<size_t>(psz);
            }

            const size_t data = pos + hsz;
            if (data >= in.size()) return false;
            const size_t room = SBXELF_MAX_DEBUGDATA - out.size();
            if (room == 0) return false;
            size_t produced = room;
            size_t consumed = in.size() - data;           // in: bytes available
            int status = 0;
            out.resize(out.size() + room);
            const int res = decode(out.data() + out.size() - room, &produced,
                                   in.data() + data, &consumed, prop, 0, &status,
                                   &alloc);
            // `produced` is written by the decoder. Clamp it before the resize:
            // a decoder that reports more than the room it was given would
            // otherwise grow the buffer past SBXELF_MAX_DEBUGDATA.
            if (produced > room) produced = room;
            out.resize(out.size() - room + produced);     // keep only what landed
            if (res != 0) return false;

            // `consumed` is an OUT parameter here, not a remaining count: it
            // comes back as the number of input bytes the decoder ate. Reading
            // it as "remaining" walks into the middle of the next block's data
            // and the stream desynchronises one block in.
            pos = data + consumed;
            pos = (pos + 3) & ~static_cast<size_t>(3);    // 4-byte alignment
            pos += check_sz;                              // block check field
            if (++blocks > 4096) return false;
        }
        // The caller feeds this to the mini-ELF parser, so only a real ELF
        // image counts as success: a stream that decompresses to something
        // else is a wrong answer, not a partial one.
        if (out.size() < sizeof(ElfW(Ehdr))) return false;
        ElfW(Ehdr) meh;
        std::memcpy(&meh, out.data(), sizeof(meh));
        if (std::memcmp(meh.e_ident, ELFMAG, SELFMAG) != 0) return false;
        return true;
    }

    /// Resolves Lzma2Decode in whichever liblzma is actually mapped.
    ///
    /// Two different libraries carry that name and they are not interchangeable:
    /// Android's xz-embedded fork (libart's own DT_NEEDED liblzma.so) exports
    /// the one-shot `Lzma2Decode`, while a full liblzma — the public one in
    /// /system/lib64 on some images, and any host build — does not. Probe the
    /// loaded images in order and take the first that has it.
    static void* lzma_decode_entry() {
        static void* cached = nullptr;
        static bool  probed = false;
        if (probed) return cached;
        probed = true;
        // Try the copies that actually ship the xz-embedded entry point, most
        // specific first. "liblzma.so" alone can land on a full liblzma (the
        // public one on some images, and any host build) that has no
        // Lzma2Decode at all, so a bare substring match is not enough.
        static const char* const kNeedles[] = {
            "art/lib64/liblzma.so",      // apex copy, the one libart links
            "system/lib64/liblzma.so",   // platform copy on most ROMs
            "liblzma.so",
        };
        for (const char* needle : kNeedles) {
            Elf lzma(needle, /*want_debugdata=*/false);
            if (void* p = lzma.getSymbAddress("Lzma2Decode")) { cached = p; break; }
        }
        return cached;
    }

    /// Reads a variable-length integer (xz's own encoding) and advances `pos`.
    static long vli(const std::vector<unsigned char>& b, size_t& pos) {
        unsigned long v = 0;
        unsigned shift = 0;
        while (pos < b.size()) {
            const unsigned char c = b[pos++];
            v |= static_cast<unsigned long>(c & 0x7f) << shift;
            shift += 7;
            if ((c & 0x80) == 0) break;
            if (shift > 63) return -1;
        }
        return static_cast<long>(v);
    }

    /// Parses the decompressed mini-ELF's .symtab.
    void load_mini_elf(const std::vector<unsigned char>& elf) {
        if (elf.size() < sizeof(ElfW(Ehdr))) return;
        ElfW(Ehdr) eh;
        std::memcpy(&eh, elf.data(), sizeof(eh));
        if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return;
        if (eh.e_shnum == 0 || eh.e_shentsize != sizeof(ElfW(Shdr))) return;
        // `e_shoff > size` before the sum: adding a near-SIZE_MAX offset to the
        // table size wraps and the check passes, after which the loop would read
        // from elf.data() + that offset. The decompressed image is data, not a
        // trusted pointer, so bound it directly.
        if (!detail::in_range(eh.e_shoff,
                      static_cast<uint64_t>(eh.e_shnum) * sizeof(ElfW(Shdr)),
                      elf.size()))
            return;

        const bool pic = (eh.e_type == ET_DYN);
        for (size_t i = 0; i < eh.e_shnum; ++i) {
            ElfW(Shdr) s;
            std::memcpy(&s, elf.data() + eh.e_shoff + i * sizeof(s), sizeof(s));
            if (s.sh_type != SHT_SYMTAB || s.sh_link >= eh.e_shnum || s.sh_size == 0)
                continue;
            if (s.sh_entsize != sizeof(ElfW(Sym))) continue;
            if (!detail::in_range(s.sh_offset, s.sh_size, elf.size())) continue;

            ElfW(Shdr) str;
            std::memcpy(&str, elf.data() + eh.e_shoff + s.sh_link * sizeof(str), sizeof(str));
            if (str.sh_type != SHT_STRTAB ||
                !detail::in_range(str.sh_offset, str.sh_size, elf.size()))
                continue;

            const char* names = reinterpret_cast<const char*>(elf.data() + str.sh_offset);
            const size_t n = s.sh_size / sizeof(ElfW(Sym));
            for (size_t k = 1; k < n; ++k) {
                ElfW(Sym) sym;
                std::memcpy(&sym, elf.data() + s.sh_offset + k * sizeof(sym), sizeof(sym));
                if (sym.st_name == 0 || sym.st_name >= str.sh_size) continue;
                if (sym.st_shndx == SHN_UNDEF || sym.st_value == 0) continue;
                const char* name = names + sym.st_name;
                if (std::memchr(name, '\0', str.sh_size - sym.st_name) == nullptr) continue;
                // The mini-ELF describes the real image, so its st_value is the
                // main library's link-time address, not an offset into `elf`.
                const uintptr_t addr = pic ? base_ + sym.st_value : sym.st_value;
                add(std::string_view(name), reinterpret_cast<void*>(addr));
            }
            return;   // one .symtab per image
        }
    }

    const char* needle_ = "";
    bool        loaded_ = false;
    bool        want_debugdata_ = true;
    uintptr_t   base_   = 0;
    std::string path_;
    std::unordered_map<std::string, void*> symbols_;
    std::vector<std::pair<std::string, void*>> order_;
};

}  // namespace sbxelf
