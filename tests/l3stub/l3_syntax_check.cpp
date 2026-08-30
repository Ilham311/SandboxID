// L3 syntax-check translation unit. Including sbx_lsplant.hpp under
// -DSBX_ENABLE_LSPLANT parses the entire LSPlant hook body (and, transitively,
// lsparself.hpp's .gnu_debugdata/xz path) against the stub headers in this
// directory. Compiled with -fsyntax-only by tools/validate.sh — never linked.
#include "sbx_lsplant.hpp"

int main() {
    return sbxlsp::available() ? 0 : 1;
}
