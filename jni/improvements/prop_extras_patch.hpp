// prop_extras_patch.hpp
//
// Two one-liner fixes for L2 gaps observed in the v1.0.27 boot log:
//
//   L2 LEAK: persist.radio.multisim.config -> 'dsds' (real value returned)
//   L2 MISS: telephony.active_modems.max_count (no spoof, defaults leak)
//
// Include this header AFTER g_id and static_defaults are declared in main.cpp
// (or wherever the property maps live) and call ttfix::install_prop_extras()
// during init, before install_prop_hooks() runs.
//
// Values:
//   persist.radio.multisim.config = "ss"   (single-sim; matches SIM operator
//                                            row from L2 SPOOF: gsm.sim.operator.numeric=51010)
//   telephony.active_modems.max_count = "1"

#pragma once

#include <string>
#include <unordered_map>

namespace ttfix {

// Signature-compatible with the g_id / static_defaults maps in main.cpp.
template <typename IdMap, typename DefMap>
inline void install_prop_extras(IdMap& g_id, DefMap& static_defaults) {
    // Per-identity spoof (moves with device — goes into g_id).
    g_id["persist.radio.multisim.config"] = "ss";

    // Global truth (never varies per identity — goes into static_defaults).
    static_defaults["telephony.active_modems.max_count"] = "1";

    // Optional: also blank out other common leak keys observed via TT_DEBUG on
    // similar builds. Safe defaults, uncomment if boot log confirms leaks:
    // static_defaults["ro.telephony.default_network"] = "9";
    // static_defaults["persist.radio.aosp_usb_workaround"] = "1";
}

} // namespace ttfix
