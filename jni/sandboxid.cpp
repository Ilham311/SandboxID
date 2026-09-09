
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <fstream>
#include <sstream>
#include <random>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "config.hpp"
#include "sbx_carrier.hpp"
#include "sbx_identity.hpp"
#include "sbx_native_read.hpp"
#include "sbx_persona.hpp"
#include "sbx_property.hpp"
#include "sbx_sha256.hpp"
#include "sbx_target.hpp"
#include "sbx_transaction.hpp"
#include <sys/system_properties.h>

static const char* IDENTITY_FILE  = sandboxid::IDENTITY_FILE;
static const char* IDENTITY_BAK   = sandboxid::IDENTITY_BAK;
static const char* IDENTITY_META  = sandboxid::IDENTITY_META;
static const char* IDENTITY_META_BAK = sandboxid::IDENTITY_META_BAK;
static const char* MODE_FILE      = sandboxid::MODE_FILE;
static const char* RESETPROP      = sandboxid::RESETPROP;
static const char* MOUNTDIR       = sandboxid::MOUNTDIR;
static const char* TARGET_FILE    = sandboxid::TARGET_FILE;
static const char* PERSONAS_FILE  = sandboxid::PERSONAS_FILE;
static const char* PERSONA_OVERRIDE = sandboxid::PERSONA_OVERRIDE;
static const char* PERSONA_CACHE = sandboxid::PERSONA_CACHE;
static const char* PERSONA_CACHE_META = sandboxid::PERSONA_CACHE_META;
static const char* IDENTITY_PENDING = sandboxid::IDENTITY_PENDING;
static const char* PENDING_META = sandboxid::PENDING_META;
static const char* STATE_LOCK = sandboxid::STATE_LOCK;
static const char* MUTATION_LOCK = sandboxid::MUTATION_LOCK;
static const char* MUTATION_OWNER = sandboxid::MUTATION_OWNER;
static const char* ACTION_OWNER = sandboxid::ACTION_OWNER;
static const char* ACTION_STATE = sandboxid::ACTION_STATE;
static const char* ACTION_RESULT = sandboxid::ACTION_RESULT;
static const char* CARRIER_CONF   = sandboxid::CARRIER_CONF;
static const char* LEGACY_SETTINGS_OVERLAY = sandboxid::LEGACY_SETTINGS_OVERLAY;

static bool load_target_set(sbxtarget::TargetSet& out, std::string& error) {
    std::ifstream file(TARGET_FILE, std::ios::binary);
    if (!file) {
        error = std::string(TARGET_FILE) + " is unreadable";
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        error = std::string(TARGET_FILE) + " could not be read completely";
        return false;
    }
    return sbxtarget::parse(buffer.str(), out, error);
}

static std::string random_hex(int bytes, bool upper) {
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ (uint64_t)std::chrono::steady_clock::now()
                              .time_since_epoch().count());
    std::uniform_int_distribution<int> d(0, 15);
    const char* al = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    s.reserve(bytes * 2);
    for (int i = 0; i < bytes * 2; ++i) s.push_back(al[d(gen)]);
    return s;
}

static bool close_checked(int fd) {
    return ::close(fd) == 0;
}

static bool fsync_checked(int fd) {
    while (::fsync(fd) != 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

static bool fsync_directory(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync_checked(fd);
    if (!close_checked(fd)) ok = false;
    return ok;
}

static std::string parent_directory(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." :
           (slash == 0 ? "/" : path.substr(0, slash));
}

static bool atomic_write(const std::string& p, const std::string& data) {
    std::string tmp = p + ".tmp." + std::to_string((long)::getpid());
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t off = 0; off < data.size();) {
        ssize_t written = ::write(fd, data.data() + off, data.size() - off);
        if (written < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (written == 0) {
            ok = false;
            break;
        }
        off += static_cast<size_t>(written);
    }
    if (ok && ::fchmod(fd, 0644) != 0) ok = false;
    if (ok && !fsync_checked(fd)) ok = false;
    if (!close_checked(fd)) ok = false;
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), p.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    const std::string dir = parent_directory(p);
    return fsync_directory(dir);
}

static std::string read_file(const std::string& p);
static bool path_exists(const std::string& path);

static bool read_proc_stat(pid_t pid, uint64_t& parent, uint64_t& start) {
    std::ifstream input("/proc/" + std::to_string(static_cast<long>(pid)) + "/stat");
    std::string line;
    if (!input || !std::getline(input, line)) return false;
    const size_t close = line.rfind(") ");
    if (close == std::string::npos) return false;
    std::istringstream fields(line.substr(close + 2));
    std::string value;
    for (int index = 1; index <= 20; ++index) {
        if (!(fields >> value)) return false;
        if (index == 2) {
            if (!sbxtxn::decimal_u64(value, parent)) return false;
        } else if (index == 20) {
            if (!sbxtxn::decimal_u64(value, start)) return false;
        }
    }
    return parent > 0 && start > 0;
}

static bool process_descends_from(pid_t child, uint64_t owner) {
    uint64_t current = static_cast<uint64_t>(child);
    for (int depth = 0; depth < 64 && current > 0; ++depth) {
        if (current == owner) return true;
        uint64_t parent = 0, start = 0;
        if (!read_proc_stat(static_cast<pid_t>(current), parent, start) ||
            parent == current) return false;
        current = parent;
    }
    return false;
}

static bool environment_matches(const char* name, const std::string& value) {
    const char* actual = ::getenv(name);
    return actual && value == actual;
}

static bool action_owner_matches(const sbxtxn::MutationOwner& mutation,
                                 std::string& error) {
    if (mutation.kind != "action" || mutation.run.empty()) {
        error = "mutation owner is not an Action";
        return false;
    }
    sbxtxn::MutationOwner action;
    if (!sbxtxn::parse_mutation_owner(read_file(ACTION_OWNER), action, error))
        return false;
    if (action.kind != "action" || action.pid != mutation.pid ||
        action.proc_start != mutation.proc_start || action.run != mutation.run ||
        action.token != mutation.token) {
        error = "Action and mutation owner metadata disagree";
        return false;
    }
    return true;
}

struct MutationGuard {
    int fd = -1;

    bool acquire(std::string& error) {
        const std::string raw = read_file(MUTATION_OWNER);
        if (!raw.empty()) {
            sbxtxn::MutationOwner owner;
            if (!sbxtxn::parse_mutation_owner(raw, owner, error)) {
                error = "global mutation owner is incomplete or malformed";
                return false;
            }
            uint64_t parent = 0, start = 0;
            if (!read_proc_stat(static_cast<pid_t>(owner.pid), parent, start) ||
                start != owner.proc_start) {
                error = "global mutation owner is stale; shell must reclaim it";
                return false;
            }
            if ((owner.kind != "action" && owner.kind != "standalone") ||
                (owner.kind == "action" &&
                 !environment_matches("SBX_ACTION_RUN_ID", owner.run)) ||
                (owner.kind == "standalone" && !owner.run.empty()) ||
                !environment_matches("SBX_MUTATION_OWNER_PID", std::to_string(owner.pid)) ||
                !environment_matches("SBX_MUTATION_OWNER_START", std::to_string(owner.proc_start)) ||
                !environment_matches("SBX_MUTATION_OWNER_TOKEN", owner.token) ||
                !process_descends_from(::getpid(), owner.pid) ||
                (owner.kind == "action" && !action_owner_matches(owner, error))) {
                error = "global mutation domain is owned by another operation";
                return false;
            }
            return true;
        }
        if (::mkdir(MUTATION_LOCK, 0700) != 0) {
            error = errno == EEXIST ? "global mutation domain is busy" :
                    std::string("cannot acquire global mutation domain: ") +
                    std::strerror(errno);
            return false;
        }
        std::string token = random_hex(16, false);
        uint64_t parent = 0, start = 0;
        if (!read_proc_stat(::getpid(), parent, start)) {
            ::rmdir(MUTATION_LOCK);
            error = "cannot bind mutation owner to process start";
            return false;
        }
        const std::string owner = "version=1\nkind=standalone\npid=" +
            std::to_string(static_cast<long>(::getpid())) + "\nproc_start=" +
            std::to_string(start) + "\ntoken=" + token + "\n";
        if (!atomic_write(MUTATION_OWNER, owner)) {
            ::rmdir(MUTATION_LOCK);
            error = "cannot publish global mutation owner";
            return false;
        }
        fd = ::open(MUTATION_OWNER, O_RDONLY | O_CLOEXEC);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (fd >= 0) close_checked(fd);
            fd = -1;
            ::unlink(MUTATION_OWNER);
            ::rmdir(MUTATION_LOCK);
            error = "cannot hold global mutation owner";
            return false;
        }
        return true;
    }

    ~MutationGuard() {
        if (fd >= 0) {
            struct stat held{};
            struct stat current{};
            const bool same_owner = ::fstat(fd, &held) == 0 &&
                ::stat(MUTATION_OWNER, &current) == 0 &&
                held.st_dev == current.st_dev && held.st_ino == current.st_ino;
            ::flock(fd, LOCK_UN);
            close_checked(fd);
            if (same_owner) {
                ::unlink(MUTATION_OWNER);
                ::rmdir(MUTATION_LOCK);
                fsync_directory(parent_directory(MUTATION_LOCK));
            }
        }
    }
};

static bool require_action_writer(std::string& error) {
    sbxtxn::MutationOwner owner;
    if (!sbxtxn::parse_mutation_owner(read_file(MUTATION_OWNER), owner, error) ||
        !action_owner_matches(owner, error) ||
        !environment_matches("SBX_ACTION_RUN_ID", owner.run) ||
        !environment_matches("SBX_MUTATION_OWNER_PID", std::to_string(owner.pid)) ||
        !environment_matches("SBX_MUTATION_OWNER_START", std::to_string(owner.proc_start)) ||
        !environment_matches("SBX_MUTATION_OWNER_TOKEN", owner.token) ||
        !process_descends_from(::getpid(), owner.pid)) {
        if (error.empty()) error = "action-write caller is not the active Action owner";
        return false;
    }
    return true;
}

static int cmd_action_write(const char* kind, const char* source_path) {
    if (::geteuid() != 0) {
        fprintf(stderr, "! root required\n");
        return 1;
    }
    std::string authorization_error;
    if (!require_action_writer(authorization_error)) {
        fprintf(stderr, "! %s\n", authorization_error.c_str());
        return 75;
    }
    if (!kind || !source_path) {
        fprintf(stderr, "Usage: sandboxid action-write <state|result> <source>\n");
        return 64;
    }
    const char* destination = nullptr;
    if (!std::strcmp(kind, "state")) destination = ACTION_STATE;
    else if (!std::strcmp(kind, "result")) destination = ACTION_RESULT;
    else {
        fprintf(stderr, "! action-write kind must be state or result\n");
        return 64;
    }
    int fd = ::open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fprintf(stderr, "! action-write source could not be opened\n");
        return 64;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || status.st_size > 64 * 1024) {
        close_checked(fd);
        fprintf(stderr, "! action-write source is missing, empty, or oversized\n");
        return 64;
    }
    std::string content(static_cast<size_t>(status.st_size), '\0');
    const bool read_ok = sandboxid::read_full(fd, content.data(), content.size());
    const bool close_ok = close_checked(fd);
    if (!read_ok || !close_ok) {
        fprintf(stderr, "! action-write source could not be read completely\n");
        return 1;
    }
    if (!atomic_write(destination, content)) {
        fprintf(stderr, "! action-write durable publication failed\n");
        return 32;
    }
    return 0;
}

struct StateLock {
    int fd = -1;

    bool acquire(std::string& error) {
        fd = ::open(STATE_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            error = std::string("cannot open state lock: ") + std::strerror(errno);
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            error = errno == EWOULDBLOCK ? "another state mutation is active" :
                    std::string("cannot acquire state lock: ") + std::strerror(errno);
            close_checked(fd);
            fd = -1;
            return false;
        }
        return true;
    }

    ~StateLock() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            close_checked(fd);
        }
    }
};

static bool path_exists(const std::string& path) {
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0;
}

static bool remove_file_durable(const std::string& path) {
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) return false;
    return fsync_directory(parent_directory(path));
}

