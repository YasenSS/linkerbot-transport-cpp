// glibc requires this exact feature-test macro for the Linux socket extensions.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/sock_diag.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

/* No per-frame hooks. Observe the original binary's sockets at open/close.
 * Role numbering follows stress.cpp: first CAN socket Session, second peer.
 * SO_RCVBUFFORCE changes only this socket; no global sysctl changes. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int roles[65536];
static int next_role;
static void record(int fd, int role, const char *event, int tune_errno) {
    int saved = errno;
    uint32_t mem[SK_MEMINFO_VARS] = {0};
    socklen_t size = sizeof(mem);
    int rc = getsockopt(fd, SOL_SOCKET, SO_MEMINFO, mem, &size);
    int probe_errno = rc ? errno : 0;
    const char *path = getenv("LBT_PROBE_LOG");
    if (path) {
        char line[768];
        // Linux does not provide Annex K snprintf_s; bound and check this output.
        // NOLINTBEGIN(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        int n =
            snprintf(line, sizeof(line),
                     "{\"event\":\"%s\",\"role\":\"%s\",\"fd\":%d,\"rcvbuf\":%u,\"rmem_alloc\":%u,"
                     "\"drops\":%u,\"probe_errno\":%d,\"tune_errno\":%d}\n",
                     event, role == 1 ? "session" : "peer", fd, mem[SK_MEMINFO_RCVBUF],
                     mem[SK_MEMINFO_RMEM_ALLOC], mem[SK_MEMINFO_DROPS], probe_errno, tune_errno);
        // NOLINTEND(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
        if (n > 0 && (size_t)n < sizeof(line)) {
            int out = (int)syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (out >= 0) {
                syscall(SYS_write, out, line, (size_t)n);
                syscall(SYS_close, out);
            }
        }
    }
    errno = saved;
}
int socket(int domain, int type, int protocol) {
    int fd = (int)syscall(SYS_socket, domain, type, protocol);
    if (fd >= 0 && fd < 65536 && domain == PF_CAN) {
        pthread_mutex_lock(&lock);
        int role = ++next_role;
        roles[fd] = role;
        const char *s = getenv(role == 1 ? "LBT_SESSION_RCVBUF" : "LBT_PEER_RCVBUF");
        int tune_errno = 0;
        if (s) {
            int bytes = atoi(s);
            if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bytes, sizeof(bytes)))
                tune_errno = errno;
        }
        record(fd, role, "open", tune_errno);
        pthread_mutex_unlock(&lock);
    }
    return fd;
}
int close(int fd) {
    if (fd >= 0 && fd < 65536) {
        pthread_mutex_lock(&lock);
        if (roles[fd]) {
            record(fd, roles[fd], "close", 0);
            roles[fd] = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return (int)syscall(SYS_close, fd);
}
