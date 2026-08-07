// prop_extras_patch.hpp
// v2.1.2: renamed namespace ttfix:: -> tt::.
//
// Two one-liner fixes for L2 gaps observed in the v1.0.27 boot log:
//
//   L2 LEAK: persist.radio.multisim.config -> 'dsds' (real value returned)
//   L2 MISS: telephony.active_modems.max_count (no spoof, defaults leak)
//
// Call tt::install_prop_extras() during init, before install_prop_hooks() runs.
// NOTE: This is dead-code by default in v2.1.x (the same rows are set inline in
// main.cpp). Keep for future refactor where prop maps get externalized.

#pragma once

#include <string>
#include <unordered_map>

namespace tt {

template <typename IdMap, typename DefMap>
inline void install_prop_extras(IdMap& g_id, DefMap& static_defaults) {
    g_id["persist.radio.multisim.config"] = "ss";
    static_defaults["telephony.active_modems.max_count"] = "1";
}

}  // namespace tt
