/* Module-owned HTTP/WebSocket front door. Fixed upstreams; no shell, TLS or
 * modem operations. TLS terminates at the authorized HTTPS tunnel service.
 * Listen only on loopback: the tunnel client is the sole public entry point. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "traffic.h"

#define HEADER 12288
#define BODY 8192
#define LIMIT 12
#ifndef RELAY_IDLE_MS
#define RELAY_IDLE_MS 60000
#endif
static volatile sig_atomic_t stopping;
static int listener = -1;
static char expected_origin[256];
static unsigned short port = 8788, web_port = 80, audio_port = 8789;
static const char *upstream = "192.168.225.1";
static long long now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void stop(int sig) { (void)sig; stopping = 1; }
static int wait_io(int fd, short events, long long deadline) {
    while (!stopping) {
        long long remaining = deadline - now_ms();
        if (remaining <= 0) return 0;
        struct pollfd p = {fd, events, 0};
        int n = poll(&p, 1, remaining > 1000 ? 1000 : (int)remaining);
        if (n > 0) return !(p.revents & POLLNVAL);
        if (n < 0 && errno != EINTR) return 0;
    }
    return 0;
}
static int write_all(int fd, const void *data, size_t size, long long deadline) {
    const char *p = data;
    while (size) {
        if (!wait_io(fd, POLLOUT, deadline)) return 0;
        ssize_t n = write(fd, p, size);
        if (n > 0) { p += n; size -= (size_t)n; }
        else if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        else return 0;
    }
    return 1;
}
static int read_header(int fd, char *buf, long long deadline) {
    /* Do not consume any upgraded WebSocket frame or HTTP body here. */
    size_t n = 0;
    while (n < HEADER - 1) {
        if (!wait_io(fd, POLLIN, deadline)) return 0;
        ssize_t r = read(fd, buf + n, 1);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (r != 1 || !buf[n]) return 0;
        n++;
        if (n >= 4 && !memcmp(buf + n - 4, "\r\n\r\n", 4)) {
            buf[n] = 0; return 1;
        }
    }
    return 0;
}
static void reject(int fd, int status) {
    char s[160]; int n = snprintf(s, sizeof s,
        "HTTP/1.1 %d Rejected\r\nContent-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status);
    (void)write_all(fd, s, (size_t)n, now_ms() + 1000);
}
static int append(char *out, const char *name, const char *value) {
    size_t used = strlen(out);
    int n = snprintf(out + used, HEADER - used, "%s: %s\r\n", name, value);
    return n > 0 && (size_t)n < HEADER - used;
}
static int known(const char *p, const char *const *set) {
    for (; *set; set++) if (!strcmp(p, *set)) return 1;
    return 0;
}
static int connect_backend(unsigned short target) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(target);
    if (inet_pton(AF_INET, upstream, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
        int e = 0; socklen_t n = sizeof e;
        if (errno != EINPROGRESS || !wait_io(fd, POLLOUT, now_ms() + 5000) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n) || e) { close(fd); return -1; }
    }
    return fd;
}
static void relay(int client, int backend, int websocket, int events) {
    struct { unsigned char data[4096]; size_t start, end; int eof; } b[2] = {{{0},0,0,0},{{0},0,0,0}};
    int fd[2] = {client, backend}; long long deadline = now_ms() + RELAY_IDLE_MS;
    /* Event streams can be silent. Keep them until disconnect/session expiry
     * without generating periodic packets or forcing idle reconnects. */
    while (!stopping && (events || now_ms() < deadline)) {
        struct pollfd p[2];
        for (int i = 0; i < 2; i++) {
            p[i].fd = fd[i]; p[i].events = 0; p[i].revents = 0;
            if (!b[i].eof && b[i].end < sizeof b[i].data) p[i].events |= POLLIN;
            if (b[1-i].end > b[1-i].start) p[i].events |= POLLOUT;
        }
        int n = poll(p, 2, 1000); if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; i++) {
            if (p[i].revents & (POLLERR | POLLNVAL)) return;
            if ((p[i].revents & (POLLIN | POLLHUP)) && !b[i].eof && b[i].end < sizeof b[i].data) {
                ssize_t r = read(fd[i], b[i].data + b[i].end, sizeof b[i].data - b[i].end);
                if (r > 0) { b[i].end += (size_t)r; deadline = now_ms() + RELAY_IDLE_MS; }
                else if (!r || (errno != EAGAIN && errno != EINTR)) b[i].eof = 1;
            }
            if ((p[i].revents & POLLOUT) && b[1-i].end > b[1-i].start) {
                ssize_t r = write(fd[i], b[1-i].data + b[1-i].start, b[1-i].end - b[1-i].start);
                if (r > 0) { b[1-i].start += (size_t)r; deadline = now_ms() + RELAY_IDLE_MS; }
                else if (r < 0 && errno != EAGAIN && errno != EINTR) return;
            }
        }
        for (int i = 0; i < 2; i++) if (b[i].start == b[i].end) {
            b[i].start = b[i].end = 0;
            if (b[i].eof) { shutdown(fd[1-i], SHUT_WR); if (websocket || events || i == 1) return; }
        }
    }
}
#include "static-assets.inc"
static void serve(int client) {
    char input[HEADER], output[HEADER], method[8], target[1024], protocol[16], extra;
    char *end, *save, origin[256] = "", upgrade[32] = "", encoding[512]="", conditional[512]="";
    int encodings=0,conditions=0;
    size_t length = 0; int hosts = 0, origins = 0, lengths = 0, upgrades = 0;
    int has_transfer = 0;
    if (!read_header(client, input, now_ms() + 10000)) { reject(client, 400); return; }
    end = strstr(input, "\r\n"); if (!end) { reject(client, 400); return; }
    *end = 0;
    if (sscanf(input, "%7s %1023s %15s %c", method, target, protocol, &extra) != 3 || strcmp(protocol, "HTTP/1.1") ||
        strpbrk(target, "\\%#") || target[0] != '/') { reject(client, 400); return; }
    char route[1024]; snprintf(route, sizeof route, "%s", target); route[strcspn(route, "?")] = 0;
    if (!strcmp(route, "/") || !strcmp(route, "/module")) {
        if (strcmp(method,"GET") && strcmp(method,"HEAD")) { reject(client,405); return; }
        const char s[] = "HTTP/1.1 307 Temporary Redirect\r\nLocation: /module/\r\nCache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)write_all(client, s, sizeof s - 1, now_ms() + 1000); return;
    }
    if (strncmp(route, "/module/", 8)) { reject(client,404); return; }
    const char *path = route + 7, *request_target = target + 7;
    const char *const assets[] = {"/","/index.html","/app.css","/app.js","/events.js","/sms-codec.js","/communications.js","/call-audio.js","/audio.js","/pcm-worklet.js",NULL};
    const char *const get[] = {"/cgi-bin/api/sim-pin-state","/cgi-bin/api/esim-job","/cgi-bin/api/session","/cgi-bin/api/status","/cgi-bin/api/radio","/cgi-bin/api/traffic","/cgi-bin/api/events","/cgi-bin/api/notify","/cgi-bin/api/data","/cgi-bin/api/tunnel","/cgi-bin/api/oray-status","/cgi-bin/api/oray-heartbeat",NULL};
    const char *const post[] = {"/cgi-bin/api/sim-recover","/cgi-bin/api/sim-pin-verify","/cgi-bin/api/esim-operation","/cgi-bin/api/esim-list","/cgi-bin/api/login","/cgi-bin/api/logout","/cgi-bin/api/password","/cgi-bin/api/sms-list","/cgi-bin/api/sms-send","/cgi-bin/api/sms-delete","/cgi-bin/api/call-list","/cgi-bin/api/call-dial","/cgi-bin/api/call-answer","/cgi-bin/api/call-end","/cgi-bin/api/audio-ticket","/cgi-bin/api/traffic-reset","/cgi-bin/api/notify-settings","/cgi-bin/api/notify-test","/cgi-bin/api/data-set","/cgi-bin/api/tunnel-set","/cgi-bin/api/oray-qr","/cgi-bin/api/oray-heartbeat-set","/cgi-bin/api/call-history-list","/cgi-bin/api/call-history-clear","/cgi-bin/api/call-history-import","/cgi-bin/api/sms-outbox-list","/cgi-bin/api/sms-outbox-delete",NULL};
    int ws = !strcmp(path, "/audio"), asset = known(path,assets), is_post = !strcmp(method,"POST");
    if (!((asset && (!strcmp(method,"GET") || !strcmp(method,"HEAD"))) ||
          ((!strcmp(method,"GET")) && (ws || known(path,get))) || (is_post && known(path,post)))) { reject(client,404); return; }
    snprintf(output, sizeof output, "%s %s HTTP/1.1\r\n", method, request_target);
    for (char *line = strtok_r(end + 2, "\r\n", &save); line; line = strtok_r(NULL,"\r\n",&save)) {
        char *colon = strchr(line, ':');
        if (!colon || line[0] == ' ' || line[0] == '\t') { reject(client,400); return; }
        *colon++ = 0; while (*colon == ' ' || *colon == '\t') colon++;
        for (char *v=line; *v; v++) if (!( (*v>='a'&&*v<='z') || (*v>='A'&&*v<='Z') || (*v>='0'&&*v<='9') || *v=='-')) { reject(client,400); return; }
        for (char *v=colon; *v; v++) if ((unsigned char)*v < 32 && *v != '\t') { reject(client,400); return; }
        if (!strcasecmp(line,"Accept-Encoding")){encodings++;if(strlen(colon)>=sizeof encoding){reject(client,431);return;}strcpy(encoding,colon);}
        if (!strcasecmp(line,"If-None-Match")){conditions++;if(strlen(colon)>=sizeof conditional){reject(client,431);return;}strcpy(conditional,colon);}
        if (!strcasecmp(line,"Host")) hosts++;
        if (!strcasecmp(line,"Origin")) { origins++; if (strlen(colon)>=sizeof origin) { reject(client,400); return; } strcpy(origin,colon); }
        if (!strcasecmp(line,"Upgrade")) { upgrades++; if (strlen(colon)>=sizeof upgrade) { reject(client,400); return; } strcpy(upgrade,colon); }
        if (!strcasecmp(line,"Transfer-Encoding")) has_transfer=1;
        if (!strcasecmp(line,"Content-Length")) {
            lengths++; if (!*colon || strspn(colon,"0123456789")!=strlen(colon)) { reject(client,400); return; }
            char *tail; unsigned long n=strtoul(colon,&tail,10); if (*tail || n>BODY) { reject(client,413); return; } length=(size_t)n;
        }
        if (!strcasecmp(line,"Host") || !strcasecmp(line,"Connection") || !strcasecmp(line,"Upgrade") ||
            !strcasecmp(line,"Expect") || !strcasecmp(line,"Proxy-Authorization") || !strncasecmp(line,"X-Forwarded-",12)) continue;
        if (!append(output,line,colon)) { reject(client,431); return; }
    }
    if (encodings>1 || conditions>1 || hosts!=1 || origins>1 || lengths>1 || upgrades>1 || has_transfer ||
        (!is_post && length) || (is_post && lengths!=1)) { reject(client,400); return; }
    if ((is_post || ws) && (origins!=1 || strcmp(origin,expected_origin))) { reject(client,403); return; }
    if (ws && (strcasecmp(upgrade,"websocket") || length || strcmp(request_target,"/audio"))) { reject(client,400); return; }
    if (!ws && upgrades) { reject(client,400); return; }
    if(asset){serve_asset(client,path,request_target,method,encoding,conditional);return;}
    if (!append(output,"Host",upstream) || !append(output,"Connection",ws?"Upgrade":"close") ||
        (ws && !append(output,"Upgrade","websocket")) || strlen(output)+2>=HEADER) { reject(client,431); return; }
    strcat(output,"\r\n");
    int backend=connect_backend(ws?audio_port:web_port); if (backend<0) { reject(client,502); return; }
    long long deadline=now_ms()+15000;
    if (!write_all(backend,output,strlen(output),deadline)) goto done;
    char body[BODY]; size_t received=0;
    while (received<length) {
        if (!wait_io(client,POLLIN,deadline)) goto done;
        ssize_t n=read(client,body+received,length-received);
        if (n>0) received+=(size_t)n;
        else if (n<0 && (errno==EAGAIN || errno==EINTR)) continue;
        else goto done;
    }
    if (!write_all(backend,body,length,deadline)) goto done;
    if (!read_header(backend,input,now_ms()+60000)) { reject(client,502); goto done; }
    end=strstr(input,"\r\n"); if (!end) goto done; *end=0;
    int status=0, event_type=0;
    (void)sscanf(input,"HTTP/%*s %d",&status);
    snprintf(output,sizeof output,"%s\r\n",input);
    for (char *line=strtok_r(end+2,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)) {
        char *colon=strchr(line,':'); if (!colon) goto done; *colon++=0; while (*colon==' ') colon++;
        if (!strcasecmp(line,"Content-Type") && !strncasecmp(colon,"text/event-stream",17) &&
            (!colon[17] || colon[17]==';')) event_type=1;
        if (!strcasecmp(line,"Cache-Control") || !strcasecmp(line,"X-Content-Type-Options") ||
            (!ws && !strcasecmp(line,"Connection")) || (asset && !strcasecmp(line,"Content-Type"))) continue;
        if (!append(output,line,colon)) goto done;
    }
    if (asset && !append(output,"Content-Type",strstr(path,".js")?"text/javascript; charset=utf-8":strstr(path,".css")?"text/css; charset=utf-8":"text/html; charset=utf-8")) goto done;
    if (!append(output,"Cache-Control","no-store") || !append(output,"X-Content-Type-Options","nosniff") ||
        (!ws && !append(output,"Connection","close")) || strlen(output)+2>=HEADER) goto done;
    strcat(output,"\r\n"); if (!write_all(client,output,strlen(output),now_ms()+5000)) goto done;
    relay(client,backend,ws,status==200 && event_type && !strcmp(path,"/cgi-bin/api/events"));
done:
    close(backend);
}
int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--version")) { puts("qdc-entry 0.1.9"); return 0; }
    if (argc!=2 || strncmp(argv[1],"https://",8) || strlen(argv[1])>=sizeof expected_origin || strpbrk(argv[1]+8,"/ \t\r\n?#")) return 2;
    strcpy(expected_origin,argv[1]);
