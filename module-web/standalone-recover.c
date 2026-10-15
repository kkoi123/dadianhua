/* Temporary, authenticated recovery CGI. It only kills this installation's
 * phtunnel processes; no shell, reboot, driver, network or credential writes.
 * Remove from WWW immediately after recovering ADB access. */
#define main existing_api_main
#include "api.c"
#undef main

static int owned_tunnel(pid_t task) {
    char filename[128], executable[512], command[2048];
    snprintf(filename, sizeof filename, "/proc/%ld/exe", (long)task);
    ssize_t n = readlink(filename, executable, sizeof executable - 1);
    if (n <= 0 || n >= (ssize_t)sizeof executable - 1) return 0;
    executable[n] = 0;
    if (strcmp(executable, "/usrdata/qdc-web/standalone/lib/libc.so") &&
        strcmp(executable, "/usrdata/qdc-web/standalone/phtunnel")) return 0;
    snprintf(filename, sizeof filename, "/proc/%ld/cmdline", (long)task);
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, command, sizeof command); close(fd);
    if (n <= 0 || n >= (ssize_t)sizeof command || command[n - 1]) return 0;
    int program = 0, config = 0;
    for (ssize_t offset = 0; offset < n;) {
        const char *arg = command + offset;
        if (!strcmp(arg, "/usrdata/qdc-web/standalone/phtunnel")) program = 1;
        if (!strcmp(arg, "/usrdata/qdc-web/private/module-oray.json") || !strcmp(arg, "/run/qdc-web/module-oray.json")) config = 1;
        offset += (ssize_t)strlen(arg) + 1;
    }
    return program && config;
}
int main(void) {
    umask(077); alarm(10);
    if (!initialize_paths()) return 1;
    const char *method = getenv("REQUEST_METHOD");
    if (!method || strcmp(method, "POST")) { error_response(405, "POST required"); return 0; }
    if (!read_body()) { error_response(400, "Invalid request"); return 0; }
    Credential c; Session s; char sid[65];
    if (!authenticate(&c, &s, sid)) { error_response(401, "Login required"); return 0; }
    if (!safe_equal(field("csrfToken"), s.csrf) || strcmp(field("operation"), "stop-owned-tunnel")) {
        error_response(403, "Confirmation invalid"); return 0;
    }
    unsigned stopped = 0;
    for (unsigned pass = 0; pass < 4; pass++) {
        pid_t tasks[1024]; unsigned count = 0;
        DIR *directory = opendir("/proc"); if (!directory) break;
        struct dirent *entry;
        while ((entry = readdir(directory)) && count < 1024) {
            if (!*entry->d_name || strspn(entry->d_name, "0123456789") != strlen(entry->d_name)) continue;
            long value = strtol(entry->d_name, NULL, 10);
            if (value <= 1 || value > 1048576 || !owned_tunnel((pid_t)value)) continue;
            if (!kill((pid_t)value, SIGSTOP)) tasks[count++] = (pid_t)value;
        }
        closedir(directory);
        for (unsigned i = 0; i < count; i++) if (owned_tunnel(tasks[i]) && !kill(tasks[i], SIGKILL)) stopped++;
        if (!count) break;
    }
    headers(200, NULL); printf("{\"ok\":true,\"stopped\":%u}\n", stopped);
    memset(fields, 0, sizeof fields); return 0;
}
