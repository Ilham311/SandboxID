#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dlfcn.h>

static const char* PATH =
    "/data/data/com.termux/files/home/SandboxID/tests/host/sbx_fake.so";
static const char* dir  =
    "/data/data/com.termux/files/home/SandboxID/tests/host";

static int find_in_maps(unsigned* maj, unsigned* min, unsigned long* ino) {
    FILE* m = fopen("/proc/self/maps", "r");
    if (!m) return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof line, m)) {
        if (strstr(line, "sbx_fake.so")) {
            unsigned long a, b, off; char perm[8];
            if (sscanf(line, "%lx-%lx %s %lx %x:%x %lu", &a, &b, perm, &off,
                       maj, min, ino) == 7)
                found = 1;
            break;
        }
    }
    fclose(m);
    return found;
}

int main(void) {
    /* build a REAL shared object so dlopen actually maps it */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "gcc -shared -fPIC -o %s /dev/null 2>/dev/null", PATH);
    if (system(cmd) != 0) { printf("build v1 failed\n"); return 1; }
    struct stat before; stat(PATH, &before);
    printf("on-disk inode before dlopen : %llu  dev=%llu (major=%u minor=%u)\n",
           (unsigned long long)before.st_ino, (unsigned long long)before.st_dev,
           (unsigned)major(before.st_dev), (unsigned)minor(before.st_dev));

    void* h = dlopen(PATH, RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    printf("dlopen                       : ok (mapped)\n");

    /* replace the file on disk with a different, valid .so while it stays mapped */
    snprintf(cmd, sizeof cmd,
             "echo 'int sbx_marker_v2(void){return 2;}' > %s/sbx_v2.c && "
             "gcc -shared -fPIC -o %s %s/sbx_v2.c 2>/dev/null",
             dir, PATH, dir);
    if (system(cmd) != 0) { printf("build v2 failed\n"); return 1; }
    struct stat after; stat(PATH, &after);
    printf("on-disk inode after replace : %llu  (changed: %s)\n",
           (unsigned long long)after.st_ino,
           after.st_ino != before.st_ino ? "YES" : "no");

    unsigned mmaj = 0, mmin = 0; unsigned long map_ino = 0;
    if (!find_in_maps(&mmaj, &mmin, &map_ino)) {
        printf("maps line not found\n"); return 1;
    }
    printf("maps line                    : dev=%x:%x inode=%lu\n",
           mmaj, mmin, map_ino);

    dev_t maps_dev = makedev(mmaj, mmin);
    printf("makedev(%x,%x) == st_dev      : %s\n", mmaj, mmin,
           maps_dev == after.st_dev
               ? "YES (maps-derived dev matches stat)"
               : "NO - encoding mismatch, naive parse would BREAK normal case");

    printf("\n=== VERDICT ===\n");
    printf("maps inode (what pltHookCommit matches)  : %lu\n", map_ino);
    printf("stat() inode (what module_hooks passes)  : %llu\n",
           (unsigned long long)after.st_ino);
    if (map_ino != after.st_ino) {
        printf("DIVERGENCE: REPRODUCED - a register-by-stat hook for this lib "
               "would match no mapped region and silently not land\n");
    } else {
        printf("DIVERGENCE: not reproduced\n");
    }
    /* also confirm the maps entry survives the on-disk replacement: the
       mapping is unchanged because the page cache object was pinned at mmap */
    unsigned mmaj2 = 0, mmin2 = 0; unsigned long map_ino2 = 0;
    find_in_maps(&mmaj2, &mmin2, &map_ino2);
    printf("maps inode re-read after replace          : %lu (stable: %s)\n",
           map_ino2, map_ino2 == map_ino ? "yes" : "no");
    dlclose(h);
    unlink(PATH);
    return 0;
}