static bool remove_tree(const std::string& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode)) return ::unlink(path.c_str()) == 0;
    DIR* directory = ::opendir(path.c_str());
    if (!directory) return false;
    bool ok = true;
    errno = 0;
    while (dirent* entry = ::readdir(directory)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
            continue;
        if (!remove_tree(path + "/" + entry->d_name)) {
            ok = false;
            break;
        }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (::closedir(directory) != 0) ok = false;
    if (ok && ::rmdir(path.c_str()) != 0) ok = false;
    return ok;
}

static bool remove_tree_durable(const std::string& path) {
    if (!remove_tree(path)) return false;
    return fsync_directory(parent_directory(path));
}

static std::string read_file(const std::string& p);

static bool restore_path(const std::string& path, bool existed,
                         const std::string& content) {
    return existed ? atomic_write(path, content) : remove_file_durable(path);
}

enum class PairReplaceResult {
    Committed,
    FailedRestored,
    FailedDegraded,
};

static PairReplaceResult replace_pair(const std::string& first_path,
                                      const std::string& first_data,
                                      const std::string& second_path,
                                      const std::string& second_data) {
    struct stat first_status{};
    struct stat second_status{};
    const bool first_existed = ::stat(first_path.c_str(), &first_status) == 0;
    const bool second_existed = ::stat(second_path.c_str(), &second_status) == 0;
    const std::string old_first = first_existed ? read_file(first_path) : std::string();
    const std::string old_second = second_existed ? read_file(second_path) : std::string();
    if ((first_existed && old_first.empty() && first_status.st_size > 0) ||
        (second_existed && old_second.empty() && second_status.st_size > 0))
        return PairReplaceResult::FailedRestored;
    auto restore = [&]() {
        bool first_ok = restore_path(first_path, first_existed, old_first);
        bool second_ok = restore_path(second_path, second_existed, old_second);
        if (!first_ok || !second_ok)
            fprintf(stderr, "! failed to restore previous state pair after write failure\n");
        return first_ok && second_ok;
    };
    if (!atomic_write(first_path, first_data))
        return restore() ? PairReplaceResult::FailedRestored
                         : PairReplaceResult::FailedDegraded;
    if (atomic_write(second_path, second_data)) return PairReplaceResult::Committed;
    return restore() ? PairReplaceResult::FailedRestored
                     : PairReplaceResult::FailedDegraded;
}

static PairReplaceResult remove_pair_durable(const std::string& first_path,
                                              const std::string& second_path) {
    struct stat first_status{};
    struct stat second_status{};
    const bool first_existed = ::stat(first_path.c_str(), &first_status) == 0;
    const bool second_existed = ::stat(second_path.c_str(), &second_status) == 0;
    const std::string old_first = first_existed ? read_file(first_path) : std::string();
    const std::string old_second = second_existed ? read_file(second_path) : std::string();
    if ((first_existed && old_first.empty() && first_status.st_size > 0) ||
        (second_existed && old_second.empty() && second_status.st_size > 0))
        return PairReplaceResult::FailedRestored;
    auto restore = [&]() {
        bool first_ok = restore_path(first_path, first_existed, old_first);
        bool second_ok = restore_path(second_path, second_existed, old_second);
        if (!first_ok || !second_ok)
            fprintf(stderr, "! failed to restore previous state pair after removal failure\n");
        return first_ok && second_ok;
    };
    if (remove_file_durable(first_path) && remove_file_durable(second_path))
        return PairReplaceResult::Committed;
    return restore() ? PairReplaceResult::FailedRestored
                     : PairReplaceResult::FailedDegraded;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    size_t st = s.find_first_not_of(" \t");
    if (st != std::string::npos) s = s.substr(st);
    return s;
}

