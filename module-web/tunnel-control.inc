/* Application-owned tunnel policy. No SDK credentials or binding tokens leave
 * the module; only fixed service commands are invoked after authentication. */
static int tunnel_policy(void) {
    char p[768], b[8] = {0};
    if (!data_file(p, sizeof p, 1, "tunnel-policy")) return -1;
    int fd = safe_file(p, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    return n == 2 && b[1] == '\n' && (b[0] == '0' || b[0] == '1') ? b[0] - '0' : -1;
}
static int tunnel_job(void) {
    char p[768], b[64] = {0}, extra; unsigned long long started;
    if (!data_file(p, sizeof p, 0, "tunnel-job")) return 0;
    int fd = safe_file(p, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    uint64_t now = now_seconds();
    return n > 0 && sscanf(b, "%llu %c", &started, &extra) == 1 && started <= now && now - started < 60;
}
static int tunnel_configured(void) {
    char p[768];
    if (!data_file(p, sizeof p, 1, "module-oray.json")) return 0;
    int fd = safe_file(p, O_RDONLY); if (fd < 0) return 0;
    struct stat st; int nonempty = !fstat(fd, &st) && st.st_size > 0 && st.st_size < 8192; close(fd);
    if (!nonempty) return 0;
    return path(p, sizeof p, root, "standalone/service.sh") && access(p, X_OK) == 0;
}
static int tunnel_command(const char *command, char *out, size_t n) {
    char script[768];
    if (!path(script, sizeof script, root, "standalone/service.sh")) return 0;
    char *args[] = {"/bin/sh", script, (char *)command, NULL};
    return data_exec(args, out, n, 10);
}
static void tunnel_public_url(char out[256]) {
    char p[768], b[240] = {0}; out[0] = 0;
    if (!data_file(p, sizeof p, 1, "audio-origin")) return;
    int fd = safe_file(p, O_RDONLY); if (fd < 0) return;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 9 || n >= (ssize_t)sizeof b - 1 || b[n - 1] != '\n') return;
    b[n - 1] = 0;
    if (strncmp(b, "https://", 8) || b[8] == '.') return;
    for (char *s = b + 8; *s; s++) if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') return;
    snprintf(out, 256, "%s/module/", b);
}
static void tunnel_response(void) {
    int enabled = tunnel_policy(), pending = tunnel_job(), configured = tunnel_configured();
    char result[256], url[256]; const char *state = "unavailable";
    if (configured && tunnel_command("tunnel-status", result, sizeof result)) {
        if (!strcmp(result, "running\n")) state = "running";
        else if (!strcmp(result, "stopped\n")) state = "stopped";
        else if (!strcmp(result, "waiting\n")) state = "waiting";
        else if (!strcmp(result, "blocked\n")) state = "blocked";
    }
    int target = -1, last = -1; char p[768];
    if (data_file(p, sizeof p, 0, "tunnel-result")) {
        int fd = safe_file(p, O_RDONLY);
        if (fd >= 0) { FILE *f = fdopen(fd, "r"); if (f) { if (fscanf(f, "%d %d", &target, &last) != 2) last = -1; fclose(f); } else close(fd); }
    }
    if (tunnel_job()) pending = 1;
    tunnel_public_url(url);
    headers(200, NULL);
    printf("{\"ok\":true,\"enabled\":%s,\"configured\":%s,\"pending\":%s,\"state\":", enabled < 0 ? "null" : enabled ? "true" : "false", configured ? "true" : "false", pending ? "true" : "false");
    json_string(state); printf(",\"publicURL\":"); json_string(url);
    printf(",\"lastSuccess\":%s}\n", pending || last < 0 || target != enabled ? "null" : last ? "true" : "false");
}
static int tunnel_apply(void) {
    /* Let the acceptance response reach a public browser before disconnecting. */
    struct timespec delay = {2, 0}; nanosleep(&delay, NULL);
    int lock = -1;
    for (unsigned i = 0; i < 20 && lock < 0; i++) { lock = data_lock(); if (lock < 0) { struct timespec pause = {0, 100000000}; nanosleep(&pause, NULL); } }
    char p[768], record[32], out[256]; int enabled = tunnel_policy(), ok = 0;
    if (lock >= 0 && enabled >= 0 && !data_job()) {
        if (enabled || data_idle()) ok = tunnel_command("tunnel-apply", out, sizeof out);
        else { /* A call started after the initial idle check: retain its tunnel. */
            if (data_file(p, sizeof p, 1, "tunnel-policy")) atomic_write(p, "1\n");
            enabled = 1;
        }
    }
    if (data_file(p, sizeof p, 0, "tunnel-result")) { snprintf(record, sizeof record, "%d %d\n", enabled, ok); atomic_write(p, record); }
    if (data_file(p, sizeof p, 0, "tunnel-job")) unlink(p);
    if (lock >= 0) close(lock);
    return ok ? 0 : 1;
}
static void tunnel_set(void) {
    if(esim_operation_busy()){error_response(409,"请等待 eSIM 操作完成后再切换花生壳服务");return;}
    const char *value = field("enabled");
    if (strcmp(value, "0") && strcmp(value, "1")) { error_response(400, "花生壳开关参数无效"); return; }
    int lock = data_lock();
    if (lock < 0) { error_response(409, "服务正在切换，请稍后刷新"); return; }
    if (data_job() || tunnel_job()) { close(lock); error_response(409, "联网或花生壳服务正在切换，请稍后刷新"); return; }
    if (!tunnel_configured()) { close(lock); error_response(503, "花生壳尚未安装或配置"); return; }
    if (value[0] == '0' && !data_idle()) { close(lock); error_response(409, "请先结束电话；无法确认空闲时不会关闭花生壳"); return; }
    int old = tunnel_policy(); char policy[768], job[768], program[768], record[64];
    if (old < 0 || !data_file(policy, sizeof policy, 1, "tunnel-policy") || !data_file(job, sizeof job, 0, "tunnel-job") || !path(program, sizeof program, root, "www/cgi-bin/api")) {
        close(lock); error_response(503, "花生壳设置暂不可用"); return;
    }
    snprintf(record, sizeof record, "%llu\n", (unsigned long long)now_seconds());
    if (!atomic_write(job, record)) { close(lock); error_response(503, "无法创建花生壳切换任务"); return; }
    char desired[3] = {value[0], '\n', 0};
    if (!atomic_write(policy, desired)) { unlink(job); close(lock); error_response(503, "花生壳设置保存失败"); return; }
    pid_t pid = fork();
    if (!pid) {
        close(lock); setsid(); int sink = open("/dev/null", O_RDWR);
        if (sink >= 0) { dup2(sink, STDIN_FILENO); dup2(sink, STDOUT_FILENO); dup2(sink, STDERR_FILENO); if (sink > 2) close(sink); }
        execl(program, program, "--tunnel-apply", NULL); _exit(127);
    }
    if (pid < 0) { desired[0] = (char)('0' + old); atomic_write(policy, desired); unlink(job); close(lock); error_response(503, "无法启动花生壳切换任务"); return; }
    close(lock); char url[256]; tunnel_public_url(url);
    headers(202, NULL); printf("{\"ok\":true,\"accepted\":true,\"configured\":true,\"enabled\":%s,\"pending\":true,\"publicURL\":", value[0] == '1' ? "true" : "false"); json_string(url); puts("}"); fflush(stdout);
}
