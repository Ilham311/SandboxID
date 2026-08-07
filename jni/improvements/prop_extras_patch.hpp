

#pragma once

#include <string>
#include <unordered_map>

namespace tt {

template <typename IdMap, typename DefMap>
inline void install_prop_extras(IdMap& g_id, DefMap& static_defaults) {

    g_id["persist.radio.multisim.config"] = "ss";

    static_defaults["telephony.active_modems.max_count"] = "1";

}

}