static int run_bin(const char* path, std::vector<const char*> argv, bool null_io = false) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (null_io) {
            int nul = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (nul >= 0) {
                dup2(nul, STDIN_FILENO);
                dup2(nul, STDOUT_FILENO);
                dup2(nul, STDERR_FILENO);
                if (nul > STDERR_FILENO) ::close(nul);
            }
        }
        argv.push_back(nullptr);
        execv(path, const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_bin_path(const char* file, std::vector<const char*> argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        argv.push_back(nullptr);
        execvp(file, const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_framework(const char* path, std::vector<const char*> argv,
                         const std::string& label) {
    const int attempts = 2;
    int rc = -1;
    for (int i = 0; i < attempts; ++i) {
        rc = run_bin(path, argv, true);
        if (rc == 0) return 0;
        if (i + 1 < attempts) ::usleep(200 * 1000);
    }
    fprintf(stderr, "! %s gagal (exit=%d) setelah %d percobaan — binder transaction ditolak (SELinux/FD)\n",
            label.c_str(), rc, attempts);
    return rc;
}

struct Identity {
    std::map<std::string, std::string> kv;

    std::string serialize() const {
        static constexpr std::string_view order[] = {
        "BRAND","MANUFACTURER","MODEL","MARKETNAME","DEVICE","PRODUCT",
        "BOARD","HARDWARE","BOARD_PLATFORM","SOC_MANUFACTURER","SOC_MODEL",
        "FINGERPRINT","ID","DISPLAY","DESCRIPTION",
        "BOOTLOADER","HOST","USER","TYPE","TAGS",
        "INCREMENTAL","RELEASE","SDK_INT","SECURITY_PATCH",
        "SERIAL","RADIO","ANDROID_ID","GOOGLE_AID",
        "GSM_OPERATOR_NUMERIC","GSM_OPERATOR_ALPHA","GSM_OPERATOR_ISO","GSM_SIM_STATE",
        "SKU","ODM_SKU","BUILD_TIME_UTC","BUILD_DATE","FLAVOR","APPLOG_EPOCH",
        "WIFI_MAC","BLUETOOTH_ADDR","BLUETOOTH_NAME","BOOT_COUNT",
        "SBX_NATIVE_READ","SBX_HIDE","SBX_CPU_REVISION",
        "SBX_PROC_VERSION","SBX_MEMINFO","SBX_SYSFS_MAC",
        };
        std::string out;
        if (!sbxid::serialize_identity_values(
                kv, order, sizeof(order) / sizeof(order[0]), out))
            return {};
        return out;
    }
};

static std::string uuid_v4() {
    std::string h = random_hex(16, false);
    h.insert(20, "-");
    h.insert(16, "-");
    h.insert(12, "-");
    h.insert(8, "-");
    h[14] = '4';
    static const char* v = "89ab";
    std::random_device rd;
    std::mt19937 g(rd());
    h[19] = v[g() % 4];
    return h;
}

static int device_sdk() {
    char b[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.build.version.sdk", b) > 0) return atoi(b);
    return 0;
}

static bool runtime_is_stable_release() {
    char preview_sdk[PROP_VALUE_MAX] = {0};
    char codename[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.build.version.preview_sdk", preview_sdk) <= 0 ||
        __system_property_get("ro.build.version.codename", codename) <= 0)
        return false;
    return sbxprop::stable_release_runtime(preview_sdk, codename);
}

static std::string gen_host_suffix() {
    std::random_device rd;
    std::mt19937 g(rd());
    static const char* prefixes[] = {
        "abfarm", "abfarm-release", "abfarm-server", "build", "build-server",
        "release", "release-server", "farm", "buildfarm",
    };
    constexpr int n_prefixes = sizeof(prefixes) / sizeof(prefixes[0]);
    std::string host = prefixes[g() % n_prefixes];

    host += "-";
    host += std::to_string(g() % 900 + 100);
    return host;
}

using PixelEntry = sbxpersona::Persona;

static bool is_tensor_platform(const std::string& platform) {
    return sbxpersona::is_tensor_platform(platform);
}

static long long build_utc_from_patch(const std::string& patch,
                                      const std::string& incremental) {
    if (patch.size() < 10 || patch[4] != '-' || patch[7] != '-') return 0;
    const int y = atoi(patch.substr(0, 4).c_str());
    const int m = atoi(patch.substr(5, 2).c_str());
    const int d = atoi(patch.substr(8, 2).c_str());
    if (y < 2008 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) return 0;

    int digits = 0;
    for (char c : incremental) if (c >= '0' && c <= '9') digits += c - '0';

    struct tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon  = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 3;
    time_t t = timegm(&tmv);
    if (t == (time_t)-1) return 0;

    t -= (time_t)(1 + digits % 6) * 86400;
    t += (time_t)(digits % 60) * 60 + (time_t)(digits % 37);
    return (long long)t;
}

static std::string build_date_string(long long utc) {
    if (utc <= 0) return "";
    time_t t = (time_t)utc;
    struct tm tmv{};
    if (!gmtime_r(&t, &tmv)) return "";
    char buf[64];
    if (strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S UTC %Y", &tmv) == 0) return "";
    return buf;
}

static bool valid_provenance_meta(const std::string& raw, int runtime_sdk,
                                  const std::string& candidate,
                                  std::string& error);

static std::vector<PixelEntry> builtin_personas() {
    return sbxpersona::compiled_personas();
}

static uint64_t persona_selector() {
    std::random_device random;
    uint64_t selector = static_cast<uint64_t>(random()) << 32;
    selector ^= static_cast<uint64_t>(random());
    selector ^= static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return selector;
}

static void load_valid_persona_cache(int runtime_sdk, std::string& cache,
                                     std::string& warning) {
    cache = read_file(PERSONA_CACHE);
    if (cache.empty()) return;
    std::string metadata = read_file(PERSONA_CACHE_META);
    std::string error;
    if (!valid_provenance_meta(metadata, runtime_sdk, cache, error)) {
        warning = "cache skipped: " + error;
        cache.clear();
    }
}

static bool select_persona(sbxpersona::Selection& selection,
                           std::vector<std::string>& warnings,
                           std::string& error) {
    int runtime_sdk = device_sdk();
    if (runtime_sdk <= 0) {
        error = "device SDK is unavailable";
        return false;
    }
    std::vector<std::string> extension_warnings;
    std::vector<PixelEntry> extensions = sbxpersona::parse_extensions(
        read_file(PERSONAS_FILE), &extension_warnings);
    std::string cache;
    std::string cache_warning;
    load_valid_persona_cache(runtime_sdk, cache, cache_warning);
    std::vector<std::string> selection_warnings;
    bool selected = sbxpersona::select(
        runtime_sdk, read_file(PERSONA_OVERRIDE), cache, extensions,
        builtin_personas(), persona_selector(), selection, selection_warnings, error);
    warnings = std::move(extension_warnings);
    if (!cache_warning.empty()) warnings.push_back(std::move(cache_warning));
    warnings.insert(warnings.end(), selection_warnings.begin(),
                    selection_warnings.end());
    return selected;
}

static std::string local_mac() {
    std::string raw = random_hex(6, false);
    unsigned first = static_cast<unsigned>(std::strtoul(raw.substr(0, 2).c_str(), nullptr, 16));
    first = (first | 0x02u) & 0xfeu;
    char octet[3];
    std::snprintf(octet, sizeof(octet), "%02x", first);
    raw.replace(0, 2, octet);
    std::string mac;
    for (size_t i = 0; i < raw.size(); i += 2) {
        if (!mac.empty()) mac.push_back(':');
        mac.append(raw, i, 2);
    }
    return mac;
}

static Identity derive_identity(const PixelEntry& p) {
    const bool tensor = is_tensor_platform(p.platform);

    auto tensor_soc_model = [](const std::string& plat) -> const char* {
        if (plat == "gs101")   return "GS101";
        if (plat == "gs201")   return "GS201";
        if (plat == "zuma")    return "GS301";
        if (plat == "zumapro") return "GS401";
        if (plat == "laguna")  return "GS501";
        return "unknown";
    };

    const std::string brand   = p.brand.empty()        ? "google" : p.brand;
    const std::string manuf   = p.manufacturer.empty() ? "Google" : p.manufacturer;
    const std::string soc_man = !p.soc_manufacturer.empty() ? p.soc_manufacturer
                                : (tensor ? "Google" : std::string());
    const std::string soc_mod = !p.soc_model.empty() ? p.soc_model
                                : (tensor ? tensor_soc_model(p.platform) : std::string());

    Identity id;
    id.kv["BRAND"]           = brand;
    id.kv["MANUFACTURER"]    = manuf;
    id.kv["MODEL"]           = p.model;

    id.kv["MARKETNAME"]      = p.marketname.empty() ? p.model : p.marketname;
    id.kv["DEVICE"]          = p.device;
    id.kv["PRODUCT"]         = p.product;
    id.kv["BOARD"]           = p.board;
    id.kv["HARDWARE"]        = p.board;
    id.kv["BOARD_PLATFORM"]  = p.platform;
    id.kv["SOC_MANUFACTURER"] = soc_man;
    id.kv["SOC_MODEL"]       = soc_mod;
    id.kv["ID"]              = p.id;
    id.kv["INCREMENTAL"]     = p.incremental;
    id.kv["RELEASE"]         = p.release;
    id.kv["SDK_INT"]         = std::to_string(p.sdk);
    id.kv["SECURITY_PATCH"]  = p.security_patch;
    id.kv["BOOTLOADER"]      = "unknown";
    id.kv["HOST"]            = gen_host_suffix();
    id.kv["USER"]            = "android-build";
    id.kv["TYPE"]            = "user";
    id.kv["TAGS"]            = "release-keys";

    id.kv["FLAVOR"]          = p.product + "-" + id.kv["TYPE"];

    char fp[512];
    snprintf(fp, sizeof(fp), "%s/%s/%s:%s/%s/%s:user/release-keys",
             brand.c_str(), p.product.c_str(), p.device.c_str(),
             p.release.c_str(), p.id.c_str(), p.incremental.c_str());
    id.kv["FINGERPRINT"] = fp;
    id.kv["DISPLAY"]     = p.id;

    char desc[512];
    snprintf(desc, sizeof(desc), "%s-user %s %s %s release-keys",
             p.product.c_str(), p.release.c_str(), p.id.c_str(), p.incremental.c_str());
    id.kv["DESCRIPTION"] = desc;

    if (!p.radio.empty()) id.kv["RADIO"] = p.radio;

    id.kv["SERIAL"]         = random_hex(8, true);
    id.kv["ANDROID_ID"]     = random_hex(8, false);
    id.kv["GOOGLE_AID"]     = uuid_v4();
    id.kv["WIFI_MAC"]       = local_mac();
    id.kv["BLUETOOTH_ADDR"] = local_mac();
    id.kv["BLUETOOTH_NAME"] = p.model;
    {
        std::random_device random;
        id.kv["BOOT_COUNT"] = std::to_string(1 + random() % 30);
    }

    {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_ms <= 0) now_ms = 1700000000000LL;
        id.kv["APPLOG_EPOCH"] = std::to_string(now_ms);
    }

    {
        long long utc = build_utc_from_patch(p.security_patch, p.incremental);
        if (utc > 0) {
            id.kv["BUILD_TIME_UTC"] = std::to_string(utc);
            id.kv["BUILD_DATE"]     = build_date_string(utc);
        }
    }

    id.kv["SKU"]     = "";
    id.kv["ODM_SKU"] = "";
    id.kv["SBX_NATIVE_READ"] = "1";
    id.kv["SBX_HIDE"] = "0";
    id.kv["SBX_CPU_REVISION"] = "0";
    id.kv["SBX_PROC_VERSION"] = "0";
    id.kv["SBX_MEMINFO"] = "0";
    id.kv["SBX_SYSFS_MAC"] = "0";

    return id;
}

static bool gen_identity(Identity& out, std::string& error,
                         sbxpersona::Source* source = nullptr) {
    sbxpersona::Selection selection;
    std::vector<std::string> warnings;
    if (!select_persona(selection, warnings, error)) return false;
    for (const std::string& warning : warnings)
        fprintf(stderr, "! %s\n", warning.c_str());
    fprintf(stderr, "* persona source: %s (%s, SDK %d)\n",
            sbxpersona::source_name(selection.source),
            selection.persona.model.c_str(), selection.persona.sdk);
    out = derive_identity(selection.persona);
    if (source) *source = selection.source;
    return true;
}

#ifdef SBX_DEBUG
#define SBX_VARIANT_TAG "debug"
#define DBG(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
#else
#define SBX_VARIANT_TAG "release"
#define DBG(...) ((void)0)
#endif

static int apply_properties(const Identity& id) {
    DBG("apply_properties: identity has %zu kv pairs", id.kv.size());
    auto get = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    struct Rp { const char* key; std::string val; bool del_if_empty = false; };

    const std::string SERIAL       = get("SERIAL");
    const std::string MODEL        = get("MODEL");
    const std::string BRAND        = get("BRAND");
    const std::string MANUFACTURER = get("MANUFACTURER");
    const std::string DEVICE       = get("DEVICE");
    const std::string PRODUCT      = get("PRODUCT");
    const std::string ID_          = get("ID");
    const std::string FP           = get("FINGERPRINT");
    const std::string DISPLAY      = get("DISPLAY");
    const std::string DESC         = get("DESCRIPTION");
    const std::string RELEASE      = get("RELEASE");
    const std::string SECPATCH     = get("SECURITY_PATCH");
    const std::string INCREMENTAL  = get("INCREMENTAL");
    const std::string RADIO        = get("RADIO");
    const std::string TAGS         = get("TAGS");
    const std::string TYPE         = get("TYPE");
    const std::string USER_        = get("USER");
    const std::string HOST         = get("HOST");
    const std::string MARKETNAME   = get("MARKETNAME");

    const std::string SKU        = get("SKU");
    const std::string ODM_SKU    = get("ODM_SKU");

    const std::string FLAVOR     = get("FLAVOR");
    const std::string BUILD_UTC  = get("BUILD_TIME_UTC");
    const std::string BUILD_DATE = get("BUILD_DATE");

    std::vector<Rp> rp = {
        {"ro.serialno",                        SERIAL},
        {"ro.boot.serialno",                   SERIAL},

        {"ro.build.fingerprint",               FP},
        {"ro.bootimage.build.fingerprint",     FP},
        {"ro.system.build.fingerprint",        FP},
        {"ro.vendor.build.fingerprint",        FP},
        {"ro.odm.build.fingerprint",           FP},
        {"ro.product.build.fingerprint",       FP},
        {"ro.system_ext.build.fingerprint",    FP},
        {"ro.vendor_dlkm.build.fingerprint",   FP},
        {"ro.odm_dlkm.build.fingerprint",      FP},

        {"ro.product.model",                   MODEL},
        {"ro.product.system.model",            MODEL},
        {"ro.product.vendor.model",            MODEL},
        {"ro.product.odm.model",               MODEL},
        {"ro.product.product.model",           MODEL},
        {"ro.product.system_ext.model",        MODEL},

        {"ro.product.brand",                   BRAND},
        {"ro.product.system.brand",            BRAND},
        {"ro.product.vendor.brand",            BRAND},
        {"ro.product.odm.brand",               BRAND},
        {"ro.product.product.brand",           BRAND},
        {"ro.product.system_ext.brand",        BRAND},

        {"ro.product.manufacturer",            MANUFACTURER},
        {"ro.product.system.manufacturer",     MANUFACTURER},
        {"ro.product.vendor.manufacturer",     MANUFACTURER},
        {"ro.product.odm.manufacturer",        MANUFACTURER},
        {"ro.product.product.manufacturer",    MANUFACTURER},
        {"ro.product.system_ext.manufacturer", MANUFACTURER},

        {"ro.product.device",                  DEVICE},
        {"ro.product.system.device",           DEVICE},
        {"ro.product.vendor.device",           DEVICE},
        {"ro.product.odm.device",              DEVICE},
        {"ro.product.product.device",          DEVICE},
        {"ro.product.system_ext.device",       DEVICE},

        {"ro.product.name",                    PRODUCT},
        {"ro.product.system.name",             PRODUCT},
        {"ro.product.vendor.name",             PRODUCT},
        {"ro.product.odm.name",                PRODUCT},
        {"ro.product.product.name",            PRODUCT},
        {"ro.product.system_ext.name",         PRODUCT},

        {"ro.build.product",                   DEVICE},

        {"ro.product.marketname",              MARKETNAME},
        {"ro.product.vendor.marketname",       MARKETNAME},
        {"ro.product.odm.marketname",          MARKETNAME},
        {"ro.product.system.marketname",       MARKETNAME},
        {"ro.product.product.marketname",      MARKETNAME},

        {"ro.build.id",                        ID_},
        {"ro.build.display.id",                DISPLAY},
        {"ro.build.description",               DESC},
        {"ro.build.tags",                      TAGS},
        {"ro.build.type",                      TYPE},
        {"ro.build.user",                      USER_},
        {"ro.build.host",                      HOST},
        {"ro.build.flavor",                    FLAVOR},
        {"ro.build.date.utc",                  BUILD_UTC},
        {"ro.build.date",                      BUILD_DATE},

        {"ro.build.version.release",           RELEASE},
        {"ro.build.version.security_patch",    SECPATCH},
        {"ro.vendor.build.security_patch",     SECPATCH},
        {"ro.build.version.incremental",       INCREMENTAL},

        {"ro.product.build.id",                  ID_},
        {"ro.system.build.id",                   ID_},
        {"ro.system_ext.build.id",               ID_},
        {"ro.vendor.build.id",                   ID_},
        {"ro.odm.build.id",                      ID_},

        {"ro.product.build.version.incremental",     INCREMENTAL},
        {"ro.system.build.version.incremental",      INCREMENTAL},
        {"ro.system_ext.build.version.incremental",  INCREMENTAL},
        {"ro.vendor.build.version.incremental",      INCREMENTAL},
        {"ro.odm.build.version.incremental",         INCREMENTAL},

        {"ro.product.build.version.release",         RELEASE},
        {"ro.system.build.version.release",          RELEASE},
        {"ro.system_ext.build.version.release",      RELEASE},
        {"ro.vendor.build.version.release",          RELEASE},
        {"ro.odm.build.version.release",             RELEASE},

        {"ro.product.build.date.utc",                BUILD_UTC},
        {"ro.system.build.date.utc",                 BUILD_UTC},
        {"ro.system_ext.build.date.utc",             BUILD_UTC},
        {"ro.vendor.build.date.utc",                 BUILD_UTC},
        {"ro.odm.build.date.utc",                    BUILD_UTC},
        {"ro.bootimage.build.date.utc",              BUILD_UTC},

        {"ro.product.build.date",                    BUILD_DATE},
        {"ro.system.build.date",                     BUILD_DATE},
        {"ro.system_ext.build.date",                 BUILD_DATE},
        {"ro.vendor.build.date",                     BUILD_DATE},
        {"ro.odm.build.date",                        BUILD_DATE},
        {"ro.bootimage.build.date",                  BUILD_DATE},

        {"ro.product.build.type",                    TYPE},
        {"ro.system.build.type",                     TYPE},
        {"ro.system_ext.build.type",                 TYPE},
        {"ro.vendor.build.type",                     TYPE},
        {"ro.odm.build.type",                        TYPE},

        {"ro.product.build.tags",                    TAGS},
        {"ro.system.build.tags",                     TAGS},
        {"ro.system_ext.build.tags",                 TAGS},
        {"ro.vendor.build.tags",                     TAGS},
        {"ro.odm.build.tags",                        TAGS},

        {"gsm.version.baseband",               RADIO, true},
        {"ro.build.expect.baseband",           RADIO, true},

        {"ro.bootloader",                      std::string("unknown")},
        {"ro.boot.bootloader",                 std::string("unknown")},

        {"ro.boot.hardware.sku",               SKU},
        {"ro.boot.product.hardware.sku",       ODM_SKU},
    };

    if (runtime_is_stable_release()) {
        rp.push_back({"ro.build.version.release_or_codename", RELEASE});
        static const char* const aliases[] = {
            "ro.product.build.version.release_or_codename",
            "ro.system.build.version.release_or_codename",
            "ro.system_ext.build.version.release_or_codename",
            "ro.vendor.build.version.release_or_codename",
            "ro.odm.build.version.release_or_codename",
        };
        for (const char* key : aliases) rp.push_back({key, RELEASE});
    }

    int failures = 0;
    bool have_bundled = (::access(RESETPROP, X_OK) == 0);
    {
        int applied = 0, failed = 0;
        for (const auto& r : rp) {
            if (r.val.empty() && !r.del_if_empty) continue;
            int rc;
            if (r.val.empty()) {

                if (have_bundled) {
                    rc = run_bin(RESETPROP, {"resetprop-rs", "--delete", r.key});
                } else {
                    rc = run_bin_path("resetprop", {"resetprop", "--delete", r.key});
                    if (rc != 0)
                        rc = run_bin_path("resetprop-rs", {"resetprop-rs", "--delete", r.key});
                }
            } else if (have_bundled) {
                rc = run_bin(RESETPROP, {"resetprop-rs", "-n", r.key, r.val.c_str()});
            } else {
                rc = run_bin_path("resetprop", {"resetprop", "-n", r.key, r.val.c_str()});
                if (rc != 0)
                    rc = run_bin_path("resetprop-rs", {"resetprop-rs", "-n", r.key, r.val.c_str()});
            }
            if (rc == 0) {
                applied++;
            } else {
                failed++;
                failures++;
                fprintf(stderr, "! resetprop gagal (exit!=0): %s\n", r.key);
            }
        }
        printf("  Native prop: %d ok, %d gagal%s\n", applied, failed,
               have_bundled ? "" : " [fallback PATH]");
        if (applied == 0 && failed > 0)
            fprintf(stderr, "! SEMUA resetprop gagal%s — cek ketersediaan resetprop / resetprop-rs\n",
                    have_bundled ? "" : " (bundled absent + PATH fallback gagal)");
    }

    {
        static const char* const emu_props[] = {
            "ro.kernel.qemu",
            "ro.kernel.qemu.gles",
            "ro.boot.qemu",
            "ro.boot.qemu.gltransport",
            "ro.hardware.virtual_device",
            "qemu.hw.mainkeys",
            "init.svc.qemud",
            "init.svc.qemu-props",
            "init.svc.goldfish-logcat",
            "init.svc.goldfish-setup",
            "init.svc.ranchu-net",
        };
        static const char* const identity_props[] = {
            "ro.ril.factory_id",
            "persist.odm.ril.factory_id",
            "ro.ril.oem.imei",  "ro.ril.oem.imei0", "ro.ril.oem.imei1", "ro.ril.oem.imei2",
            "ro.ril.miui.imei", "ro.ril.miui.imei0", "ro.ril.miui.imei1", "ro.ril.miui.imei2",
            "ro.ril.oem.meid",  "ro.ril.oem.psno",  "ro.ril.oem.btmac",
            "persist.odm.ril.oem.imei0", "persist.odm.ril.oem.imei1", "persist.odm.ril.oem.imei2",
            "persist.odm.ril.oem.sno", "persist.odm.ril.oem.psno",
            "persist.odm.ril.oem.wifimac", "persist.odm.ril.oem.btmac",
            "persist.radio.imei", "persist.radio.imei0", "persist.radio.imei1", "persist.radio.imei2",
            "ro.product.serial", "ro.build.serial",
            "ro.kernel.androidboot.serialno", "ril.serialnumber",
            "gsm.sim.preiccid_0", "gsm.sim.preiccid_1",
            "persist.vendor.radio.cfu.iccid.1",
            "persist.netd.stable_secret",
        };
        static const char* const custom_rom_props[] = {
            "ro.modversion",
            "ro.cm.version",
            "ro.cm.build.date",
        };
        static const char* const oem_props[] = {
            "ro.product.cert",
            "ro.product.mod_device",
            "ro.fota.oem",
            "ro.netflix.bsp_rev",
            "ro.baseband",
            "persist.sys.hardcoder.name",
            "persist.vendor.sys.fp.module",
            "persist.vendor.sys.fp.vendor",
            "ro.com.google.clientidbase",
            "ro.com.google.clientidbase.ms",
            "ro.com.google.clientidbase.tx",
            "ro.com.google.clientidbase.vs",
            "ro.com.google.clientidbase.am",
            "ro.com.google.clientidbase.yt",
            "ro.miui.build.region",
            "ro.miui.ui.version.code",
            "ro.miui.ui.version.name",
            "ro.miui.cust_variant",
            "ro.miui.region",
            "ro.miui.mcc",
            "ro.miui.mnc",
            "ro.vendor.miui.region",
            "ro.vendor.miui.mcc",
            "ro.vendor.miui.mnc",
            "ro.vendor.miui.cust_variant",
        };
        int del_ok = 0, del_skip = 0;
        auto try_delete = [&](const char* prop) {
            char buf[PROP_VALUE_MAX] = {0};
            if (__system_property_get(prop, buf) <= 0) { del_skip++; return; }
            int rc;
            if (have_bundled) {
                rc = run_bin(RESETPROP, {"resetprop-rs", "--delete", prop});
            } else {
                rc = run_bin_path("resetprop", {"resetprop", "--delete", prop});
                if (rc != 0)
                    rc = run_bin_path("resetprop-rs", {"resetprop-rs", "--delete", prop});
            }
            if (rc == 0) {
                del_ok++;
            } else {
                failures++;
                fprintf(stderr, "! resetprop delete gagal (exit!=0): %s\n", prop);
            }
        };
        for (const char* p : emu_props) try_delete(p);
        for (const char* p : identity_props) try_delete(p);
        for (const char* p : custom_rom_props) try_delete(p);
        for (const char* p : oem_props) try_delete(p);
        if (del_ok > 0)
            printf("  Sanitized: %d prop(s) deleted, %d absent\n", del_ok, del_skip);
    }
    return failures == 0 ? 0 : 1;
}

static int apply_framework_settings(const Identity& id) {
    auto get = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };
    const std::string model = get("MODEL");
    int failures = 0;
    int applied = 0;

    // ANDROID_ID is retained as profile entropy for locally derived identifiers.
    // A user-level secure setting is not a per-app SSAID, so do not publish it.
    if (!model.empty()) {
        int global_rc = run_framework("/system/bin/settings",
                {"settings", "put", "--user", "0", "global", "device_name", model.c_str()},
                "settings put global device_name");
        if (global_rc == 0) applied++; else failures++;

        int system_rc = run_framework("/system/bin/settings",
                {"settings", "put", "--user", "0", "system", "device_name", model.c_str()},
                "settings put system device_name");
        if (system_rc == 0) applied++; else failures++;
    }
    printf("  Framework settings: %d ok, %d gagal\n", applied, failures);
    return failures == 0 ? 0 : 1;
}

struct OverlaySet {
    std::map<std::string, std::string> files;
};

static bool build_mount_files(const Identity& id, OverlaySet& overlays) {
    overlays.files.clear();
    auto g = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };

    const std::string SERIAL       = g("SERIAL");
    const std::string MODEL        = g("MODEL");
    const std::string BRAND        = g("BRAND");
    const std::string MANUFACTURER = g("MANUFACTURER");
    const std::string DEVICE       = g("DEVICE");
    const std::string PRODUCT      = g("PRODUCT");
    const std::string ID_          = g("ID");
    const std::string FP           = g("FINGERPRINT");
    const std::string DISPLAY      = g("DISPLAY");
    const std::string DESC         = g("DESCRIPTION");
    const std::string RELEASE      = g("RELEASE");
    const std::string SECPATCH     = g("SECURITY_PATCH");
    const std::string INCREMENTAL  = g("INCREMENTAL");
    const std::string RADIO        = g("RADIO");
    const std::string TAGS         = g("TAGS");
    const std::string TYPE         = g("TYPE");
    const std::string USER_        = g("USER");
    const std::string HOST         = g("HOST");
    const std::string MARKETNAME   = g("MARKETNAME");
    const bool stable_release      = runtime_is_stable_release();

    std::string base;
    base += "# begin build properties\n";
    auto add = [&](const char* k, const std::string& v) {
        if (!v.empty()) { base += k; base += '='; base += v; base += '\n'; }
    };
    add("ro.serialno",                        SERIAL);
    add("ro.boot.serialno",                   SERIAL);
    add("ro.build.fingerprint",               FP);
    add("ro.bootimage.build.fingerprint",     FP);
    add("ro.system.build.fingerprint",        FP);
    add("ro.vendor.build.fingerprint",        FP);
    add("ro.odm.build.fingerprint",           FP);
    add("ro.product.build.fingerprint",       FP);
    add("ro.system_ext.build.fingerprint",    FP);
    add("ro.vendor_dlkm.build.fingerprint",   FP);
    add("ro.odm_dlkm.build.fingerprint",      FP);
    add("ro.product.model",                   MODEL);
    add("ro.product.brand",                   BRAND);
    add("ro.product.manufacturer",            MANUFACTURER);
    add("ro.product.device",                  DEVICE);
    add("ro.product.name",                    PRODUCT);
    add("ro.product.marketname",              MARKETNAME);
    add("ro.product.vendor.marketname",       MARKETNAME);
    add("ro.product.odm.marketname",          MARKETNAME);
    add("ro.product.system.marketname",       MARKETNAME);
    add("ro.product.product.marketname",      MARKETNAME);
    add("ro.build.id",                        ID_);
    add("ro.build.display.id",                DISPLAY);
    add("ro.build.description",               DESC);
    add("ro.build.tags",                      TAGS);
    add("ro.build.type",                      TYPE);
    add("ro.build.user",                      USER_);
    add("ro.build.host",                      HOST);
    add("ro.build.flavor",                    g("FLAVOR"));
    add("ro.build.date.utc",                  g("BUILD_TIME_UTC"));
    add("ro.build.date",                      g("BUILD_DATE"));
    add("ro.build.version.release",           RELEASE);
    if (stable_release)
        add("ro.build.version.release_or_codename", RELEASE);
    add("ro.build.version.security_patch",    SECPATCH);
    add("ro.build.version.incremental",       INCREMENTAL);

    add("ro.boot.hardware.sku",               g("SKU"));
    add("ro.boot.product.hardware.sku",       g("ODM_SKU"));

    add("ro.bootloader",                      std::string("unknown"));
    add("ro.boot.bootloader",                 std::string("unknown"));
    add("ro.build.product",                   DEVICE);
    add("gsm.version.baseband",               RADIO);
    add("ro.build.expect.baseband",           RADIO);

    struct { const char* dir; const char* pfx; } parts[] = {
        {"system",     "ro.product.system."},
        {"vendor",     "ro.product.vendor."},
        {"odm",        "ro.product.odm."},
        {"product",    "ro.product.product."},
        {"system_ext", "ro.product.system_ext."},
    };
    for (const auto& p : parts) {
        std::string c = base;
        std::string pfx = p.pfx;
        if (!MODEL.empty())        c += pfx + "model="        + MODEL        + "\n";
        if (!BRAND.empty())        c += pfx + "brand="        + BRAND        + "\n";
        if (!MANUFACTURER.empty()) c += pfx + "manufacturer=" + MANUFACTURER + "\n";
        if (!DEVICE.empty())       c += pfx + "device="       + DEVICE       + "\n";
        if (!PRODUCT.empty())      c += pfx + "name="         + PRODUCT      + "\n";
        if (!MARKETNAME.empty())   c += pfx + "marketname="   + MARKETNAME   + "\n";
        std::string ppfx = std::string("ro.") + p.dir + ".build.";
        c += ppfx + "id=" + ID_ + "\n";
        c += ppfx + "fingerprint=" + FP + "\n";
        c += ppfx + "type=" + TYPE + "\n";
        c += ppfx + "tags=" + TAGS + "\n";
        c += ppfx + "version.incremental=" + INCREMENTAL + "\n";
        c += ppfx + "version.release=" + RELEASE + "\n";
        if (stable_release)
            c += ppfx + "version.release_or_codename=" + RELEASE + "\n";
        if (!g("BUILD_TIME_UTC").empty()) {
            c += ppfx + "date.utc=" + g("BUILD_TIME_UTC") + "\n";
            c += ppfx + "date=" + g("BUILD_DATE") + "\n";
        }
        if (std::string(p.dir) == "vendor") {
            c += "ro.vendor.build.security_patch=" + SECPATCH + "\n";
        }
        overlays.files.emplace(p.dir, std::move(c));
    }
    return overlays.files.size() == sandboxid::MOUNT_PARTS_N;
}

static bool ensure_overlay_directories(const std::string& root) {
    if (::mkdir(root.c_str(), 0755) != 0 || ::chmod(root.c_str(), 0755) != 0)
        return false;
    for (size_t i = 0; i < sandboxid::MOUNT_PARTS_N; ++i) {
        std::string directory = root + "/" + sandboxid::MOUNT_PARTS[i];
        if (::mkdir(directory.c_str(), 0755) != 0 ||
            ::chmod(directory.c_str(), 0755) != 0)
            return false;
    }
    return true;
}

static bool sync_overlay_tree(const std::string& root) {
    for (size_t i = 0; i < sandboxid::MOUNT_PARTS_N; ++i) {
        std::string directory = root + "/" + sandboxid::MOUNT_PARTS[i];
        if (!fsync_directory(directory)) return false;
    }
    return fsync_directory(root);
}

struct OverlayPublication {
    bool activated = false;
    bool had_live = false;
    std::string previous;
};

static bool publish_mount_files(const OverlaySet& overlays,
                                OverlayPublication* publication = nullptr) {
    DBG("publish_mount_files: MOUNTDIR=%s", MOUNTDIR);
    const std::string suffix = std::to_string(static_cast<long>(::getpid()));
    const std::string stage = std::string(MOUNTDIR) + ".stage." + suffix;
    const std::string previous = std::string(MOUNTDIR) + ".previous." + suffix;
    if (publication) *publication = {};
    if (::access(stage.c_str(), F_OK) == 0 || ::access(previous.c_str(), F_OK) == 0) {
        fprintf(stderr, "! overlay transaction path already exists\n");
        return false;
    }
    if (!ensure_overlay_directories(stage)) {
        remove_tree_durable(stage);
        fprintf(stderr, "! cannot create staged overlay tree\n");
        return false;
    }
    struct { const char* sub; const char* ctx; } part_ctx[] = {
        {"system",     "u:object_r:system_file:s0"},
        {"vendor",     "u:object_r:vendor_file:s0"},
        {"odm",        "u:object_r:vendor_file:s0"},
        {"product",    "u:object_r:system_file:s0"},
        {"system_ext", "u:object_r:system_file:s0"},
    };
    for (const auto& pc : part_ctx) {
        auto content = overlays.files.find(pc.sub);
        if (content == overlays.files.end()) {
            remove_tree_durable(stage);
            fprintf(stderr, "! staged overlay content missing for %s\n", pc.sub);
            return false;
        }
        std::string path = stage + "/" + pc.sub + "/build.prop";
        if (!atomic_write(path, content->second) || ::chmod(path.c_str(), 0644) != 0 ||
            run_bin("/system/bin/chcon", {"chcon", pc.ctx, path.c_str()}) != 0) {
            remove_tree_durable(stage);
            fprintf(stderr, "! failed to stage overlay %s\n", path.c_str());
            return false;
        }
    }
    if (!sync_overlay_tree(stage)) {
        remove_tree_durable(stage);
        fprintf(stderr, "! failed to sync staged overlay tree\n");
        return false;
    }
    const std::string parent = parent_directory(MOUNTDIR);
    const bool had_live = ::access(MOUNTDIR, F_OK) == 0;
    if (had_live && ::rename(MOUNTDIR, previous.c_str()) != 0) {
        remove_tree_durable(stage);
        fprintf(stderr, "! failed to preserve live overlay tree\n");
        return false;
    }
    if (::rename(stage.c_str(), MOUNTDIR) != 0) {
        bool restored = !had_live || ::rename(previous.c_str(), MOUNTDIR) == 0;
        remove_tree_durable(stage);
        if (restored) fsync_directory(parent);
        fprintf(stderr, restored
                    ? "! failed to activate staged overlay tree\n"
                    : "! failed to activate staged overlay tree and restore previous tree\n");
        return false;
    }
    if (!fsync_directory(parent)) {
        fprintf(stderr, "! overlay tree activated but directory sync failed\n");
        if (publication) {
            publication->activated = true;
            publication->had_live = had_live;
            publication->previous = previous;
        }
        return false;
    }
    if (publication) {
        publication->activated = true;
        publication->had_live = had_live;
        publication->previous = previous;
    } else if (had_live) {
        if (!remove_tree(previous) || !fsync_directory(parent)) {
            fprintf(stderr, "! refreshed overlay activated but previous tree cleanup failed\n");
            return false;
        }
    }
    if (::unlink(LEGACY_SETTINGS_OVERLAY) != 0 && errno != ENOENT) {
        fprintf(stderr, "! failed to remove legacy settings overlay\n");
        return false;
    }
    printf("  Mount overlay: 5 build.prop trees -> %s\n", MOUNTDIR);
    return true;
}

static bool finish_overlay_publication(OverlayPublication& publication) {
    if (!publication.activated) return true;
    if (publication.had_live) {
        if (!remove_tree(publication.previous) ||
            !fsync_directory(parent_directory(publication.previous)))
            return false;
    }
    publication = {};
    return true;
}

static bool restore_previous_mount_files(OverlayPublication& publication) {
    if (!publication.activated) return true;
    const std::string parent = parent_directory(MOUNTDIR);
    const std::string failed = std::string(MOUNTDIR) + ".failed." +
                               std::to_string(static_cast<long>(::getpid()));
    if (::access(failed.c_str(), F_OK) == 0) return false;
    if (::rename(MOUNTDIR, failed.c_str()) != 0) return false;
    if (publication.had_live && ::rename(publication.previous.c_str(), MOUNTDIR) != 0) {
        ::rename(failed.c_str(), MOUNTDIR);
        return false;
    }
    if (!fsync_directory(parent)) return false;
    if (!remove_tree(failed) || !fsync_directory(parent)) return false;
    publication = {};
    return true;
}

static int publish_generated_mount_files(const Identity& id) {
    OverlaySet overlays;
    OverlayPublication publication;
    if (!build_mount_files(id, overlays) ||
        !publish_mount_files(overlays, &publication)) {
        if (publication.activated && !restore_previous_mount_files(publication))
            return 32;
        return 1;
    }
    return finish_overlay_publication(publication) ? 0 : 20;
}

static int cmd_targets(const char* view) {
    sbxtarget::TargetSet targets;
    std::string error;
    if (!load_target_set(targets, error)) {
        fprintf(stderr, "! target list rejected: %s\n", error.c_str());
        return 1;
    }
    if (view && strcmp(view, "--processes") && strcmp(view, "--packages")) {
        fprintf(stderr, "Usage: sandboxid targets [--processes|--packages]\n");
        return 64;
    }
    const bool packages = view && !strcmp(view, "--packages");
    const std::vector<std::string>& selected =
        packages ? targets.packages : targets.processes;
    for (const std::string& item : selected) printf("%s\n", item.c_str());
    return 0;
}

static bool parse_identity_blob(const std::string& blob, const char* path,
                                Identity& id, std::string& error,
                                bool* needs_migration = nullptr) {
    if (needs_migration) *needs_migration = false;
    if (blob.empty()) {
        error = std::string(path) + " is empty or unreadable";
        return false;
    }
    const int sdk = device_sdk();
    if (sdk <= 0) {
        error = "device SDK is unavailable";
        return false;
    }
    sbxid::IdentitySnapshot snapshot;
    sbxid::ValidationContext context;
    context.runtime_sdk = sdk;
    context.max_blob = sandboxid::MAX_IDENTITY_BLOB;
    context.drop_legacy_capabilities = true;
    if (!sbxid::parse_and_validate_identity(blob, context, snapshot, error))
        return false;
    if (!snapshot.dropped_legacy_capabilities.empty()) {
        fprintf(stderr, "* ignored %zu legacy runtime-capability key(s) in %s\n",
                snapshot.dropped_legacy_capabilities.size(), path);
        if (needs_migration) *needs_migration = true;
    }
    id.kv = std::move(snapshot.values);
    return true;
}

static bool load_identity_file(const char* path, Identity& id, std::string& error,
                               bool* needs_migration = nullptr) {
    return parse_identity_blob(read_file(path), path, id, error, needs_migration);
}

static bool validate_identity(const Identity& id, std::string& error) {
    const int sdk = device_sdk();
    if (sdk <= 0) {
        error = "device SDK is unavailable";
        return false;
    }
    sbxid::IdentitySnapshot snapshot;
    sbxid::ValidationContext context;
    context.runtime_sdk = sdk;
    context.max_blob = sandboxid::MAX_IDENTITY_BLOB;
    return sbxid::parse_and_validate_identity(id.serialize(), context, snapshot, error);
}

static bool merge_carrier(Identity& id) {

    sbxcarrier::CarrierSel sel = sbxcarrier::parse_carrier_conf(read_file(CARRIER_CONF));
    return sbxcarrier::apply_carrier(id.kv, sel);
}

static bool ensure_root() {
    if (geteuid() != 0) {
        fprintf(stderr, "! sandboxid must run as root. Use: su -c sandboxid <cmd>\n");
        return false;
    }
    return true;
}

static bool load_current_identity_state(Identity& identity,
                                        sbxtxn::CanonicalMeta& meta,
                                        std::string& error);

static std::string make_run_id() {
    return random_hex(16, false);
}

static std::string canonical_meta(const std::string& run_id,
                                  const std::string& source,
                                  const std::string& identity) {
    sbxtxn::CanonicalMeta meta;
    meta.run = run_id;
    meta.source = source;
    meta.identity_sha256 = sbxhash::sha256(identity);
    return sbxtxn::serialize_meta(meta);
}

static std::string pending_meta(const std::string& run_id,
                                sbxpersona::Source source,
                                const std::string& identity,
                                const std::string& base_state_sha256) {
    sbxtxn::CanonicalMeta meta;
    meta.run = run_id;
    meta.source = sbxpersona::source_name(source);
    meta.identity_sha256 = sbxhash::sha256(identity);
    meta.base_state_sha256 = base_state_sha256;
    return sbxtxn::serialize_meta(meta);
}

static bool load_pending_meta(const std::string& path,
                              const std::string& identity,
                              sbxtxn::CanonicalMeta& meta,
                              std::string& error) {
    if (!sbxtxn::parse_pending_meta(read_file(path), meta, error)) {
        error = path + ": " + error;
        return false;
    }
    if (!sbxtxn::identity_matches(meta, identity)) {
        error = path + ": identity hash mismatch";
        return false;
    }
    return true;
}

static bool load_canonical_meta(const std::string& path,
                                const std::string& identity,
                                sbxtxn::CanonicalMeta& meta,
                                std::string& error) {
    if (!sbxtxn::parse_identity_meta(read_file(path), meta, error)) {
        error = path + ": " + error;
        return false;
    }
    if (!sbxtxn::identity_matches(meta, identity)) {
        error = path + ": identity hash mismatch";
        return false;
    }
    return true;
}

static bool load_optional_current_state(Identity* identity,
                                        sbxtxn::CanonicalMeta* meta,
                                        std::string& serialized,
                                        std::string& metadata,
                                        std::string& error) {
    serialized = read_file(IDENTITY_FILE);
    metadata = read_file(IDENTITY_META);
    const bool have_identity = path_exists(IDENTITY_FILE);
    const bool have_metadata = path_exists(IDENTITY_META);
    if (!have_identity && !have_metadata) return true;
    if (!have_identity || !have_metadata || serialized.empty() || metadata.empty()) {
        error = "canonical identity/metadata pair is incomplete";
        return false;
    }
    Identity parsed_identity;
    if (!parse_identity_blob(serialized, IDENTITY_FILE, parsed_identity, error))
        return false;
    sbxtxn::CanonicalMeta parsed_meta;
    if (!sbxtxn::parse_identity_meta(metadata, parsed_meta, error)) {
        error = std::string(IDENTITY_META) + ": " + error;
        return false;
    }
    if (!sbxtxn::identity_matches(parsed_meta, serialized)) {
        error = std::string(IDENTITY_META) + ": identity hash mismatch";
        return false;
    }
    if (identity) *identity = std::move(parsed_identity);
    if (meta) *meta = std::move(parsed_meta);
    return true;
}

static bool valid_run_id(const std::string& value) {
    return sbxtxn::valid_run_id(value);
}

static bool valid_provenance_meta(const std::string& raw, int runtime_sdk,
                                  const std::string& candidate,
                                  std::string& error) {
    return sbxtxn::validate_provenance(raw, runtime_sdk, candidate, error);
}

static int prepare_locked(std::string& run_id,
                          const char* requested_run_id = nullptr) {
    if (requested_run_id && !valid_run_id(requested_run_id)) {
        fprintf(stderr, "Usage: sandboxid prepare [run-id]\n");
        return 64;
    }
    std::string mode = trim(read_file(MODE_FILE));
    if (mode == "locked") {
        fprintf(stderr, "! identity mode is locked; unlock it explicitly first\n");
        return 64;
    }
    std::string error;
    Identity pending;
    sbxpersona::Source source = sbxpersona::Source::None;
    if (!gen_identity(pending, error, &source)) {
        fprintf(stderr, "! prepare failed: %s\n", error.c_str());
        return 30;
    }
    Identity current;
    std::string current_identity;
    std::string current_metadata;
    if (!load_optional_current_state(&current, nullptr, current_identity,
                                     current_metadata, error)) {
        fprintf(stderr, "! current canonical state rejected: %s\n", error.c_str());
        return 30;
    }
    if (!current_identity.empty())
        sbxid::preserve_operational_flags(current.kv, pending.kv);
    merge_carrier(pending);
    if (!validate_identity(pending, error)) {
        fprintf(stderr, "! prepared identity rejected: %s\n", error.c_str());
        return 30;
    }
    run_id = requested_run_id ? requested_run_id : make_run_id();
    const std::string pending_identity = pending.serialize();
    const std::string base_state_sha256 =
        sbxtxn::state_digest(current_identity, current_metadata);
    PairReplaceResult prepared = replace_pair(
        IDENTITY_PENDING, pending_identity, PENDING_META,
        pending_meta(run_id, source, pending_identity, base_state_sha256));
    if (prepared != PairReplaceResult::Committed) {
        fprintf(stderr, prepared == PairReplaceResult::FailedDegraded
                    ? "! pending publication failed and previous transaction could not be restored\n"
                    : "! failed to publish pending identity transaction; previous transaction preserved\n");
        return prepared == PairReplaceResult::FailedDegraded ? 32 : 30;
    }
    printf("RUN_ID=%s\n", run_id.c_str());
    printf("SOURCE=%s\n", sbxpersona::source_name(source));
    printf("OK: identity prepared without device mutation\n");
    return 0;
}

static int cmd_prepare(const char* requested_run_id) {
    if (!ensure_root()) return 1;
    if (requested_run_id && !valid_run_id(requested_run_id)) {
        fprintf(stderr, "Usage: sandboxid prepare [run-id]\n");
        return 64;
    }
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    std::string run_id;
    return prepare_locked(run_id, requested_run_id);
}

static int cmd_abort(const char* run_id) {
    if (!ensure_root()) return 1;
    if (!run_id || !valid_run_id(run_id)) {
        fprintf(stderr, "Usage: sandboxid abort <run-id>\n");
        return 64;
    }
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    sbxtxn::CanonicalMeta meta;
    if (!sbxtxn::parse_pending_meta(read_file(PENDING_META), meta, error)) {
        fprintf(stderr, "! pending transaction metadata rejected: %s\n",
                error.c_str());
        return 64;
    }
    if (meta.run != run_id) {
        fprintf(stderr, "! pending transaction does not match run %s\n", run_id);
        return 64;
    }
    PairReplaceResult removed = remove_pair_durable(IDENTITY_PENDING, PENDING_META);
    if (removed != PairReplaceResult::Committed) {
        fprintf(stderr, removed == PairReplaceResult::FailedDegraded
                    ? "! pending transaction removal failed and prior state is unproven\n"
                    : "! pending transaction could not be removed; prior state retained\n");
        return removed == PairReplaceResult::FailedDegraded ? 32 : 1;
    }
    printf("OK: pending transaction aborted\n");
    return 0;
}

static int commit_locked(const char* run_id) {
    if (!run_id || !valid_run_id(run_id)) {
        fprintf(stderr, "Usage: sandboxid commit <run-id>\n");
        return 64;
    }
    std::string error;
    const std::string pending_identity = read_file(IDENTITY_PENDING);
    sbxtxn::CanonicalMeta meta;
    if (!load_pending_meta(PENDING_META, pending_identity, meta, error)) {
        fprintf(stderr, "! pending transaction metadata rejected: %s\n",
                error.c_str());
        return 64;
    }
    if (meta.run != run_id) {
        fprintf(stderr, "! pending transaction metadata does not match run %s\n",
                run_id);
        return 64;
    }
    Identity pending;
    if (!load_identity_file(IDENTITY_PENDING, pending, error)) {
        fprintf(stderr, "! pending identity rejected: %s\n", error.c_str());
        return 30;
    }
    std::string current;
    std::string current_meta;
    if (!load_optional_current_state(nullptr, nullptr, current, current_meta, error)) {
        fprintf(stderr, "! current canonical state rejected: %s\n", error.c_str());
        return 30;
    }
    if (sbxtxn::state_digest(current, current_meta) != meta.base_state_sha256) {
        fprintf(stderr, "! canonical state changed after prepare; refusing stale commit\n");
        return 30;
    }
    OverlaySet overlays;
    if (!build_mount_files(pending, overlays)) {
        fprintf(stderr, "! checked overlay generation failed before identity commit\n");
        return 30;
    }
    if (!current.empty()) {
        Identity validated_current;
        if (!load_identity_file(IDENTITY_FILE, validated_current, error)) {
            fprintf(stderr, "! current canonical identity rejected: %s\n",
                    error.c_str());
            return 30;
        }
        const std::string current_identity = validated_current.serialize();
        if (current_identity != current) {
            fprintf(stderr, "! current canonical identity is non-canonical; run seed migration first\n");
            return 30;
        }
        PairReplaceResult backed_up = replace_pair(
            IDENTITY_BAK, current, IDENTITY_META_BAK, current_meta);
        if (backed_up != PairReplaceResult::Committed) {
            fprintf(stderr, backed_up == PairReplaceResult::FailedDegraded
                        ? "! backup publication failed and prior backup pair is degraded\n"
                        : "! failed to preserve current canonical identity pair\n");
            return backed_up == PairReplaceResult::FailedDegraded ? 32 : 30;
        }
    } else {
        PairReplaceResult cleared = remove_pair_durable(IDENTITY_BAK, IDENTITY_META_BAK);
        if (cleared != PairReplaceResult::Committed) {
            fprintf(stderr, cleared == PairReplaceResult::FailedDegraded
                        ? "! stale backup removal failed and backup state is unproven\n"
                        : "! stale backup pair could not be cleared before first commit\n");
            return cleared == PairReplaceResult::FailedDegraded ? 32 : 30;
        }
    }
    OverlayPublication overlay_publication;
    if (!publish_mount_files(overlays, &overlay_publication)) {
        if (overlay_publication.activated &&
            !restore_previous_mount_files(overlay_publication)) {
            fprintf(stderr, "! overlay publication failed after activation and rollback failed\n");
            return 32;
        }
        fprintf(stderr, "! checked overlay publication failed before identity commit\n");
        return 30;
    }
    const std::string committed_identity = pending.serialize();
    const std::string committed_meta =
        canonical_meta(run_id, meta.source, committed_identity);
    PairReplaceResult committed = replace_pair(
        IDENTITY_FILE, committed_identity, IDENTITY_META, committed_meta);
    if (committed != PairReplaceResult::Committed) {
        bool overlays_restored = restore_previous_mount_files(overlay_publication);
        bool degraded = committed == PairReplaceResult::FailedDegraded || !overlays_restored;
        fprintf(stderr, degraded
                    ? "! canonical publication failed and coherent rollback could not be proven\n"
                    : "! canonical publication failed; previous identity and overlays restored\n");
        return degraded ? 32 : 30;
    }
    bool cleanup_ok = finish_overlay_publication(overlay_publication);
    bool identity_removed = remove_file_durable(IDENTITY_PENDING);
    bool meta_removed = remove_file_durable(PENDING_META);
    cleanup_ok = identity_removed && meta_removed && cleanup_ok;
    if (meta.source == "override")
        cleanup_ok = remove_file_durable(PERSONA_OVERRIDE) && cleanup_ok;
    printf("IDENTITY_SHA256=%s\n", meta.identity_sha256.c_str());
    if (!cleanup_ok) {
        fprintf(stderr, "! identity committed, but transaction cleanup is incomplete\n");
        return 20;
    }
    printf("OK: identity committed for run %s\n", run_id);
    return 0;
}

static int cmd_commit(const char* run_id) {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    return commit_locked(run_id);
}

static int cmd_restore() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity backup;
    if (!load_identity_file(IDENTITY_BAK, backup, error)) {
        fprintf(stderr, "! backup rejected: %s\n", error.c_str());
        return 1;
    }
    OverlaySet overlays;
    if (!build_mount_files(backup, overlays)) {
        fprintf(stderr, "! failed to generate restored overlay set\n");
        return 1;
    }
    std::string backup_meta = read_file(IDENTITY_META_BAK);
    const std::string backup_identity = read_file(IDENTITY_BAK);
    sbxtxn::CanonicalMeta parsed_backup_meta;
    if (!sbxtxn::parse_identity_meta(backup_meta, parsed_backup_meta, error)) {
        fprintf(stderr, "! backup metadata is missing or invalid: %s\n",
                error.c_str());
        return 1;
    }
    if (!sbxtxn::identity_matches(parsed_backup_meta, backup_identity)) {
        fprintf(stderr, "! backup metadata does not match backup identity\n");
        return 1;
    }
    const std::string restored_identity = backup.serialize();
    if (restored_identity != backup_identity) {
        fprintf(stderr, "! backup identity is non-canonical; refusing inexact restore\n");
        return 1;
    }
    OverlayPublication overlay_publication;
    if (!publish_mount_files(overlays, &overlay_publication)) {
        if (overlay_publication.activated &&
            !restore_previous_mount_files(overlay_publication)) {
            fprintf(stderr, "! restored overlay publication failed and rollback failed\n");
            return 32;
        }
        fprintf(stderr, "! failed to publish restored overlays\n");
        return 1;
    }
    PairReplaceResult restored = replace_pair(
        IDENTITY_FILE, restored_identity, IDENTITY_META, backup_meta);
    if (restored != PairReplaceResult::Committed) {
        bool overlays_restored = restore_previous_mount_files(overlay_publication);
        bool degraded = restored == PairReplaceResult::FailedDegraded || !overlays_restored;
        fprintf(stderr, degraded
                    ? "! failed to restore canonical identity and coherent rollback is unproven\n"
                    : "! failed to restore canonical identity; current state retained\n");
        return degraded ? 32 : 1;
    }
    if (!finish_overlay_publication(overlay_publication)) {
        fprintf(stderr, "! canonical identity restored, but previous overlay cleanup failed\n");
        return 20;
    }
    printf("OK: canonical identity restored; reapply properties/settings separately\n");
    return 0;
}

static int cmd_verify(const char* option, const char* run_id,
                      const char* digest_option, const char* expected_digest) {
    const bool have_run = option != nullptr;
    const bool have_digest = digest_option != nullptr;
    if ((have_run && (strcmp(option, "--run-id") || !run_id ||
                      !valid_run_id(run_id))) ||
        (have_digest && (strcmp(digest_option, "--identity-sha256") ||
                         !expected_digest ||
                         !sbxtxn::valid_sha256(expected_digest))) ||
        (!have_run && run_id) || (!have_digest && expected_digest)) {
        fprintf(stderr, "Usage: sandboxid verify [--run-id <run-id>] "
                        "[--identity-sha256 <sha256>]\n");
        return 64;
    }
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity identity;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(identity, meta, error)) {
        fprintf(stderr, "! canonical identity state rejected: %s\n", error.c_str());
        return 1;
    }
    if (have_run && (meta.legacy || meta.run != run_id)) {
        fprintf(stderr, "! canonical identity metadata does not match run\n");
        return 1;
    }
    if (have_digest && (meta.legacy || meta.identity_sha256 != expected_digest)) {
        fprintf(stderr, "! canonical identity metadata does not match expected digest\n");
        return 1;
    }
    OverlaySet expected;
    if (!build_mount_files(identity, expected)) {
        fprintf(stderr, "! canonical overlay generation failed\n");
        return 1;
    }
    struct stat root_status{};
    if (::stat(MOUNTDIR, &root_status) != 0 || !S_ISDIR(root_status.st_mode) ||
        (root_status.st_mode & 0777) != 0755) {
        fprintf(stderr, "! overlay root is missing or has an unexpected mode\n");
        return 1;
    }
    for (size_t i = 0; i < sandboxid::MOUNT_PARTS_N; ++i) {
        const std::string part = sandboxid::MOUNT_PARTS[i];
        const std::string directory = std::string(MOUNTDIR) + "/" + part;
        std::string path = directory + "/build.prop";
        struct stat directory_status{};
        if (::stat(directory.c_str(), &directory_status) != 0 ||
            !S_ISDIR(directory_status.st_mode) ||
            (directory_status.st_mode & 0777) != 0755) {
            fprintf(stderr, "! overlay directory has an unexpected mode: %s\n",
                    directory.c_str());
            return 1;
        }
        struct stat status{};
        auto content = expected.files.find(part);
        if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
            status.st_size <= 0 || content == expected.files.end() ||
            read_file(path) != content->second) {
            fprintf(stderr, "! overlay does not match canonical identity: %s\n", path.c_str());
            return 1;
        }
        if ((status.st_mode & 0777) != 0644) {
            fprintf(stderr, "! overlay has unexpected mode: %s\n", path.c_str());
            return 1;
        }
    }
    printf("OK: canonical identity and overlay set validate\n");
    return 0;
}

