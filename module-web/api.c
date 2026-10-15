#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "traffic.h"
#include "notify.h"
#include "call-history.h"
#include "data-link.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#define VERSION "0.3.43"
#define ITERATIONS 600000u
#define BODY_LIMIT 8192
#define SESSION_SECONDS 3600u
#define REMEMBER_SESSION_SECONDS (7u * 24u * 3600u)
#define MAX_SESSIONS 8
#define HEX_SIZE 65

typedef struct { unsigned iterations; char salt[33], hash[65], generation[65]; } Credential;
typedef struct { uint64_t expires; char csrf[65], generation[65]; } Session;
typedef struct { char name[32], value[4097]; } Field;
static char root[512], runtime[512], proc[512], credential_file[640], session_dir[640];
static char action_buffer[64];
static Field fields[12];
static size_t field_count;
static int esim_operation_busy(void);

static int path(char *out, size_t size, const char *base, const char *suffix) {
    int n = snprintf(out, size, "%s/%s", base, suffix);
    return n > 0 && (size_t)n < size;
}
static uint64_t now_seconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec;
}
static int safe_equal(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    if (n != m) return 0;
    unsigned char different = 0;
    for (size_t i = 0; i < n; i++) different |= (unsigned char)(a[i] ^ b[i]);
    return different == 0;
}
static int valid_hex(const char *s, size_t n) {
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++) if (!strchr("0123456789abcdef", s[i])) return 0;
    return 1;
}
static int random_bytes(unsigned char *out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return 0; }
        got += (size_t)r;
    }
    close(fd); return 1;
}
static void encode_hex(const unsigned char *in, size_t n, char *out) {
    const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = h[in[i] >> 4]; out[i * 2 + 1] = h[in[i] & 15]; }
    out[n * 2] = 0;
}
static int random_hex(char *out, size_t bytes) {
    unsigned char b[32];
    if (bytes > sizeof b || !random_bytes(b, bytes)) return 0;
    encode_hex(b, bytes, out); memset(b, 0, sizeof b); return 1;
}
static int decode_hex(const char *in, unsigned char *out, size_t bytes) {
    if (!valid_hex(in, bytes * 2)) return 0;
    for (size_t i = 0; i < bytes; i++) {
        const char *a = strchr("0123456789abcdef", in[2 * i]);
        const char *b = strchr("0123456789abcdef", in[2 * i + 1]);
        out[i] = (unsigned char)(((a - "0123456789abcdef") << 4) | (b - "0123456789abcdef"));
    }
    return 1;
}
static int derive(const char *password, const Credential *c, char output[65]) {
    unsigned char salt[16], hash[32];
    if (!decode_hex(c->salt, salt, sizeof salt)) return 0;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (!rc) rc = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const unsigned char *)password, strlen(password), salt, sizeof salt, c->iterations, sizeof hash, hash);
    mbedtls_md_free(&ctx);
    if (rc) return 0;
    encode_hex(hash, sizeof hash, output);
    memset(hash, 0, sizeof hash); return 1;
}
static int safe_file(const char *name, int flags) {
    int fd = open(name, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(fd); return -1;
    }
    return fd;
}
static int safe_create(const char *name) {
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(fd); unlink(name); return -1;
    }
    return fd;
}
static int atomic_write(const char *name, const char *text) {
    char nonce[17], temp[768];
    if (!random_hex(nonce, 8)) return 0;
    int n = snprintf(temp, sizeof temp, "%s.%s.tmp", name, nonce);
    if (n < 0 || (size_t)n >= sizeof temp) return 0;
    int fd = safe_create(temp);
    if (fd < 0) return 0;
    size_t done = 0, length = strlen(text);
    while (done < length) {
        ssize_t count = write(fd, text + done, length - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); unlink(temp); return 0; }
        done += (size_t)count;
    }
    int ok = !fsync(fd);
    if (close(fd)) ok = 0;
    if (ok && rename(temp, name)) ok = 0;
    if (ok) {
        char parent[768];
        snprintf(parent, sizeof parent, "%s", name);
        char *slash = strrchr(parent, '/');
        if (!slash) ok = 0;
        else {
            *slash = 0;
            int dir = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (dir < 0) ok = 0;
            else {
                int rc; do { rc = fsync(dir); } while (rc && errno == EINTR);
                if (rc) ok = 0;
                close(dir);
            }
        }
    }
    if (!ok) unlink(temp);
    return ok;
}
static int read_credential(Credential *c) {
    int fd = safe_file(credential_file, O_RDONLY);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    int ok = fscanf(f, "pbkdf2-sha256-v1 %u %32s %64s %64s", &c->iterations, c->salt, c->hash, c->generation) == 4;
    fclose(f);
    return ok && c->iterations == ITERATIONS && valid_hex(c->salt, 32) && valid_hex(c->hash, 64) && valid_hex(c->generation, 64);
}
static int new_credential(const char *password, Credential *c) {
    c->iterations = ITERATIONS;
    return random_hex(c->salt, 16) && random_hex(c->generation, 32) && derive(password, c, c->hash);
}
static int write_credential(const Credential *c) {
    char text[256];
    snprintf(text, sizeof text, "pbkdf2-sha256-v1 %u %s %s %s\n", c->iterations, c->salt, c->hash, c->generation);
    return atomic_write(credential_file, text);
}
static int prepare_dir(const char *name) {
    if (mkdir(name, 0700) && errno != EEXIST) return 0;
    struct stat st;
    return !lstat(name, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 077) == 0;
}
static int initialize_paths(void) {
    const char *r = getenv("QDC_WEB_ROOT"), *s = getenv("QDC_WEB_RUN"), *p = getenv("QDC_WEB_PROC_ROOT");
    if (snprintf(root, sizeof root, "%s", r ? r : "/usrdata/qdc-web") >= (int)sizeof root ||
        snprintf(runtime, sizeof runtime, "%s", s ? s : "/run/qdc-web") >= (int)sizeof runtime ||
        snprintf(proc, sizeof proc, "%s", p ? p : "/proc") >= (int)sizeof proc) return 0;
    return path(credential_file, sizeof credential_file, root, "private/credential") && path(session_dir, sizeof session_dir, runtime, "sessions");
}
static const char *request_action(void) {
    const char *path_info = getenv("PATH_INFO");
    if (path_info && path_info[0] == '/') return path_info;
    /* BusyBox httpd normally supplies PATH_INFO for /cgi-bin/api/login.
       Some vendor builds omit it, so accept only the exact same suffix from
       REQUEST_URI; queries and any other URI shape remain rejected below. */
    const char *uri = getenv("REQUEST_URI");
    const char *prefix = "/cgi-bin/api";
    size_t prefix_len = strlen(prefix);
    if (!uri || strncmp(uri, prefix, prefix_len)) return NULL;
    uri += prefix_len;
    size_t len = strcspn(uri, "?#");
    if (!len || len >= sizeof action_buffer) return NULL;
    memcpy(action_buffer, uri, len);
    action_buffer[len] = 0;
    return action_buffer;
}
static void json_string(const char *value) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}
static void headers(int status, const char *cookie) {
    printf("Status: %d\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nPragma: no-cache\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n", status);
    if (cookie) printf("Set-Cookie: %s\r\n", cookie);
    printf("\r\n");
}
static void error_response(int status, const char *message) {
    headers(status, NULL); printf("{\"ok\":false,\"error\":"); json_string(message); puts("}");
}
static int parse_string(const char **input, char *out, size_t size) {
    const unsigned char *p = (const unsigned char *)*input;
    if (*p++ != '"') return 0;
    size_t used = 0;
    while (*p && *p != '"') {
        unsigned code = *p++;
        if (code < 32) return 0;
        if (code == '\\') {
            code = *p++;
            if (code == 'u') {
                unsigned u = 0;
                for (int i = 0; i < 4; i++) {
                    int ch = *p++;
                    if (ch >= '0' && ch <= '9') u = u * 16 + (unsigned)(ch - '0');
                    else if (ch >= 'a' && ch <= 'f') u = u * 16 + (unsigned)(ch - 'a' + 10);
                    else if (ch >= 'A' && ch <= 'F') u = u * 16 + (unsigned)(ch - 'A' + 10);
                    else return 0;
                }
                if (u == 0 || (u >= 0xd800 && u <= 0xdfff)) return 0;
                if (u < 128) code = u;
                else {
                    if (used + 3 >= size) return 0;
                    if (u >= 2048) out[used++] = (char)(0xe0 | (u >> 12));
                    out[used++] = (char)((u >= 2048 ? 0x80 : 0xc0) | ((u >> 6) & (u >= 2048 ? 63 : 31)));
                    out[used++] = (char)(0x80 | (u & 63));
                    continue;
                }
            } else if (code == 'n') code = '\n';
            else if (code == 'r') code = '\r';
            else if (code == 't') code = '\t';
            else if (code == 'b') code = '\b';
            else if (code == 'f') code = '\f';
            else if (code != '"' && code != '\\' && code != '/') return 0;
        }
        if (used + 1 >= size) return 0;
        out[used++] = (char)code;
    }
    if (*p++ != '"') return 0;
    out[used] = 0; *input = (const char *)p; return 1;
}
static void whitespace(const char **p) { while (**p == ' ' || **p == '\r' || **p == '\n' || **p == '\t') ++*p; }
static const char *field(const char *name) {
    for (size_t i = 0; i < field_count; i++) if (!strcmp(fields[i].name, name)) return fields[i].value;
    return "";
}
static int read_body(void) {
    const char *type = getenv("CONTENT_TYPE"), *len = getenv("CONTENT_LENGTH");
    if (!type || (strcmp(type, "application/json") && strncmp(type, "application/json;", 17)) || !len || !*len) return 0;
    char *end;
    unsigned long n = strtoul(len, &end, 10);
    if (*end || !n || n > BODY_LIMIT) return 0;
    char body[BODY_LIMIT + 1];
    if (fread(body, 1, n, stdin) != n) return 0;
    body[n] = 0;
    if (strlen(body) != n) return 0;
    const char *p = body; whitespace(&p);
    if (*p++ != '{') return 0;
    whitespace(&p); field_count = 0;
    if (*p != '}') for (;;) {
        if (field_count == sizeof fields / sizeof fields[0]) return 0;
        Field *f = &fields[field_count];
        if (!parse_string(&p, f->name, sizeof f->name)) return 0;
        for (size_t i = 0; i < field_count; i++) if (!strcmp(f->name, fields[i].name)) return 0;
        whitespace(&p); if (*p++ != ':') return 0; whitespace(&p);
        if (!parse_string(&p, f->value, sizeof f->value)) return 0;
        field_count++; whitespace(&p);
        if (*p != ',') break;
        p++; whitespace(&p);
    }
    if (*p++ != '}') return 0;
    whitespace(&p); return *p == 0;
}
static int cookie_id(char sid[65]) {
    const char *cookie = getenv("HTTP_COOKIE");
    if (!cookie || strlen(cookie) > 2048) return 0;
    int found = 0;
    while (*cookie) {
        while (*cookie == ' ' || *cookie == ';') cookie++;
        const char *end = strchr(cookie, ';');
        if (!end) end = cookie + strlen(cookie);
        if ((size_t)(end - cookie) >= 12 && !strncmp(cookie, "qdc_session=", 12)) {
            if (found || end - cookie != 76) return 0;
            memcpy(sid, cookie + 12, 64); sid[64] = 0;
            if (!valid_hex(sid, 64)) return 0;
            found = 1;
        }
        cookie = end; if (*cookie) cookie++;
    }
    return found;
}
static int read_session_id(const char *sid, Session *s) {
    char filename[768];
    if (!valid_hex(sid, 64) || !path(filename, sizeof filename, session_dir, sid)) return 0;
    int fd = safe_file(filename, O_RDONLY);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    int ok = fscanf(f, "session-v1 %" SCNu64 " %64s %64s", &s->expires, s->csrf, s->generation) == 3;
    fclose(f);
    if (!ok || !valid_hex(s->csrf, 64) || !valid_hex(s->generation, 64) || now_seconds() >= s->expires) {
        unlink(filename); return 0;
    }
    return 1;
}
static int authenticate(Credential *c, Session *s, char sid[65]) {
    return read_credential(c) && cookie_id(sid) && read_session_id(sid, s) && safe_equal(c->generation, s->generation);
}
static void clear_sessions(void) {
    DIR *dir = opendir(session_dir);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        char filename[768];
        if (valid_hex(e->d_name, 64) && path(filename, sizeof filename, session_dir, e->d_name)) unlink(filename);
    }
    closedir(dir);
}
static int session_capacity(void) {
    DIR *dir = opendir(session_dir);
    if (!dir) return 0;
    struct dirent *e; int count = 0;
    uint64_t oldest = UINT64_MAX; char oldid[65] = "";
    while ((e = readdir(dir))) {
        if (!valid_hex(e->d_name, 64)) continue;
        Session s;
        if (!read_session_id(e->d_name, &s)) continue;
        count++;
        if (s.expires < oldest) { oldest = s.expires; memcpy(oldid, e->d_name, 65); }
    }
    closedir(dir);
    if (count >= MAX_SESSIONS && *oldid) {
        char filename[768];
        if (!path(filename, sizeof filename, session_dir, oldid) || unlink(filename)) return 0;
    }
    return 1;
}
static int lock_auth(void) {
    char filename[768];
    if (!prepare_dir(runtime) || !prepare_dir(session_dir) || !path(filename, sizeof filename, runtime, "auth.lock")) return -1;
    int fd = safe_file(filename, O_RDWR | O_CREAT);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return -1; }
    return fd;
}
static int login_rate(void) {
    char filename[768], text[96]; uint64_t timestamp = 0; unsigned count = 0;
    if (!path(filename, sizeof filename, runtime, "login-rate")) return 0;
    int fd = safe_file(filename, O_RDONLY);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "r");
        if (f) { (void)fscanf(f, "%" SCNu64 " %u", &timestamp, &count); fclose(f); } else close(fd);
    }
    uint64_t now = now_seconds();
    if (now < timestamp || now - timestamp >= 60) { timestamp = now; count = 0; }
    if (count >= 5) return 0;
    snprintf(text, sizeof text, "%" PRIu64 " %u\n", timestamp, count + 1);
    return atomic_write(filename, text);
}
static void reset_login_rate(void) {
    char filename[768]; if (path(filename, sizeof filename, runtime, "login-rate")) unlink(filename);
}
static int password_valid(const char *password) {
    size_t n = strlen(password), characters = 0;
    if (n > 128) return 0;
    for (size_t i = 0; i < n;) {
        const unsigned char *p = (const unsigned char *)password + i;
        unsigned char c = p[0]; size_t width;
        if (c < 32 || c == 127) return 0;
        if (c < 128) width = 1;
        else if (c >= 0xc2 && c <= 0xdf) width = 2;
        else if (c >= 0xe0 && c <= 0xef) width = 3;
        else if (c >= 0xf0 && c <= 0xf4) width = 4;
        else return 0;
        if (i + width > n) return 0;
        for (size_t j = 1; j < width; j++) if ((p[j] & 0xc0) != 0x80) return 0;
        if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] >= 0xa0) ||
            (c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] >= 0x90)) return 0;
        i += width; characters++;
    }
    return characters >= 8;
}
static void login(void) {
    const char *remember = field("remember");
    if (*remember && strcmp(remember, "0") && strcmp(remember, "1")) {
        error_response(400, "记住登录选项无效"); return;
    }
    unsigned session_seconds = !strcmp(remember, "1") ? REMEMBER_SESSION_SECONDS : SESSION_SECONDS;
    int lock = lock_auth();
    if (lock < 0) { error_response(429, "正在处理登录，请稍后重试"); return; }
    Credential c; char hash[65];
    if (!read_credential(&c)) { error_response(503, "后台尚未初始化，请联系安装人员"); close(lock); return; }
    if (!login_rate()) { error_response(429, "登录尝试过于频繁，请一分钟后重试"); close(lock); return; }
    const char *password = field("password");
    if (strlen(password) > 128 || !derive(password, &c, hash) || strcmp(field("username"), "admin") || !safe_equal(hash, c.hash)) {
        error_response(401, "用户名或密码错误"); close(lock); return;
    }
    char sid[65], csrf[65], filename[768], record[256], cookie[192];
    if (!random_hex(sid, 32) || !random_hex(csrf, 32) || !session_capacity() || !path(filename, sizeof filename, session_dir, sid)) {
        error_response(500, "无法创建登录会话"); close(lock); return;
    }
    snprintf(record, sizeof record, "session-v1 %" PRIu64 " %s %s\n", now_seconds() + session_seconds, csrf, c.generation);
    if (!atomic_write(filename, record)) { error_response(500, "无法保存登录会话"); close(lock); return; }
    reset_login_rate();
    snprintf(cookie, sizeof cookie, "qdc_session=%s; Path=/; Max-Age=%u; HttpOnly; SameSite=Strict", sid, session_seconds);
    headers(200, cookie); printf("{\"ok\":true,\"username\":\"admin\",\"csrfToken\":\"%s\"}\n", csrf);
    close(lock);
}
static void session_response(const Session *s) {
    headers(200, NULL); printf("{\"authenticated\":true,\"username\":\"admin\",\"csrfToken\":\"%s\"}\n", s->csrf);
}
static void password_change(const Credential *previous, const Session *s) {
    int lock = lock_auth();
    if (lock < 0) { error_response(429, "正在修改设置，请稍后重试"); return; }
    Credential c; char hash[65];
    if (!read_credential(&c) || !safe_equal(c.generation, previous->generation)) { error_response(401, "登录已失效，请重新登录"); close(lock); return; }
    if (!safe_equal(field("csrfToken"), s->csrf)) { error_response(403, "页面已过期，请刷新后重试"); close(lock); return; }
    if (!login_rate()) { error_response(429, "密码尝试过于频繁，请一分钟后重试"); close(lock); return; }
    if (strlen(field("currentPassword")) > 128 || !derive(field("currentPassword"), &c, hash) || !safe_equal(hash, c.hash)) {
        error_response(403, "当前密码不正确"); close(lock); return;
    }
    if (!password_valid(field("newPassword")) || !safe_equal(field("newPassword"), field("confirmPassword"))) {
        error_response(400, "新密码至少 8 个字符、最长 128 字节，且两次输入一致"); close(lock); return;
    }
    if (!new_credential(field("newPassword"), &c) || !write_credential(&c)) { error_response(500, "密码保存失败，原密码仍然有效"); close(lock); return; }
    clear_sessions(); reset_login_rate();
    headers(200, "qdc_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    puts("{\"ok\":true,\"reauthenticate\":true}"); close(lock);
}
static FILE *proc_file(const char *name) {
    char filename[768]; return path(filename, sizeof filename, proc, name) ? fopen(filename, "r") : NULL;
}
static uint64_t proc_uptime_seconds(void) {
    FILE *f = proc_file("uptime");
    char text[64];
    uint64_t value = 0;
    if (f && fgets(text, sizeof text, f)) {
        char *end = NULL;
        unsigned long long parsed = strtoull(text, &end, 10);
        if (end != text) value = (uint64_t)parsed;
    }
    if (f) fclose(f);
    return value;
}
static void ipv4_address(const char *name, char output[INET_ADDRSTRLEN]) {
    struct ifaddrs *list = NULL; output[0] = 0;
    if (getifaddrs(&list)) return;
    for (struct ifaddrs *p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || strcmp(p->ifa_name, name)) continue;
        struct sockaddr_in *address = (struct sockaddr_in *)p->ifa_addr;
        if (inet_ntop(AF_INET, &address->sin_addr, output, INET_ADDRSTRLEN)) break;
    }
    freeifaddrs(list);
}
static void status_response(void) {
    uint64_t total = 0, available = 0, free_mem = 0;
    char line[512], hostname[128] = "QDC507", mobile[32] = "", usb_addr[INET_ADDRSTRLEN], mobile_addr[INET_ADDRSTRLEN];
    FILE *f = proc_file("meminfo");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            uint64_t kb;
            if (sscanf(line, "MemTotal: %" SCNu64 " kB", &kb) == 1) total = kb * 1024;
            if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &kb) == 1) available = kb * 1024;
            if (sscanf(line, "MemFree: %" SCNu64 " kB", &kb) == 1) free_mem = kb * 1024;
        }
        fclose(f);
    }
    f = proc_file("sys/kernel/hostname");
    if (f) { if (fgets(hostname, sizeof hostname, f)) hostname[strcspn(hostname, "\r\n")] = 0; fclose(f); }
    f = proc_file("net/route");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char name[32]; unsigned long destination, gateway; unsigned flags;
            if (sscanf(line, "%31s %lx %lx %x", name, &destination, &gateway, &flags) == 4 && destination == 0 && (flags & 1) && !strncmp(name, "rmnet", 5)) {
                snprintf(mobile, sizeof mobile, "%s", name); break;
            }
        }
        fclose(f);
    }
    ipv4_address("bridge0", usb_addr);
    ipv4_address(mobile, mobile_addr);
    headers(200, NULL);
    printf("{\"version\":\"%s\",\"model\":\"QDC507\",\"hostname\":", VERSION); json_string(hostname);
    printf(",\"uptimeSeconds\":%" PRIu64 ",\"memory\":{\"totalBytes\":%" PRIu64 ",\"availableBytes\":%" PRIu64 ",\"freeBytes\":%" PRIu64 "},\"storage\":[", proc_uptime_seconds(), total, available, free_mem);
    const char *mounts[] = {"/", "/usrdata"}, *names[] = {"系统分区", "数据分区"};
    int count = 0;
    for (size_t i = 0; i < 2; i++) {
        struct statvfs v;
        if (statvfs(mounts[i], &v)) continue;
        if (count++) putchar(',');
        printf("{\"name\":"); json_string(names[i]); printf(",\"mount\":"); json_string(mounts[i]);
        printf(",\"totalBytes\":%" PRIu64 ",\"availableBytes\":%" PRIu64 "}", (uint64_t)v.f_blocks * v.f_frsize, (uint64_t)v.f_bavail * v.f_frsize);
    }
    printf("],\"network\":{\"usbAddress\":"); json_string(usb_addr);
    printf(",\"usbInterface\":\"bridge0\",\"mobileInterface\":"); json_string(mobile);
    printf(",\"mobileAddress\":"); json_string(mobile_addr);
    printf(",\"connected\":%s,\"apn\":null,\"operator\":null,\"signalDbm\":null,\"networkType\":null,\"simState\":null},\"stateSource\":\"system\",\"updatedAt\":%lld}\n", *mobile && *mobile_addr ? "true" : "false", (long long)time(NULL));
}
static int init_credentials(void) {
    char private_dir[640], password[129];
    if (!path(private_dir, sizeof private_dir, root, "private") || !prepare_dir(root) || !prepare_dir(private_dir) || access(credential_file, F_OK) == 0) {
        fprintf(stderr, "Credential already initialized or private directory unavailable.\n"); return 1;
    }
    if (!fgets(password, sizeof password, stdin)) {
        if (!random_hex(password, 16)) return 1;
    } else {
        size_t end = strcspn(password, "\r\n");
        if (end == sizeof password - 1) {
            int next = fgetc(stdin);
            if (next != EOF && next != '\n' && next != '\r') {
                fprintf(stderr, "Password must have at most 128 UTF-8 bytes.\n");
                memset(password, 0, sizeof password); return 1;
            }
        }
        password[end] = 0;
    }
    if (!password_valid(password)) { fprintf(stderr, "Password must have at least 8 characters and at most 128 UTF-8 bytes.\n"); return 1; }
    Credential c;
    if (!new_credential(password, &c) || !write_credential(&c)) return 1;
    puts(password); memset(password, 0, sizeof password); return 0;
}
/* Fixed allowlist and execv, never a shell. Helper stdout is bounded and read
   before waitpid so a large SMS result cannot fill the pipe and deadlock. */
