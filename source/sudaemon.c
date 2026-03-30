/*
 * sudaemon.c - Su Daemon for Android (runs as root)
 *
 * Uses a PTY (via /dev/ptmx) so interactive shells work correctly.
 *
 * Build (NDK, Windows):
 *   aarch64-linux-android21-clang sudaemon.c -o sudaemon
 *
 * Deploy:
 *   adb push sudaemon /data/local/tmp/sudaemon
 *   adb shell "chmod 700 /data/local/tmp/sudaemon"
 *   adb shell "/data/local/tmp/sudaemon > /dev/null 2>&1 &"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#define SOCKET_PATH  "/dev/socket/sudaemon"
#define BACKLOG      8
#define BUF_SIZE     4096
#define EXIT_MAGIC1  0xFF
#define EXIT_MAGIC2  0xFF

static void die(const char *msg) { perror(msg); exit(1); }

static int read_line(int fd, char *buf, int len) {
    int i = 0;
    while (i < len - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) return -1;
        if (c == '\n') { buf[i] = '\0'; return i; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

static int open_ptm(char *slave_name, int namelen) {
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) return -1;
    if (grantpt(master)  < 0) { close(master); return -1; }
    if (unlockpt(master) < 0) { close(master); return -1; }
    char *name = ptsname(master);
    if (!name) { close(master); return -1; }
    strncpy(slave_name, name, namelen - 1);
    slave_name[namelen - 1] = '\0';
    return master;
}

static void handle_client(int cfd) {
    char line[BUF_SIZE];
    uid_t target_uid   = 0;
    gid_t target_gid   = 0;
    char username[64]  = "root";
    char command[BUF_SIZE] = "";
    char cwd[BUF_SIZE]     = "";
    int  cols = 80, rows = 24;

    while (1) {
        if (read_line(cfd, line, sizeof(line)) < 0) { close(cfd); return; }
        if (strcmp(line, "END") == 0) break;
        if      (strncmp(line, "UID=",  4) == 0) target_uid = (uid_t)atoi(line + 4);
        else if (strncmp(line, "GID=",  4) == 0) target_gid = (gid_t)atoi(line + 4);
        else if (strncmp(line, "USER=", 5) == 0) strncpy(username, line + 5, sizeof(username) - 1);
        else if (strncmp(line, "CMD=",  4) == 0) strncpy(command,  line + 4, sizeof(command)  - 1);
        else if (strncmp(line, "CWD=",  4) == 0) strncpy(cwd,      line + 4, sizeof(cwd)      - 1);
        else if (strncmp(line, "COLS=", 5) == 0) cols = atoi(line + 5);
        else if (strncmp(line, "ROWS=", 5) == 0) rows = atoi(line + 5);
    }

    char shell[128] = "/system/bin/sh";
    struct passwd *pw = getpwnam(username);
    if (!pw) pw = getpwuid(target_uid);
    if (pw && pw->pw_shell && pw->pw_shell[0])
        strncpy(shell, pw->pw_shell, sizeof(shell) - 1);

    char slave_name[64];
    int master = open_ptm(slave_name, sizeof(slave_name));
    if (master < 0) {
        fprintf(stderr, "sudaemon: open_ptm: %s\n", strerror(errno));
        close(cfd);
        return;
    }

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid < 0) { close(master); close(cfd); return; }

    if (pid == 0) {
        close(master);
        close(cfd);
        setsid();

        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(1);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);

        if (setgid(target_gid) < 0) { perror("setgid"); _exit(1); }
        if (setuid(target_uid) < 0) { perror("setuid"); _exit(1); }

        /* Change to the caller's directory; silently fall back if it fails
           (e.g. the target user has no permission to access it). */
        if (cwd[0] != '\0')
            chdir(cwd);

        setenv("USER",    username, 1);
        setenv("LOGNAME", username, 1);
        setenv("SHELL",   shell,    1);
        setenv("TERM",    "xterm",  1);
        setenv("PATH", "/system/bin:/system/xbin:/vendor/bin:/data/local/tmp", 1);
        if (pw && pw->pw_dir) setenv("HOME", pw->pw_dir, 1);

        if (command[0]) {
            char *argv[] = { shell, "-c", command, NULL };
            execv(shell, argv);
        } else {
            char *argv[] = { shell, NULL };
            execv(shell, argv);
        }
        _exit(127);
    }

    /* Relay loop */
    char buf[BUF_SIZE];
    int  cfd_open = 1;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (cfd_open) FD_SET(cfd,    &rfds);
        FD_SET(master, &rfds);
        int maxfd = (master > cfd ? master : cfd) + 1;

        if (select(maxfd, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                if (waitpid(pid, NULL, WNOHANG) > 0) break;
                continue;
            }
            break;
        }

        if (cfd_open && FD_ISSET(cfd, &rfds)) {
            ssize_t n = read(cfd, buf, sizeof(buf));
            if (n <= 0) {
                cfd_open = 0;
                write(master, "\x04", 1);
            } else {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(master, buf + off, n - off);
                    if (w <= 0) break;
                    off += w;
                }
            }
        }

        if (FD_ISSET(master, &rfds)) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(cfd, buf + off, n - off);
                if (w <= 0) goto done;
                off += w;
            }
        }
    }

done:
    close(master);

    int status = 0;
    waitpid(pid, &status, 0);
    unsigned char code = (unsigned char)(WIFEXITED(status) ? WEXITSTATUS(status) : 1);

    unsigned char pkt[3] = { EXIT_MAGIC1, EXIT_MAGIC2, code };
    write(cfd, pkt, 3);
    close(cfd);
}

int main(void) {
    if (getuid() != 0) {
        fprintf(stderr, "sudaemon: must be run as root\n");
        return 1;
    }

    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    unlink(SOCKET_PATH);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) die("socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    chmod(SOCKET_PATH, 0666);
    if (listen(srv, BACKLOG) < 0) die("listen");

    fprintf(stdout, "sudaemon: listening on %s\n", SOCKET_PATH);
    fflush(stdout);

    while (1) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(srv);
            handle_client(cfd);
            _exit(0);
        }
        close(cfd);
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }
    return 0;
}
