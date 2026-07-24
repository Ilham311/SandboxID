// ============================================================
// Ternak TT v1.0 — CLI (talks to companion via UDS abstract)
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>

#define UDS "ternak.tt.ctrl"
enum { FRESHEN=10, STATUS=11, APPLY_BOOT=12, LOCK=13, UNLOCK=14, ROLLBACK=15 };

static int connect_c(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX; a.sun_path[0] = 0;
    strncpy(a.sun_path + 1, UDS, sizeof(a.sun_path) - 2);
    socklen_t al = sizeof(sa_family_t) + 1 + strlen(UDS);
    if (connect(fd, (struct sockaddr*)&a, al) < 0) {
        fprintf(stderr, "! cannot connect @%s (%s)\n"
                        "! Zygisk enabled? Rebooted after install?\n",
                UDS, strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

static int reply_print(int fd) {
    uint32_t l = 0;
    if (read(fd, &l, sizeof(l)) != sizeof(l)) return 1;
    if (l > 65536) l = 65536;
    char* b = malloc(l + 1); if (!b) return 1;
    ssize_t got = 0;
    while ((size_t)got < l) {
        ssize_t n = read(fd, b + got, l - got);
        if (n <= 0) break;
        got += n;
    }
    b[got] = 0; fwrite(b, 1, got, stdout); free(b); return 0;
}

static int send_c(uint8_t c) {
    int fd = connect_c(); if (fd < 0) return 1;
    write(fd, &c, 1); int r = reply_print(fd); close(fd); return r;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Ternak TT v1.0 \xe2\x80\x94 TikTok Zygisk fresh persona\n\n"
        "Usage: %s <command>\n\n"
        "  freshen      Rotate identity + wipe TT app data (main action)\n"
        "  status       Print current identity.prop\n"
        "  rollback     Restore previous identity from backup\n"
        "  lock         Prevent freshen (safety)\n"
        "  unlock       Re-enable freshen\n"
        "  apply-boot   Re-apply native prop (used by service.sh)\n",
        p);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const char* c = argv[1];
    if (!strcmp(c, "freshen"))    return send_c(FRESHEN);
    if (!strcmp(c, "status"))     return send_c(STATUS);
    if (!strcmp(c, "rollback"))   return send_c(ROLLBACK);
    if (!strcmp(c, "lock"))       return send_c(LOCK);
    if (!strcmp(c, "unlock"))     return send_c(UNLOCK);
    if (!strcmp(c, "apply-boot")) return send_c(APPLY_BOOT);
    usage(argv[0]); return 1;
}