static int cmd_persona_import(const char* candidate_path, const char* meta_path) {
    if (!ensure_root()) return 1;
    if (!candidate_path || !meta_path) {
        fprintf(stderr, "Usage: sandboxid persona-import <candidate> <meta>\n");
        return 64;
    }
    std::string candidate = read_file(candidate_path);
    std::string metadata = read_file(meta_path);
    if (candidate.empty() || candidate.size() > sandboxid::MAX_IDENTITY_BLOB ||
        metadata.empty() || metadata.size() > 16u * 1024u) {
        fprintf(stderr, "! candidate or provenance metadata is empty/oversized\n");
        return 64;
    }
    PixelEntry persona;
    std::string error;
    const int runtime_sdk = device_sdk();
    if (runtime_sdk <= 0) {
        fprintf(stderr, "! runtime SDK is unavailable\n");
        return 64;
    }
    if (!sbxpersona::parse_candidate(candidate, runtime_sdk, persona, error)) {
        fprintf(stderr, "! persona candidate rejected: %s\n", error.c_str());
        return 64;
    }
    const std::string canonical_candidate = sbxpersona::serialize_line(persona);
    if (!valid_provenance_meta(metadata, runtime_sdk, canonical_candidate, error)) {
        fprintf(stderr, "! provenance metadata rejected: %s\n", error.c_str());
        return 64;
    }
    Identity test = derive_identity(persona);
    if (!validate_identity(test, error)) {
        fprintf(stderr, "! derived candidate identity rejected: %s\n", error.c_str());
        return 64;
    }
    StateLock lock;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    PairReplaceResult imported = replace_pair(
        PERSONA_CACHE, canonical_candidate, PERSONA_CACHE_META, metadata);
    if (imported != PairReplaceResult::Committed) {
        fprintf(stderr, imported == PairReplaceResult::FailedDegraded
                    ? "! persona cache replacement failed and previous cache pair is degraded\n"
                    : "! validated persona cache could not be replaced; previous cache preserved\n");
        return imported == PairReplaceResult::FailedDegraded ? 32 : 1;
    }
    printf("OK: validated exact-SDK persona cache imported\n");
    return 0;
}

