// Verifies the two symbol-resolution behaviours L3 depends on, without a
// device: the mini-ELF .symtab parser behind .gnu_debugdata support, and the
// `.__uniq.<hash>` suffix tolerance that a stripped LTO ROM requires.
//
// The fixture is a synthetic mini-ELF built here, so the suite runs on any
// host. The xz container itself is exercised separately — pass a real
// .gnu_debugdata blob as argv[1] to also check the decompressor end to end
// (e.g. `busybox unxz` output from a device's libart.so section).
//
// Why these two behaviours matter: LSPlant asks libart for exact mangled names.
// On a release ROM built with LTO those names carry a `.__uniq.<hash>` tail, so
// an exact lookup misses and a plain prefix lookup also misses (the caller's
// prefix already carries a different trailing type signature). Both failures
// show up as `Failed to find GetMethodShorty` and disable the whole L3 layer.

#include "sbx_elf.hpp"

#include <cstdio>
#include <cstring>
#include <climits>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

// ---- a minimal ELF64 image: ehdr + .symtab + .strtab + section headers ------
//
// Layout (offsets chosen so nothing overlaps):
//   0x000  Ehdr (64)
//   0x040  symtab entries (3 * 24)
//   0x088  strtab
//   ...    two section headers
struct MiniElf {
    std::vector<unsigned char> bytes;

    // (name_index, value): 1 = the plain bridge name, 2 = the __uniq-suffixed
    // GetMethodShorty name. Indices into the string table built below.
    explicit MiniElf(const std::vector<std::pair<uint32_t, uint64_t>>& syms) {
        const std::string names[] = {"", "art_quick_to_interpreter_bridge",
                                     "_ZN3artL15GetMethodShortyEP7_JNIEnvP10_jmethodID.__uniq.123"};

        // strtab: "" then each name, NUL separated
        std::string str;
        std::vector<uint32_t> name_off;
        for (const auto& n : names) {
            name_off.push_back(static_cast<uint32_t>(str.size()));
            str += n;
            str.push_back('\0');
        }

        const size_t ehdr_sz = sizeof(Elf64_Ehdr);
        const size_t sym_off = ehdr_sz;
        const size_t sym_cnt = syms.size() + 1;   // [0] is the null symbol
        const size_t sym_sz = sym_cnt * sizeof(Elf64_Sym);
        const size_t str_off = sym_off + sym_sz;
        const size_t sh_off = str_off + str.size();
        const size_t sh_cnt = 3;
        const size_t total = sh_off + sh_cnt * sizeof(Elf64_Shdr);

        bytes.assign(total, 0);

        Elf64_Ehdr eh {};
        std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
        eh.e_ident[EI_CLASS] = ELFCLASS64;
        eh.e_type = ET_DYN;            // st_value is relative to a load bias
        eh.e_shentsize = sizeof(Elf64_Shdr);
        eh.e_shnum = static_cast<Elf64_Half>(sh_cnt);
        eh.e_shoff = sh_off;
        std::memcpy(bytes.data(), &eh, sizeof(eh));

        Elf64_Sym* sy = reinterpret_cast<Elf64_Sym*>(bytes.data() + sym_off);
        for (size_t i = 0; i < syms.size(); ++i) {
            sy[i + 1].st_name = name_off[syms[i].first];
            sy[i + 1].st_value = syms[i].second;
            sy[i + 1].st_shndx = 1;    // defined
        }

        std::memcpy(bytes.data() + str_off, str.data(), str.size());

        Elf64_Shdr* sh = reinterpret_cast<Elf64_Shdr*>(bytes.data() + sh_off);
        sh[1].sh_type = SHT_SYMTAB;
        sh[1].sh_offset = sym_off;
        sh[1].sh_size = sym_sz;
        sh[1].sh_entsize = sizeof(Elf64_Sym);
        sh[1].sh_link = 2;             // -> strtab
        sh[2].sh_type = SHT_STRTAB;
        sh[2].sh_offset = str_off;
        sh[2].sh_size = str.size();
    }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // ---- mini-ELF .symtab parsing ----
    std::printf("==> mini-ELF .symtab parser\n");
    MiniElf fixture({{1, 0x288350}, {2, 0x91a8bc}});

    sbxelf::Elf art("definitely-not-a-loaded-library");
    art.load_mini_elf_public(fixture.bytes);

