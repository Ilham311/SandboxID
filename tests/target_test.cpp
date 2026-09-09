#include "../jni/sbx_target.hpp"

#include <cstdio>
#include <string>

static int checks = 0;
static int failures = 0;

#define CHECK(condition, message) do {                                    \
    ++checks;                                                             \
    if (!(condition)) {                                                   \
        ++failures;                                                       \
        std::printf("FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
    }                                                                     \
} while (0)

static void test_parse_and_normalize() {
    const std::string text =
        "  com.example.app  # primary\n"
        "com.example.app:worker\n"
        "com.example.app:worker\n"
        "org.demo.client\r\n"
        "# ignored\n";
    sbxtarget::TargetSet targets;
    std::string error;
    CHECK(sbxtarget::parse(text, targets, error), "valid targets parse");
    CHECK(targets.processes.size() == 3, "exact processes deduplicate");
    CHECK(targets.processes[1] == "com.example.app:worker",
          "secondary process preserved exactly");
    CHECK(targets.packages.size() == 2, "base packages deduplicate");
    CHECK(targets.packages[0] == "com.example.app" &&
              targets.packages[1] == "org.demo.client",
          "base package order preserved");
}

static void test_invalid_targets() {
    sbxtarget::TargetSet targets;
    std::string error;
    CHECK(!sbxtarget::parse("com.example.app:one:two\n", targets, error),
          "multiple process separators rejected");
    CHECK(!sbxtarget::parse("com.example.bad-package\n", targets, error),
          "invalid package character rejected");
    CHECK(!sbxtarget::parse("single_segment\n", targets, error),
          "undotted package rejected");
    CHECK(!sbxtarget::parse("com.1invalid.app\n", targets, error),
          "numeric segment prefix rejected");
    CHECK(sbxtarget::parse("\n# empty list\n", targets, error) &&
              targets.processes.empty() && targets.packages.empty(),
          "intentional empty target list remains valid");
}

int main() {
    test_parse_and_normalize();
    test_invalid_targets();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
