#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "traffic.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINKS 16
typedef struct { char name[32]; uint64_t index, rx, tx; } Link;
typedef struct {
    char boot[64]; uint64_t sequence, checkpoint, since, received, sent;
    unsigned count; Link links[MAX_LINKS];
} State;
static const char *setting(const char *name, const char *fallback) {
    const char *v = getenv(name); return v && *v ? v : fallback;
}
static int join(char *out, size_t size, const char *dir, const char *name) {
    int n = snprintf(out, size, "%s/%s", dir, name); return n > 0 && (size_t)n < size;
}
static int directory(const char *p) {
    struct stat s;
    if (mkdir(p, 0700) && errno != EEXIST) return 0;
    return !lstat(p, &s) && S_ISDIR(s.st_mode) && s.st_uid == geteuid() && !(s.st_mode & 0077);
}
static int private_fd(const char *p, int flags) {
    int fd = open(p, flags | O_NOFOLLOW | O_CLOEXEC, 0600); struct stat s;
    if (fd < 0) return -1;
    if (fstat(fd, &s) || !S_ISREG(s.st_mode) || s.st_uid != geteuid() || (s.st_mode & 0077) || s.st_nlink != 1) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}
/* QDC507 rmnet0 is the shared transport of rmnet_dataN: its TX counter matches
 * rmnet_data0 byte for byte. Count logical SIM contexts only, never the parent. */