    CHECK(art.symbol_count() >= 2);
    CHECK(art.getSymbAddress("art_quick_to_interpreter_bridge") != nullptr);
    CHECK(art.getSymbAddress("art_quick_to_interpreter") == nullptr);

    // ---- .__uniq suffix tolerance ----
    std::printf("==> .__uniq suffix tolerance\n");
    const char* shorty = "_ZN3artL15GetMethodShortyEP7_JNIEnvP10_jmethodID";
    CHECK(art.getSymbAddress(shorty) == nullptr);          // exact lookup misses
    CHECK(art.getSymbAddressOrUniq(shorty) != nullptr);    // suffix-aware finds it

    // An exact name must still resolve to itself, not to a suffixed sibling.
    void* exact = art.getSymbAddress("art_quick_to_interpreter_bridge");
    CHECK(exact != nullptr);
    CHECK(art.getSymbAddressOrUniq("art_quick_to_interpreter_bridge") == exact);

    // A name that is only a PREFIX of a different symbol must not match: the
    // tail there is not a toolchain suffix, it is part of the symbol.
    CHECK(art.getSymbAddressOrUniq("art_quick_to_interpreter") == nullptr);
    CHECK(art.getSymbAddressOrUniq("_ZN9NoSuchThingEv") == nullptr);
    CHECK(art.getSymbAddressOrUniq("") == nullptr);

    // ---- xz container (optional, needs a real blob) ----
    if (argc > 1) {
        std::printf("==> xz decompressor on %s\n", argv[1]);
        // In an app process libart's DT_NEEDED liblzma is already mapped and
        // carries Lzma2Decode. A bare shell has no reason to have it, so a
        // missing decompressor is a skip here, not a failure — the parsing
        // checks above already ran against the synthetic fixture.
        if (!sbxelf::Elf::xz_decompressor_available()) {
            std::printf("  SKIP no mapped liblzma with Lzma2Decode "
                        "(run inside an app process, or dlopen it first)\n");
        } else {
            FILE* f = std::fopen(argv[1], "rb");
            if (!f) {
                std::printf("  FAIL cannot open %s\n", argv[1]);
                ++failures;
            } else {
                std::fseek(f, 0, SEEK_END);
                long n = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                std::vector<unsigned char> in(n > 0 ? static_cast<size_t>(n) : 0);
                const bool read_ok =
                    in.empty() || std::fread(in.data(), 1, in.size(), f) == in.size();
                std::fclose(f);

                std::vector<unsigned char> out;
                CHECK(read_ok);
                CHECK(sbxelf::Elf::xz_decompress_public(in, out));
                CHECK(out.size() > 64);
                CHECK(out.size() >= 4 && std::memcmp(out.data(), ELFMAG, SELFMAG) == 0);
                if (!out.empty()) {
                    sbxelf::Elf real("definitely-not-a-loaded-library");
                    real.load_mini_elf_public(out);
                    std::printf("  %zu bytes -> %zu symbols\n", out.size(), real.symbol_count());
                    CHECK(real.symbol_count() > 1000);
                }
            }
        }
    } else {
        std::printf("==> xz decompressor skipped (pass a .gnu_debugdata blob as argv[1])\n");
    }

    // Malformed input must be refused, never crash.
    std::printf("==> malformed input is refused\n");
    {
        std::vector<unsigned char> junk(64, 0xAB), sink;
        CHECK(!sbxelf::Elf::xz_decompress_public(junk, sink));
        std::vector<unsigned char> empty;
        CHECK(!sbxelf::Elf::xz_decompress_public(empty, sink));
        sbxelf::Elf bad("definitely-not-a-loaded-library");
        bad.load_mini_elf_public(empty);
        CHECK(bad.symbol_count() == 0);
    }

    // A section-header offset near the top of the address space must be refused,
    // not wrapped: `e_shoff + table_bytes > size` is false after the wrap, and
    // the parser would then read from elf.data() + e_shoff.
    std::printf("==> wrapping section offset is refused\n");
    {
        MiniElf wrap({{1, 0x288350}});
        Elf64_Ehdr* we = reinterpret_cast<Elf64_Ehdr*>(wrap.bytes.data());
        we->e_shoff = ~static_cast<uint64_t>(0) - sizeof(Elf64_Shdr) + 1;
        sbxelf::Elf e("definitely-not-a-loaded-library");
        e.load_mini_elf_public(wrap.bytes);
        CHECK(e.symbol_count() == 0);
    }

    if (failures == 0) std::printf("all checks passed\n");
    else std::printf("%d check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
