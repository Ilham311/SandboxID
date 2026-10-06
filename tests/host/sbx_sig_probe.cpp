// Reproduces the double-install scenario against the REAL install_crash_watchdog()
// / sbx_crash_handler() from jni/module_hooks.cpp (included, not copied).
//
// alarm() bounds the run: if the handler chains to itself forever it never
// returns and never re-raises, so the SIGALRM is what tells us it hung.
#include "module_hooks.cpp"

#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

// ---- stubs for the parts that live in other TUs (same set as sbx_hook_sm) ----
std::map<std::string, std::string> g_identity;
std::string g_pkg = "com.test.app";
bool spoof_prop_value(const std::string&, std::string&) { return false; }
extern "C" int __android_log_print(int, const char*, const char*, ...) { return 0; }
extern "C" int __android_log_write(int, const char*, const char*) { return 0; }
const std::string empty_val;
const std::string& val(const std::string& k) {
    auto it = g_identity.find(k);
    return it == g_identity.end() ? empty_val : it->second;
}

static void on_alarm(int) { _exit(42); }   // 42 == "still spinning"

int main() {
    const pid_t pid = fork();
    if (pid == 0) {
        struct sigaction al{};
        al.sa_handler = on_alarm;
        sigemptyset(&al.sa_mask);
        sigaction(SIGALRM, &al, nullptr);
        alarm(3);

        install_crash_watchdog("probe.once");
        install_crash_watchdog("probe.twice");   // the double-install

        // A genuine fatal fault.
        volatile int* p = nullptr;
        *p = 1;
        _exit(99);                               // never reached if handler is sane
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        printf("RESULT: killed by signal %d (handler delegated correctly)\n", WTERMSIG(st));
        return 0;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
        printf("RESULT: HUNG - handler chained to itself; alarm had to end it\n");
        return 1;
    }
    printf("RESULT: exit %d (unexpected)\n", WEXITSTATUS(st));
    return 1;
}