static int cmd_freshen() {
    DBG("cmd_freshen: build=%s", SBX_VARIANT_TAG);
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    std::string run_id;
    int prepared = prepare_locked(run_id);
    if (prepared != 0) return prepared;
    int committed = commit_locked(run_id.c_str());
    if (committed != 0) return committed;
    Identity identity;
    sbxtxn::CanonicalMeta canonical;
    if (!load_current_identity_state(identity, canonical, error)) {
        fprintf(stderr, "! identity state rejected: %s\n", error.c_str());
        return 1;
    }
    int properties = apply_properties(identity);
    int settings = apply_framework_settings(identity);
    printf(properties == 0 && settings == 0
               ? "OK - compatibility freshen committed and applied (targets not wiped)\n"
               : "PARTIAL - identity committed, but apply was incomplete\n");
    return properties == 0 && settings == 0 ? 0 : 20;
}

static int cmd_status() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    if (!path_exists(IDENTITY_FILE) && !path_exists(IDENTITY_META)) {
        printf("no identity yet - run `sandboxid freshen`\n");
        return 0;
    }
    const std::string serialized = read_file(IDENTITY_FILE);
    Identity identity;
    if (!load_identity_file(IDENTITY_FILE, identity, error)) {
        fprintf(stderr, "! canonical identity rejected: %s\n", error.c_str());
        return 1;
    }
    sbxtxn::CanonicalMeta meta;
    if (!load_canonical_meta(IDENTITY_META, serialized, meta, error)) {
        fprintf(stderr, "! canonical identity metadata rejected: %s\n", error.c_str());
        return 1;
    }
    fputs(serialized.c_str(), stdout);
    return 0;
}