static int data_name(const char *name) {
    const char *p;
    if (!strncmp(name, "rmnet_data", 10)) p = name + 10;
    else return 0;
    if (!*p || strlen(name) >= 32) return 0;
    for (; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}
static int number(const char *file, uint64_t *out) {
    FILE *f = fopen(file, "r"); if (!f) return 0;
    char b[64], *end; int ok = fgets(b, sizeof b, f) != NULL; fclose(f);
    if (!ok || b[0] < '0' || b[0] > '9') return 0;
    errno = 0; *out = strtoull(b, &end, 10);
    while (*end == '\n' || *end == ' ' || *end == '\t') end++;
    return !errno && !*end;
}
static int snapshot(Link *links, unsigned *count) {
    const char *root = "/sys/class/net";
#ifdef TRAFFIC_TEST
    root = setting("QDC_TRAFFIC_SYS", root);
#endif
    DIR *d = opendir(root); if (!d) return 0;
    *count = 0; struct dirent *e; int ok = 1;
    while ((e = readdir(d))) {
        if (!data_name(e->d_name)) continue;
        if (*count == MAX_LINKS) { ok = 0; break; }
        Link link = {0}; char dir[768], path[832];
        if (!join(dir, sizeof dir, root, e->d_name)) { ok = 0; break; }
        /* A disappearing interface is retried on the next sample. */
        if (!join(path, sizeof path, dir, "ifindex") || !number(path, &link.index) || !link.index ||
            !join(path, sizeof path, dir, "statistics/rx_bytes") || !number(path, &link.rx) ||
            !join(path, sizeof path, dir, "statistics/tx_bytes") || !number(path, &link.tx)) continue;
        snprintf(link.name, sizeof link.name, "%s", e->d_name); links[(*count)++] = link;
    }
    closedir(d); return ok;
}
static int boot_id(char *out, size_t size) {
    char file[768];
    if (!join(file, sizeof file, setting("QDC_WEB_PROC_ROOT", "/proc"), "sys/kernel/random/boot_id")) return 0;
    FILE *f = fopen(file, "r"); if (!f) return 0;
    int ok = fgets(out, (int)size, f) != NULL; fclose(f);
    if (!ok) return 0;
    out[strcspn(out, "\r\n")] = 0;
    if (strlen(out) != 36) return 0;
    for (unsigned i = 0; i < 36; i++) {
        char c = out[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}
/* 0 = absent, 1 = valid, -1 = damaged/unsafe. Never silently discard totals. */
static int load_state(const char *file, State *s) {
    int fd = private_fd(file, O_RDONLY); if (fd < 0) return errno == ENOENT ? 0 : -1;
    FILE *f = fdopen(fd, "r"); if (!f) { close(fd); return -1; }
    char magic[16]; int ok = fscanf(f, "%15s %63s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %u",
        magic, s->boot, &s->sequence, &s->checkpoint, &s->since, &s->received, &s->sent, &s->count) == 8;
    ok = ok && !strcmp(magic, "traffic-v1") && strlen(s->boot) == 36 && s->count <= MAX_LINKS;
    for (unsigned i = 0; ok && i < s->count; i++) {
        Link *l = &s->links[i];
        ok = fscanf(f, "%31s %" SCNu64 " %" SCNu64 " %" SCNu64, l->name, &l->index, &l->rx, &l->tx) == 4 &&
            (data_name(l->name) || !strcmp(l->name, "rmnet0")) && l->index;
        for (unsigned j = 0; ok && j < i; j++) if (!strcmp(l->name, s->links[j].name)) ok = 0;
    }
    char extra; if (ok && fscanf(f, " %c", &extra) != EOF) ok = 0;
    if (ferror(f)) ok = 0;
    fclose(f); return ok ? 1 : -1;
}
/* The caller holds traffic.lock. Remove only our private regular temporary
 * files, including the old PID-only names left by an interrupted checkpoint.
 * A symlink, hardlink or unexpectedly public file is never followed/deleted. */
static void clean_temporary(const char *file) {
    char parent[832], prefix[128];
    snprintf(parent, sizeof parent, "%s", file);
    char *slash = strrchr(parent, '/'); if (!slash) return;
    int n = snprintf(prefix, sizeof prefix, "%s.tmp.", slash + 1);
    if (n <= 0 || (size_t)n >= sizeof prefix) return;
    *slash = 0;
    DIR *dir = opendir(parent); if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, prefix, (size_t)n)) continue;
        const char *suffix = e->d_name + n; size_t length = strlen(suffix);
        int numeric = length > 0, alphanumeric = length == 6;
        for (size_t i = 0; i < length; i++) {
            char c = suffix[i];
            if (c < '0' || c > '9') numeric = 0;
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) alphanumeric = 0;
        }
        if (!numeric && !alphanumeric) continue;
        struct stat st;
        if (!fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) &&
            S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077) && st.st_nlink == 1)
            unlinkat(dirfd(dir), e->d_name, 0);
    }
    closedir(dir);
}
static int save_state(const char *file, const State *s, int durable) {
    char tmp[832]; int n = snprintf(tmp, sizeof tmp, "%s.tmp.XXXXXX", file);
    if (n <= 0 || (size_t)n >= sizeof tmp) return 0;
    clean_temporary(file);
    int fd = mkstemp(tmp); if (fd < 0) return 0;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC)) { int error = errno; close(fd); unlink(tmp); errno = error; return 0; }
    FILE *f = fdopen(fd, "w"); if (!f) { close(fd); unlink(tmp); return 0; }
    int ok = fprintf(f, "traffic-v1 %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %u\n",
        s->boot, s->sequence, s->checkpoint, s->since, s->received, s->sent, s->count) > 0;
    for (unsigned i = 0; ok && i < s->count; i++) {
        const Link *l = &s->links[i];
        ok = fprintf(f, "%s %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", l->name, l->index, l->rx, l->tx) > 0;
    }
    if (fflush(f) || (durable && fsync(fd))) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok && rename(tmp, file)) ok = 0;
    if (!ok) unlink(tmp);
    if (ok && durable) {
        char dir[832]; snprintf(dir, sizeof dir, "%s", file); char *slash = strrchr(dir, '/');
        if (!slash) return 0;
        *slash = 0; int d = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (d < 0) return 0;
        ok = fsync(d) == 0; close(d);
    }
    return ok;
}
static uint64_t add(uint64_t a, uint64_t b) { return UINT64_MAX - a < b ? UINT64_MAX : a + b; }
/* Ordinary CGI/background samples remain nonblocking. The final checkpoint
 * waits up to 1.5 seconds for a short-lived concurrent sampler to finish. */
