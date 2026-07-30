#include <iostream>
#include <cassert>
#include "proc_utils.hpp"

void test_null_pointer() {
    assert(is_sensitive_proc_path(nullptr) == false);
}

void test_non_proc_paths() {
    assert(is_sensitive_proc_path("/etc/hosts") == false);
    assert(is_sensitive_proc_path("/data/local/tmp/mountinfo") == false);
    assert(is_sensitive_proc_path("mountinfo") == false);
    assert(is_sensitive_proc_path("/proc") == false); // doesn't end with /
}

void test_non_sensitive_proc_paths() {
    assert(is_sensitive_proc_path("/proc/cpuinfo") == false);
    assert(is_sensitive_proc_path("/proc/meminfo") == false);
    assert(is_sensitive_proc_path("/proc/sys/kernel/random/uuid") == false);
}

void test_sensitive_proc_paths() {
    assert(is_sensitive_proc_path("/proc/mountinfo") == true);
    assert(is_sensitive_proc_path("/proc/self/mountinfo") == true);
    assert(is_sensitive_proc_path("/proc/1234/mountinfo") == true);

    assert(is_sensitive_proc_path("/proc/mounts") == true);
    assert(is_sensitive_proc_path("/proc/self/mounts") == true);

    assert(is_sensitive_proc_path("/proc/maps") == true);
    assert(is_sensitive_proc_path("/proc/self/maps") == true);
}

int main() {
    std::cout << "Running tests for is_sensitive_proc_path..." << std::endl;

    test_null_pointer();
    test_non_proc_paths();
    test_non_sensitive_proc_paths();
    test_sensitive_proc_paths();

    std::cout << "All tests passed!" << std::endl;
    return 0;
}