static bool operational_flag(const char* key) {
    return key && sbxid::operational_flag_key(key);
}

static int publish_canonical_identity(const Identity& identity,
                                      sbxtxn::CanonicalMeta meta,
                                      std::string& error) {
    const std::string serialized = identity.serialize();
    if (serialized.empty()) {
        error = "canonical identity serialization failed";
        return 1;
    }
    meta.identity_sha256 = sbxhash::sha256(serialized);
    PairReplaceResult published = replace_pair(
        IDENTITY_FILE, serialized, IDENTITY_META, sbxtxn::serialize_meta(meta));
    if (published == PairReplaceResult::Committed) return 0;
    error = published == PairReplaceResult::FailedDegraded
        ? "canonical identity publication failed and rollback is unproven"
        : "canonical identity publication failed; prior state retained";
    return published == PairReplaceResult::FailedDegraded ? 32 : 1;
}

static bool load_current_identity_state(Identity& identity,
                                        sbxtxn::CanonicalMeta& meta,
                                        std::string& error) {
    const std::string serialized = read_file(IDENTITY_FILE);
    if (!load_identity_file(IDENTITY_FILE, identity, error)) return false;
    if (!path_exists(IDENTITY_META)) {
        error = "canonical metadata is missing";
        return false;
    }
    return load_canonical_meta(IDENTITY_META, serialized, meta, error);
}