static int state_lock(int fd, int flush) {
    struct timespec started, current;
    if (flush && clock_gettime(CLOCK_MONOTONIC, &started)) return 0;
    for (;;) {
        if (!flock(fd, LOCK_EX | LOCK_NB)) return 1;
        if (!flush || (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)) return 0;
        if (clock_gettime(CLOCK_MONOTONIC, &current)) return 0;
        int64_t elapsed = (int64_t)(current.tv_sec - started.tv_sec) * 1000 +
                          (current.tv_nsec - started.tv_nsec) / 1000000;
        if (elapsed >= 1500) { errno = ETIMEDOUT; return 0; }
        struct timespec pause = {0, 25000000};
        nanosleep(&pause, NULL);
    }
}
int qdc_traffic_update(QdcTraffic *result, int reset, int flush) {
    char private_dir[768], disk[832], ram[832], lock[832], boot[64];
    const char *root = setting("QDC_WEB_ROOT", "/usrdata/qdc-web");
    const char *run = setting("QDC_WEB_RUN", "/run/qdc-web");
    if (!join(private_dir, sizeof private_dir, root, "private") || !directory(private_dir) || !directory(run) ||
        !join(disk, sizeof disk, private_dir, "traffic") || !join(ram, sizeof ram, run, "traffic") ||
        !join(lock, sizeof lock, run, "traffic.lock") || !boot_id(boot, sizeof boot)) return 0;
    int fd = private_fd(lock, O_RDWR | O_CREAT); if (fd < 0) return 0;
    if (!state_lock(fd, flush)) { int error = errno; close(fd); errno = error; return 0; }
    State s = {0}, saved = {0}; int loaded = load_state(ram, &s), persisted = load_state(disk, &saved);
    if (loaded < 0 || persisted < 0) { close(fd); errno = EINVAL; return 0; }
    /* A reset/checkpoint can commit to flash before its RAM rename. Prefer
     * the newer revision so a killed CGI cannot resurrect pre-reset totals. */
    if (persisted && (!loaded || saved.sequence > s.sequence)) { s = saved; loaded = 1; }
    Link links[MAX_LINKS]; unsigned count;
    if (!snapshot(links, &count)) { close(fd); return 0; }
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC, &ts)) { close(fd); return 0; }
    uint64_t now = (uint64_t)ts.tv_sec;
#ifdef TRAFFIC_TEST
    now = strtoull(setting("QDC_TRAFFIC_TIME", "100"), NULL, 10);
#endif
    int new_boot = loaded && strcmp(s.boot, boot);
    if (!loaded) s.since = (uint64_t)time(NULL);
    for (unsigned i = 0; i < count; i++) {
        Link *l = &links[i], *old = NULL;
        for (unsigned j = 0; !new_boot && j < s.count; j++) if (!strcmp(l->name, s.links[j].name)) old = &s.links[j];
        uint64_t rx = l->rx, tx = l->tx;
        if (old && old->index == l->index) {
            rx = l->rx >= old->rx ? l->rx - old->rx : l->rx;
            tx = l->tx >= old->tx ? l->tx - old->tx : l->tx;
        }
        if (loaded) { s.received = add(s.received, rx); s.sent = add(s.sent, tx); }
    }
    /* Keep baselines for temporarily absent interfaces within this boot. */
    if (new_boot) s.count = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned j; for (j = 0; j < s.count; j++) if (!strcmp(links[i].name, s.links[j].name)) break;
        if (j == MAX_LINKS) { close(fd); return 0; }
        s.links[j] = links[i]; if (j == s.count) s.count++;
    }
    snprintf(s.boot, sizeof s.boot, "%s", boot);
    if (s.sequence == UINT64_MAX) { close(fd); errno = EOVERFLOW; return 0; }
    s.sequence++;
    if (reset) { s.received = s.sent = 0; s.since = (uint64_t)time(NULL); }
    int durable = !loaded || new_boot || reset || flush || now < s.checkpoint || now - s.checkpoint >= 30;
    if (durable) {
        s.checkpoint = now;
        if (!save_state(disk, &s, 1)) { close(fd); return 0; }
    }
    int ok = save_state(ram, &s, 0);
    if (ok && result) *result = (QdcTraffic){s.received, s.sent, s.since, count};
    close(fd); return ok;
}