#ifdef ENTRY_TEST
    if (getenv("ENTRY_PORT")) port=(unsigned short)atoi(getenv("ENTRY_PORT"));
    if (getenv("ENTRY_WEB_PORT")) web_port=(unsigned short)atoi(getenv("ENTRY_WEB_PORT"));
    if (getenv("ENTRY_AUDIO_PORT")) audio_port=(unsigned short)atoi(getenv("ENTRY_AUDIO_PORT"));
    upstream="127.0.0.1";
    if(getenv("ENTRY_ASSET_ROOT"))asset_root=getenv("ENTRY_ASSET_ROOT");
#endif
    struct sigaction sa={0}; sa.sa_handler=stop; sigemptyset(&sa.sa_mask); sigaction(SIGTERM,&sa,NULL); sigaction(SIGINT,&sa,NULL); signal(SIGPIPE,SIG_IGN);
    listener=socket(AF_INET,SOCK_STREAM,0); if (listener<0) return 1;
    fcntl(listener,F_SETFD,FD_CLOEXEC); int one=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(0x7f000001U); a.sin_port=htons(port);
    if (bind(listener,(struct sockaddr *)&a,sizeof a) || listen(listener,LIMIT)) return 1;
    fprintf(stderr,"qdc-entry 0.1.9 listening on loopback:%u\n",port);
    int children=0; pid_t workers[LIMIT]={0};
    long long next_sample = 0;
    long long last_traffic_error = -60000;
    int traffic_failed = 0;