static int cmd_set_flag(const char* key, const char* value) {
    if (!ensure_root()) return 1;
    if (!key || !value || !operational_flag(key) ||
        (strcmp(value, "0") && strcmp(value, "1"))) {
        fprintf(stderr, "Usage: sandboxid set-flag <SBX_* flag> <0|1>\n");
        return 64;
    }
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity id;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(id, meta, error)) {
        fprintf(stderr, "! canonical identity state rejected: %s\n", error.c_str());
        return 1;
    }
    id.kv[key] = value;
    if (!validate_identity(id, error)) {
        fprintf(stderr, "! updated identity rejected: %s\n", error.c_str());
        return 1;
    }
    int published = publish_canonical_identity(id, meta, error);
    if (published != 0) {
        fprintf(stderr, "! %s\n", error.c_str());
        return published;
    }
    printf("OK: %s=%s (restart target app to apply)\n", key, value);
    return 0;
}

static bool local_identity_key(const char* key) {
    if (!key) return false;
    return !strcmp(key, "GOOGLE_AID") || !strcmp(key, "WIFI_MAC") ||
           !strcmp(key, "BLUETOOTH_ADDR") || !strcmp(key, "BLUETOOTH_NAME") ||
           !strcmp(key, "BOOT_COUNT");
}

static int cmd_set_local(const char* key, const char* value) {
    if (!ensure_root()) return 1;
    if (!local_identity_key(key) || !value || !*value) {
        fprintf(stderr, "Usage: sandboxid set-local <GOOGLE_AID|WIFI_MAC|BLUETOOTH_ADDR|BLUETOOTH_NAME|BOOT_COUNT> <value>\n");
        return 64;
    }
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity identity;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(identity, meta, error)) {
        fprintf(stderr, "! canonical identity state rejected: %s\n", error.c_str());
        return 1;
    }
    identity.kv[key] = value;
    if (!validate_identity(identity, error)) {
        fprintf(stderr, "! local identity update rejected: %s\n", error.c_str());
        return 64;
    }
    int published = publish_canonical_identity(identity, meta, error);
    if (published != 0) {
        fprintf(stderr, "! %s\n", error.c_str());
        return published;
    }
    printf("OK: %s updated atomically\n", key);
    return 0;
}

static int cmd_carrier(const char* operation, const char* candidate_path) {
    if (!ensure_root()) return 1;
    if (!operation || (strcmp(operation, "apply") && strcmp(operation, "disable"))) {
        fprintf(stderr, "Usage: sandboxid carrier <apply <candidate>|disable>\n");
        return 64;
    }
    std::string config;
    sbxcarrier::CarrierSel selection;
    if (!strcmp(operation, "apply")) {
        if (!candidate_path || !(config = read_file(candidate_path)).size()) {
            fprintf(stderr, "! carrier candidate is empty or unreadable\n");
            return 64;
        }
        selection = sbxcarrier::parse_carrier_conf(config);
        if (!selection.valid) {
            fprintf(stderr, "! carrier candidate is invalid\n");
            return 64;
        }
    }
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity identity;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(identity, meta, error)) {
        fprintf(stderr, "! canonical identity state rejected: %s\n", error.c_str());
        return 1;
    }
    sbxcarrier::apply_carrier(identity.kv, selection);
    if (!validate_identity(identity, error)) {
        fprintf(stderr, "! carrier identity update rejected: %s\n", error.c_str());
        return 64;
    }
    std::string old_config = read_file(CARRIER_CONF);
    struct stat old_status{};
    const bool old_config_existed = ::stat(CARRIER_CONF, &old_status) == 0;
    const bool config_published = !strcmp(operation, "disable")
                                      ? remove_file_durable(CARRIER_CONF)
                                      : atomic_write(CARRIER_CONF, config);
    if (!config_published) {
        fprintf(stderr, "! failed to publish carrier configuration\n");
        return 1;
    }
    int identity_published = publish_canonical_identity(identity, meta, error);
    if (identity_published != 0) {
        bool restored = restore_path(CARRIER_CONF, old_config_existed, old_config);
        fprintf(stderr, restored
                    ? "! failed to publish carrier identity; configuration restored: %s\n"
                    : "! failed to publish carrier identity and restore configuration: %s\n",
                error.c_str());
        return !restored || identity_published == 32 ? 32 : identity_published;
    }
    printf("OK: carrier %s committed coherently\n", operation);
    return 0;
}

static int cmd_apply_props() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity id;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(id, meta, error)) {
        fprintf(stderr, "! identity state rejected: %s\n", error.c_str());
        return 1;
    }
    int rc = apply_properties(id);
    printf(rc == 0 ? "OK: presentation properties applied\n"
                   : "FAIL: one or more presentation properties failed\n");
    return rc;
}

static int cmd_apply_boot() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity id;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(id, meta, error)) {
        fprintf(stderr, "! identity state rejected: %s\n", error.c_str());
        return 1;
    }
    int settings_rc = apply_framework_settings(id);
    int overlays_rc = publish_generated_mount_files(id);
    if (settings_rc == 0 && overlays_rc == 0) {
        printf("OK: framework settings + mount overlay refreshed\n");
        return 0;
    }
    if (overlays_rc == 32) {
        fprintf(stderr, "! framework apply left overlay rollback unproven\n");
        return 32;
    }
    if (overlays_rc == 20) {
        fprintf(stderr, settings_rc == 0
                    ? "! framework settings applied, but overlay cleanup is incomplete\n"
                    : "! framework settings failed and overlay cleanup is incomplete\n");
        return 20;
    }
    printf("FAIL: framework settings or mount overlay incomplete\n");
    return 1;
}

