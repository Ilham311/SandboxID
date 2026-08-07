

#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/prctl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <sys/types.h>

namespace tt {

inline bool write_all(int fd, const void* buf, size_t n) {
    const auto* p = static_cast<const unsigned char*>(buf);
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::write(fd, p + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        sent += (size_t)w;
    }
    return true;
}

inline bool read_all(int fd, void* buf, size_t n) {
    auto* p = static_cast<unsigned char*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        got += (size_t)r;
    }
    return true;
}

inline void close_all_fds_except(std::initializer_list<int> keep) {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return;
    struct dirent* e;
    int dirfd_self = ::dirfd(d);
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        int fd = ::atoi(e->d_name);
        if (fd <= 2) continue;
        if (fd == dirfd_self) continue;
        bool skip = false;
        for (int k : keep) if (fd == k) { skip = true; break; }
        if (skip) continue;
        ::close(fd);
    }
    ::closedir(d);
}

inline void child_init(const char* name, std::initializer_list<int> keep_fds = {}) {
    if (name && *name) {
        ::prctl(PR_SET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0);
    }
    close_all_fds_except(keep_fds);
}

}
