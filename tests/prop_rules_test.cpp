#include <iostream>
#include <string>
#include <vector>
#include <cassert>

// Include the implementation we wrote in main.cpp for testing
#include "../jni/main.cpp"

void test_resolve_prop() {
    std::string out;
    bool found;

    // Test exact match wins over prefix match
    // ro.product.mod_device is an exact match mapped to identity DEVICE
    // We set up a mock g_id in our host stub.
    g_id["DEVICE"] = "mock_device";
    found = resolve_prop("ro.product.mod_device", PropValueKind::Str, out);
    assert(found);
    assert(out == "mock_device");

    // Test prefix rules work
    found = resolve_prop("ro.miui.ui.font.mi_font_path", PropValueKind::Str, out);
    assert(found);
    assert(out == "");

    // Suppressed (empty-value) prefix rules must report unresolved for
    // int/long/bool so callers keep the app's own default instead of a
    // fabricated "0"/false.
    found = resolve_prop("persist.sys.miui_optimization", PropValueKind::Int, out);
    assert(!found);

    found = resolve_prop("persist.sys.turbosched.some_key", PropValueKind::Bool, out);
    assert(!found);

    // Test unknown keys fall through
    found = resolve_prop("ro.unknown.key", PropValueKind::Str, out);
    assert(!found);

    std::cout << "All prop_rules_test assertions passed." << std::endl;
}

int main() {
    test_resolve_prop();
    return 0;
}