static int publish_seed_state(const Identity& identity,
                              sbxtxn::CanonicalMeta meta,
                              std::string& error) {
    OverlaySet overlays;
    if (!build_mount_files(identity, overlays)) {
        error = "mount overlay generation failed";
        return 1;
    }
    OverlayPublication publication;
    if (!publish_mount_files(overlays, &publication)) {
        if (publication.activated && !restore_previous_mount_files(publication)) {
            error = "overlay publication failed and rollback is unproven";
            return 32;
        }
        error = "mount overlay publication failed";
        return 1;
    }
    const std::string serialized = identity.serialize();
    meta.identity_sha256 = sbxhash::sha256(serialized);
    PairReplaceResult published = replace_pair(
        IDENTITY_FILE, serialized, IDENTITY_META, sbxtxn::serialize_meta(meta));
    if (published != PairReplaceResult::Committed) {
        bool overlays_restored = restore_previous_mount_files(publication);
        if (published == PairReplaceResult::FailedDegraded || !overlays_restored) {
            error = "canonical publication failed and rollback is unproven";
            return 32;
        }
        error = "canonical publication failed; prior state retained";
        return 1;
    }
    if (!finish_overlay_publication(publication)) {
        error = "canonical state published but overlay cleanup failed";
        return 20;
    }
    return 0;
}

static int cmd_seed() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity identity;
    sbxtxn::CanonicalMeta metadata;
    const bool have_identity = path_exists(IDENTITY_FILE);
    const bool have_metadata = path_exists(IDENTITY_META);
    std::string existing = read_file(IDENTITY_FILE);
    if (have_identity || have_metadata) {
        if (!have_identity || existing.empty()) {
            fprintf(stderr, "! seed: canonical identity/metadata pair is incomplete\n");
            return 1;
        }
        bool needs_migration = false;
        if (!load_identity_file(IDENTITY_FILE, identity, error, &needs_migration)) {
            fprintf(stderr, "! seed: existing identity rejected: %s\n", error.c_str());
            return 1;
        }
        sbxtxn::CanonicalMeta current_meta;
        if (!have_metadata) {
            current_meta.legacy = true;
            current_meta.source = "legacy";
            current_meta.identity_sha256 = sbxhash::sha256(existing);
        } else if (!load_canonical_meta(
                       IDENTITY_META, existing, current_meta, error)) {
            fprintf(stderr, "! seed: canonical metadata rejected: %s\n", error.c_str());
            return 1;
        }
        metadata = current_meta;
        DBG("seed: reusing existing identity (%zu keys%s)", identity.kv.size(),
            needs_migration ? ", legacy fields removed" : "");
    } else {
        DBG("seed: no identity yet, generating fresh");
        sbxpersona::Source source = sbxpersona::Source::None;
        if (!gen_identity(identity, error, &source)) {
            fprintf(stderr, "! seed: cannot generate identity: %s\n", error.c_str());
            return 1;
        }
        merge_carrier(identity);
        if (!validate_identity(identity, error)) {
            fprintf(stderr, "! seed: generated identity rejected: %s\n", error.c_str());
            return 1;
        }
        metadata.run = make_run_id();
        metadata.source = sbxpersona::source_name(source);
    }
    int published = publish_seed_state(identity, metadata, error);
    if (published != 0) {
        fprintf(stderr, "! seed: %s\n", error.c_str());
        return published;
    }
    printf("OK: seed complete (canonical metadata + mount overlay ready at %s)\n",
           MOUNTDIR);
    return 0;
}

static int cmd_lock() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    if (!atomic_write(MODE_FILE, "locked\n")) {
        fprintf(stderr, "! failed to persist locked mode\n");
        return 1;
    }
    printf("OK: locked\n");
    return 0;
}

static int cmd_unlock() {
    if (!ensure_root()) return 1;
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    if (!atomic_write(MODE_FILE, "fresh\n")) {
        fprintf(stderr, "! failed to persist fresh mode\n");
        return 1;
    }
    printf("OK: unlocked\n");
    return 0;
}

static int cmd_rollback() {
    fprintf(stderr, "* rollback is deprecated; restoring canonical state without target wipe\n");
    return cmd_restore();
}

static int cmd_applog_ids(const char* pkg) {
    if (!ensure_root()) return 1;
    if (!pkg || !*pkg) {
        fprintf(stderr, "applog-ids: butuh nama package\n");
        return 64;
    }
    std::string normalized_package;
    if (!sbxtarget::split_process(pkg, normalized_package) || normalized_package != pkg) {
        fprintf(stderr, "! applog-ids requires a valid base package name\n");
        return 64;
    }
    StateLock lock;
    std::string error;
    if (!lock.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    Identity id;
    sbxtxn::CanonicalMeta meta;
    if (!load_current_identity_state(id, meta, error)) {
        fprintf(stderr, "! identity state rejected: %s\n", error.c_str());
        return 1;
    }
    auto g = [&](const char* k) -> std::string {
        auto it = id.kv.find(k);
        return it != id.kv.end() ? it->second : std::string();
    };
    uint64_t epoch_ms = strtoull(g("APPLOG_EPOCH").c_str(), nullptr, 10);
    if (epoch_ms == 0) epoch_ms = 1700000000000ULL;
    uint64_t seed = sbxnr::fnv1a(g("FINGERPRINT") + "|" + g("SERIAL") + "|" +
                                 g("ANDROID_ID") + "|" + std::string(pkg));
    sbxnr::ApplogIds ids = sbxnr::make_applog_ids(seed, epoch_ms);
    printf("PKG=%s\n",        pkg);
    printf("EPOCH=%llu\n",    (unsigned long long)epoch_ms);
    printf("DID=%s\n",        ids.did.c_str());
    printf("IID=%s\n",        ids.iid.c_str());
    printf("SSID=%s\n",       ids.ssid.c_str());
    printf("OPENUDID=%s\n",   ids.openudid.c_str());
    printf("CLIENTUDID=%s\n", ids.clientudid.c_str());
    printf("CDID=%s\n",       ids.cdid.c_str());
    return 0;
}

static void usage(const char* p) {
    fprintf(stderr,
        "SandboxID — Android device identifier privacy research module\n\n"
        "Usage: %s <command>\n\n"
        "  prepare [run-id]\n"
        "               Generate and validate pending identity without mutation\n"
        "  commit <run-id>\n"
        "               Publish a matching pending identity and checked overlays\n"
        "  abort <run-id>\n"
        "               Remove a matching pending transaction\n"
        "  restore      Restore backup identity/overlays without wiping targets\n"
        "  verify [--run-id <run-id>] [--identity-sha256 <sha256>]\n"
        "               Validate canonical identity and overlay set\n"
        "  persona-import <candidate> <meta>\n"
        "               Validate and atomically replace persistent persona cache\n"
        "  action-write <state|result> <source>\n"
        "               Durably publish one bounded Action state/result file\n"
        "  freshen      Compatibility prepare/commit/apply (does not wipe targets)\n"
        "  status       Print current identity.prop\n"
        "  set-flag <key> <0|1>\n"
        "               Set one operational SBX_* flag atomically\n"
        "  set-local <key> <value>\n"
        "               Validate and atomically update a local rotation value\n"
        "  carrier apply <candidate>|disable\n"
        "               Commit validated carrier config and identity state\n"
        "  rollback     Deprecated alias for restore; never wipes targets\n"
        "  lock         Prevent freshen (safety)\n"
        "  unlock       Re-enable freshen\n"
        "  apply-props  Apply presentation properties before Zygote\n"
        "  apply-boot   Apply post-boot framework Settings + refresh overlays\n"
        "  seed         Fast bootstrap: validate/generate identity + mount overlay\n"
        "               (used by post-fs-data.sh before apply-props)\n"
        "  targets [--processes|--packages]\n"
        "               Print validated exact processes or normalized packages\n"
        "  applog-ids <pkg>\n"
        "               Print the AppLog IDs the L9 hook serves for <pkg>\n",
        p);
}

static int run_mutation(int (*command)()) {
    std::string error;
    MutationGuard mutation;
    if (!mutation.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    return command();
}

static int run_mutation_arg(int (*command)(const char*), const char* arg) {
    std::string error;
    MutationGuard mutation;
    if (!mutation.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    return command(arg);
}

static int run_mutation_args(int (*command)(const char*, const char*),
                             const char* first, const char* second) {
    std::string error;
    MutationGuard mutation;
    if (!mutation.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    return command(first, second);
}

static int run_mutation_verify(int argc, char** argv) {
    const char* run_id = nullptr;
    const char* expected_digest = nullptr;
    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return cmd_verify(argv[index], nullptr, nullptr, nullptr);
        }
        if (!strcmp(argv[index], "--run-id") && !run_id) {
            run_id = argv[index + 1];
        } else if (!strcmp(argv[index], "--identity-sha256") &&
                   !expected_digest) {
            expected_digest = argv[index + 1];
        } else {
            return cmd_verify(argv[index], argv[index + 1], nullptr, nullptr);
        }
    }
    std::string error;
    MutationGuard mutation;
    if (!mutation.acquire(error)) {
        fprintf(stderr, "! %s\n", error.c_str());
        return 75;
    }
    return cmd_verify(run_id ? "--run-id" : nullptr, run_id,
                      expected_digest ? "--identity-sha256" : nullptr,
                      expected_digest);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char* c = argv[1];
    if (!strcmp(c, "prepare"))    return run_mutation_arg(cmd_prepare, argc > 2 ? argv[2] : nullptr);
    if (!strcmp(c, "commit"))     return run_mutation_arg(cmd_commit, argc > 2 ? argv[2] : nullptr);
    if (!strcmp(c, "abort"))      return run_mutation_arg(cmd_abort, argc > 2 ? argv[2] : nullptr);
    if (!strcmp(c, "restore"))    return run_mutation(cmd_restore);
    if (!strcmp(c, "verify"))     return run_mutation_verify(argc, argv);
    if (!strcmp(c, "persona-import"))
        return run_mutation_args(cmd_persona_import, argc > 2 ? argv[2] : nullptr,
                                 argc > 3 ? argv[3] : nullptr);
    if (!strcmp(c, "action-write"))
        return cmd_action_write(argc > 2 ? argv[2] : nullptr,
                                argc > 3 ? argv[3] : nullptr);
    if (!strcmp(c, "freshen"))    return run_mutation(cmd_freshen);
    if (!strcmp(c, "status"))     return cmd_status();
    if (!strcmp(c, "set-flag"))   return run_mutation_args(cmd_set_flag,
                                                            argc > 2 ? argv[2] : nullptr,
                                                            argc > 3 ? argv[3] : nullptr);
    if (!strcmp(c, "set-local"))  return run_mutation_args(cmd_set_local,
                                                            argc > 2 ? argv[2] : nullptr,
                                                            argc > 3 ? argv[3] : nullptr);
    if (!strcmp(c, "carrier"))    return run_mutation_args(cmd_carrier,
                                                            argc > 2 ? argv[2] : nullptr,
                                                            argc > 3 ? argv[3] : nullptr);
    if (!strcmp(c, "rollback"))   return run_mutation(cmd_rollback);
    if (!strcmp(c, "lock"))       return run_mutation(cmd_lock);
    if (!strcmp(c, "unlock"))     return run_mutation(cmd_unlock);
    if (!strcmp(c, "apply-props")) return run_mutation(cmd_apply_props);
    if (!strcmp(c, "apply-boot")) return run_mutation(cmd_apply_boot);
    if (!strcmp(c, "seed"))       return run_mutation(cmd_seed);
    if (!strcmp(c, "targets"))    return cmd_targets(argc > 2 ? argv[2] : nullptr);
    if (!strcmp(c, "applog-ids")) return cmd_applog_ids(argc > 2 ? argv[2] : nullptr);
    usage(argv[0]);
    return 1;
}