#ifdef ENTRY_TEST
    int collect = getenv("QDC_WEB_ROOT") != NULL;
#else
    int collect = 1;
#endif
    while (!stopping) {
        if (collect && now_ms() >= next_sample) {
            errno = 0;
            if (qdc_traffic_update(NULL, 0, 0)) {
                if (traffic_failed) fprintf(stderr,"Traffic collection recovered.\n");
                traffic_failed = 0;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                int error = errno; long long now = now_ms();
                if (!traffic_failed || now - last_traffic_error >= 60000) {
                    fprintf(stderr,"Traffic collection/checkpoint failed: %s\n",error ? strerror(error) : "state unavailable");
                    last_traffic_error = now;
                }
                traffic_failed = 1;
            }
            next_sample = now_ms() + 5000;
        }
        pid_t reaped;
        while ((reaped=waitpid(-1,NULL,WNOHANG))>0) {
            for (int i=0;i<LIMIT;i++) if (workers[i]==reaped) { workers[i]=0; children--; break; }
        }
        struct pollfd p={listener,POLLIN,0}; if (poll(&p,1,250)<=0) continue;
        int client=accept(listener,NULL,NULL); if (client<0) continue; fcntl(client,F_SETFL,O_NONBLOCK);
        if (children>=LIMIT) { reject(client,503); close(client); continue; }
        pid_t pid=fork();
        if (!pid) { close(listener); serve(client); close(client); _exit(0); }
        if (pid>0) {
            for (int i=0;i<LIMIT;i++) if (!workers[i]) { workers[i]=pid; children++; break; }
        } else reject(client,503);
        close(client);
    }
    close(listener);
    int checkpoint_ok = 1;
    if (collect) {
        errno = 0;
        checkpoint_ok = qdc_traffic_update(NULL, 0, 1);
        if (!checkpoint_ok) fprintf(stderr,"Final traffic checkpoint failed: %s\n",errno ? strerror(errno) : "state unavailable");
    }
    for (int i=0;i<LIMIT;i++) if (workers[i]>0) kill(workers[i],SIGTERM);
    return checkpoint_ok ? 0 : 1;
}