static int outbox_stamp(void);
static int data_job(void);
static int tunnel_job(void);
static int esim_operation_lock(void);
static void communication(const char *action) {
    int pin_verify=!strcmp(action,"/sim-pin-verify"),sim_recover=!strcmp(action,"/sim-recover"),input[2]={-1,-1},guard=-1,data_guard=-1;
    if(pin_verify||sim_recover){
        for(size_t i=0;i<field_count;i++)if((!pin_verify||strcmp(fields[i].name,"pin"))&&strcmp(fields[i].name,"stateToken")&&strcmp(fields[i].name,"requestId")&&strcmp(fields[i].name,"csrfToken")){error_response(400,"SIM 请求包含无效字段");return;}
        size_t n=strlen(field("pin")),t=strlen(field("stateToken"));
        if((pin_verify&&(n<4||n>8||strspn(field("pin"),"0123456789")!=n))||!t||t>2048||t%2||!valid_hex(field("stateToken"),t)||!valid_hex(field("requestId"),64)){error_response(400,"请先读取卡片状态并核对输入");return;}
        guard=esim_operation_lock();char name[768];path(name,sizeof name,runtime,"data.lock");data_guard=safe_file(name,O_RDWR|O_CREAT);
        if(guard<0||flock(guard,LOCK_EX|LOCK_NB)||data_guard<0||flock(data_guard,LOCK_EX|LOCK_NB)||data_job()||tunnel_job()){
            if(guard>=0)close(guard);if(data_guard>=0)close(data_guard);error_response(409,"模块正在处理卡片或联网操作，请稍后再操作");return;}
        if(pipe(input)){close(guard);close(data_guard);error_response(503,"无法提交卡片请求");return;}
    }
    if(!strcmp(action,"/sms-send")&&!outbox_stamp()){error_response(503,"无法保存短信发送时间，未执行发送");return;}
    char helper[768];
    if(!path(helper,sizeof helper,root,"www/cgi-bin/qdc-comm")) {error_response(500,"通讯程序路径无效");return;}
    char *args[8]={helper,(char *)(action+1),NULL};
    if(!strcmp(action,"/sms-list")) args[2]=(char *)field("cursor");
    else if(!strcmp(action,"/sms-delete")) {args[2]=(char *)field("storage");args[3]=(char *)field("index");args[4]=(char *)field("pdu");args[5]=(char *)field("sim");}
    else if(!strcmp(action,"/sms-send")) {args[2]=(char *)field("requestId");args[3]=(char *)field("pdus");args[4]=(char *)field("sim");}
    else if(strcmp(action,"/call-list")&&strcmp(action,"/radio")&&strcmp(action,"/esim-list")&&strcmp(action,"/sim-pin-state")&&strcmp(action,"/sim-pin-verify")&&strcmp(action,"/sim-recover")) {args[2]=(char *)field(!strcmp(action,"/call-dial")?"number":"callId");args[3]=(char *)field("requestId");}
    int diagnostic=-1;
    if(!strcmp(action,"/call-end")||!strcmp(action,"/call-list")){
        char name[768];if(path(name,sizeof name,runtime,"call-control.log")){
            diagnostic=safe_file(name,O_WRONLY|O_CREAT|O_APPEND);
            if(diagnostic>=0){struct stat info;if(!fstat(diagnostic,&info)&&info.st_size>16384)ftruncate(diagnostic,0);}
        }
    }
    int pipes[2];if(pipe(pipes)){if(diagnostic>=0)close(diagnostic);error_response(503,"无法启动通讯程序");return;}
    pid_t pid=fork();
    if(pid<0){close(pipes[0]);close(pipes[1]);if(diagnostic>=0)close(diagnostic);error_response(503,"无法启动通讯程序");return;}
    if(!pid) {
        close(pipes[0]);dup2(pipes[1],STDOUT_FILENO);close(pipes[1]);
        int nullfd=open("/dev/null",O_RDWR);if(nullfd>=0){dup2(nullfd,STDIN_FILENO);dup2(nullfd,STDERR_FILENO);close(nullfd);}
        if(pin_verify||sim_recover){close(input[1]);dup2(input[0],STDIN_FILENO);close(input[0]);}
        if(diagnostic>=0){dup2(diagnostic,STDERR_FILENO);setenv("QDC_COMM_DEBUG","1",1);}
        execv(helper,args);_exit(127);
    }
    if(pin_verify||sim_recover){
        close(input[0]);char wire[2200];int n=pin_verify?snprintf(wire,sizeof wire,"%s\n%s\n%s\n",field("pin"),field("stateToken"),field("requestId")):snprintf(wire,sizeof wire,"%s\n%s\n",field("stateToken"),field("requestId"));
        signal(SIGPIPE,SIG_IGN);
        if(write(input[1],wire,(size_t)n)!=n)kill(pid,SIGKILL);close(input[1]);memset(wire,0,sizeof wire);
        for(size_t i=0;i<field_count;i++)if(!strcmp(fields[i].name,"pin"))memset(fields[i].value,0,sizeof fields[i].value);
    }
    close(pipes[1]);char result[32769];size_t used=0;int ok=1;
    uint64_t start=now_seconds();
    while(used<sizeof result-1) {
        if(now_seconds()-start>=37){ok=0;break;}
        struct pollfd p={pipes[0],POLLIN,0};int ready=poll(&p,1,1000);
        if(ready<0&&errno==EINTR)continue;
        if(ready<0){ok=0;break;}if(!ready)continue;
        ssize_t n=read(pipes[0],result+used,sizeof result-1-used);
        if(n<0&&errno==EINTR)continue;if(n<0){ok=0;break;}if(!n)break;used+=(size_t)n;
    }
    close(pipes[0]);if(!ok||used==sizeof result-1)kill(pid,SIGKILL);
    int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    result[used]=0;
    if(guard>=0)close(guard);if(data_guard>=0)close(data_guard);
    if(diagnostic>=0){dprintf(diagnostic,"api %s: elapsed=%llu bytes=%zu bounded=%d exit=%d signal=%d\n",action,(unsigned long long)(now_seconds()-start),used,ok,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);close(diagnostic);}
    if(!ok||!used||used==sizeof result-1||!WIFEXITED(status)||WEXITSTATUS(status)||result[0]!='{'||!strchr(result,'}')) {
        error_response(503,sim_recover?"重新识别结果未确认，请刷新卡片状态，不要直接重复操作":pin_verify?"PIN 验证结果未确认，请先刷新卡片状态，不要直接重试":!strcmp(action,"/esim-list")?"读卡未返回完整结果，请稍后重新读卡":"通讯操作未返回完整结果，请刷新状态；请勿直接重发短信或重复拨号");return;
    }
    headers(200,NULL);fputs(result,stdout);
}
/* Single-use authorization; no permanent password or password hash in tickets. */
static void audio_ticket(const Session *s, const char *sid) {
    char dir[640], filename[768], ticket[65], text[256];
    if (!path(dir,sizeof dir,runtime,"audio-tickets") || !prepare_dir(dir)) {
        error_response(503,"音频授权目录不可用"); return;
    }
    int lock=lock_auth(); if(lock<0){error_response(503,"正在处理登录请求，请稍后重试");return;}
    DIR *d=opendir(dir); unsigned count=0;
    if(d){struct dirent *e;while((e=readdir(d))){
        if(!valid_hex(e->d_name,64)||!path(filename,sizeof filename,dir,e->d_name))continue;
        int fd=safe_file(filename,O_RDONLY);uint64_t expires=0;
        if(fd>=0){FILE *f=fdopen(fd,"r");if(f){unsigned long long value=0;if(fscanf(f,"audio-v1 %llu",&value)==1)expires=value;fclose(f);}else close(fd);}
        if(expires<=now_seconds())unlink(filename);else count++;
    }closedir(d);}
    if(count>=16||!random_hex(ticket,32)||!path(filename,sizeof filename,dir,ticket)){
        close(lock);error_response(429,"音频授权请求过多，请稍后重试");return;
    }
    snprintf(text,sizeof text,"audio-v1 %llu %s %s\n",(unsigned long long)(now_seconds()+60),sid,s->generation);
    int ok=atomic_write(filename,text);close(lock);
    if(!ok){error_response(503,"无法创建音频授权");return;}
    headers(200,NULL);printf("{\"ok\":true,\"ticket\":\"%s\",\"expiresIn\":60}\n",ticket);
}
static void traffic_response(int reset) {
    QdcTraffic t;
    if (!qdc_traffic_update(&t, reset, 0)) {
        error_response(503, "流量统计暂时不可用，请稍后刷新确认结果"); return;
    }
    headers(200, NULL);
    printf("{\"ok\":true,\"receivedBytes\":%" PRIu64 ",\"sentBytes\":%" PRIu64
        ",\"since\":%" PRIu64 ",\"interfaces\":%u,\"checkpointSeconds\":30}\n", t.received, t.sent, t.since, t.interfaces);
}
#include "events-api.inc"
static int tunnel_job(void);
static void compat_fields(void);
static void compat_set(void);
typedef struct {int state,permission,profile;} RoamingState;
static int roaming_policy(void);
static RoamingState roaming_state_read(void);
static int roaming_configure(const RoamingState *state,int permission);
static int roaming_permission(int policy,const RoamingState *state);
static int roaming_effective(int enabled,int permission,int state);
#include "data-control.inc"
#include "roaming.inc"
#include "usb-compat.inc"
#include "tunnel-control.inc"
#include "oray-binding.inc"
#include "oray-heartbeat.inc"
#include "sms-outbox.inc"
#include "call-history-api.inc"
#include "esim-api.inc"
int main(int argc, char **argv) {
    umask(0077);
    alarm(42);
    if (!initialize_paths()) return 1;
    if (argc == 2 && !strcmp(argv[1], "--heartbeat-apply")) { alarm(0); return heartbeat_apply(); }
    if (argc == 2 && !strcmp(argv[1], "--tunnel-apply")) return tunnel_apply();
    if (argc == 2 && !strcmp(argv[1], "--data-apply")) return data_reconcile(1);
    if (argc == 2 && !strcmp(argv[1], "--data-reconcile")) return data_reconcile(0);
    if (argc == 2 && !strcmp(argv[1], "--usb-compat-reconcile")) return compat_reconcile();
    if (argc == 2 && !strcmp(argv[1], "--init")) return init_credentials();
    const char *method = getenv("REQUEST_METHOD"), *action = request_action();
    if (!method || !action) { fprintf(stderr, "Use as CGI or --init.\n"); return 2; }
    int post = !strcmp(method, "POST"), get = !strcmp(method, "GET");
    if (!get && !post) { error_response(405, "请求方法不支持"); return 0; }
    int comm = !strcmp(action,"/sim-recover")||!strcmp(action,"/sim-pin-verify")||!strcmp(action,"/esim-list")||!strcmp(action,"/sms-list")||!strcmp(action,"/sms-send")||!strcmp(action,"/sms-delete")||!strcmp(action,"/call-list")||!strcmp(action,"/call-dial")||!strcmp(action,"/call-answer")||!strcmp(action,"/call-end");
    if (!comm && strcmp(action, "/esim-operation") && strcmp(action, "/sim-pin-state") && strcmp(action, "/esim-job") && strcmp(action, "/call-history-list") && strcmp(action, "/call-history-clear") && strcmp(action, "/call-history-import") && strcmp(action, "/sms-outbox-list") && strcmp(action, "/sms-outbox-delete") && strcmp(action, "/oray-heartbeat") && strcmp(action, "/oray-heartbeat-set") && strcmp(action, "/oray-status") && strcmp(action, "/oray-qr") && strcmp(action, "/tunnel") && strcmp(action, "/tunnel-set") && strcmp(action, "/data") && strcmp(action, "/data-set") && strcmp(action, "/events") && strcmp(action, "/notify") && strcmp(action, "/notify-settings") && strcmp(action, "/notify-test") && strcmp(action, "/traffic") && strcmp(action, "/traffic-reset") && strcmp(action, "/audio-ticket") && strcmp(action, "/radio") && strcmp(action, "/login") && strcmp(action, "/logout") && strcmp(action, "/password") && strcmp(action, "/status") && strcmp(action, "/session")) {
        error_response(404, "页面不存在"); return 0;
    }
    if (((!strcmp(action, "/sim-pin-state") || !strcmp(action, "/esim-job") || !strcmp(action, "/oray-heartbeat") || !strcmp(action, "/oray-status") || !strcmp(action, "/tunnel") || !strcmp(action, "/data") || !strcmp(action, "/events") || !strcmp(action, "/notify") || !strcmp(action, "/traffic") || !strcmp(action, "/radio") || !strcmp(action, "/status") || !strcmp(action, "/session")) && !get) ||
        ((comm || !strcmp(action, "/esim-operation") || !strcmp(action, "/call-history-list") || !strcmp(action, "/call-history-clear") || !strcmp(action, "/call-history-import") || !strcmp(action, "/sms-outbox-list") || !strcmp(action, "/sms-outbox-delete") || !strcmp(action, "/oray-heartbeat-set") || !strcmp(action, "/oray-qr") || !strcmp(action, "/tunnel-set") || !strcmp(action, "/data-set") || !strcmp(action, "/notify-settings") || !strcmp(action, "/notify-test") || !strcmp(action, "/traffic-reset") || !strcmp(action, "/audio-ticket") || !strcmp(action, "/login") || !strcmp(action, "/logout") || !strcmp(action, "/password")) && !post)) {
        error_response(405, "请求方法不支持"); return 0;
    }
    if (post && !read_body()) { error_response(400, "请求内容无效"); return 0; }
    if (!strcmp(action, "/login")) { login(); return 0; }
    Credential c; Session s; char sid[65];
    if (!authenticate(&c, &s, sid)) { error_response(401, "登录已失效，请重新登录"); return 0; }
    if (!strcmp(action, "/session")) session_response(&s);
    else if (!strcmp(action, "/esim-job")) esim_job_response();
    else if (!strcmp(action, "/status")) status_response();
    else if (!strcmp(action, "/radio")||!strcmp(action,"/sim-pin-state")) communication(action);
    else if (!strcmp(action, "/tunnel")) tunnel_response();
    else if (!strcmp(action, "/oray-heartbeat")) heartbeat_response();
    else if (!strcmp(action, "/oray-status")) oray_binding_status();
    else if (!strcmp(action, "/data")) data_response();
    else if (!strcmp(action, "/traffic")) traffic_response(0);
    else if (!strcmp(action, "/events")) events_response(sid);
    else if (!strcmp(action, "/notify")) notify_response();
    else if (!safe_equal(field("csrfToken"), s.csrf)) error_response(403, "页面已过期，请刷新后重试");
    else if (!strcmp(action, "/esim-operation")) esim_operation_start();
    else if (!strcmp(action, "/call-history-list")) history_list();
    else if (!strcmp(action, "/call-history-clear")) history_clear();
    else if (!strcmp(action, "/call-history-import")) history_import();
    else if (!strcmp(action, "/sms-outbox-list")) outbox_list();
    else if (!strcmp(action, "/sms-outbox-delete")) outbox_delete();
    else if (!strcmp(action, "/oray-heartbeat-set")) heartbeat_set();
    else if (!strcmp(action, "/tunnel-set")) tunnel_set();
    else if (!strcmp(action, "/oray-qr")) oray_qrcode();
    else if (!strcmp(action, "/data-set")) data_set();
    else if (!strcmp(action, "/traffic-reset")) traffic_response(1);
    else if (!strcmp(action, "/notify-settings")) notify_settings();
    else if (!strcmp(action, "/notify-test")) notify_test();
    else if (!strcmp(action, "/password")) password_change(&c, &s);
    else if (!strcmp(action, "/audio-ticket")) audio_ticket(&s,sid);
    else if (comm) communication(action);
    else {
        char filename[768];
        if (path(filename, sizeof filename, session_dir, sid)) unlink(filename);
        headers(200, "qdc_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"); puts("{\"ok\":true}");
    }
    memset(fields, 0, sizeof fields);
    return 0;
}
