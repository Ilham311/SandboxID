// secure_xml_gaid.hpp
// v2.1.2: Deprecated stub — functionality merged into secure_xml_template.hpp.
//
// Historically this file contained a duplicate build_secure_xml() with the
// GAID surface. In v2.1.2 the unified XML template lives in
// secure_xml_template.hpp (namespace tt::). This file is kept only so any
// pre-existing #include directive continues to compile.
//
// Kernel-level fix in this rewrite:
//   - Removed the broken call to ttfix::urandom_bytes() (function did not exist).
//   - Renamed remaining helpers from ttfix:: to tt:: for namespace consistency.
//   - Delegated all logic to secure_xml_template.hpp + random_util.hpp.

#pragma once

#include <string>
#include "random_util.hpp"          // provides tt::uuid_v4 / tt::urandom_fill
#include "secure_xml_template.hpp"  // provides tt::build_secure_xml

namespace tt {

// GAID = UUID v4, lower-case, hyphenated. Wrapper for backward compatibility.
inline std::string gen_gaid_uuid() {
    return uuid_v4();
}

}  // namespace tt
