/*
 * su.c - Su client for Android
 *
 * Interactive mode (stdin is a TTY):
 *   Puts terminal in raw mode, asks daemon for a PTY shell, relays I/O.
 *
 * Pipe mode (stdin is NOT a TTY, e.g. libsu):
 *   Asks daemon to fork a root shell and return its pipe fds via SCM_RIGHTS.
 *   Relays between caller's original stdin/stdout/stderr and the shell pipes.
 *   Zero extra buffering — libsu talks to the shell at full speed.
 *
 * Flags handled for compatibility:
 *   -v / --version   print version string and exit
 *   -V               print version code and exit
 *   --mount-master   silently ignored (Magisk compat)
 *   -c <cmd>         run command as root
 *   <user|uid>       switch to user
 *
 * Build (NDK, Windows):
 *   aarch64-linux-android21-clang su.c -o su
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

#define SOCKET_PATH  "/dev/socket/sudaemon"
#define BUF_SIZE     4096
#define EXIT_MAGIC1  ((unsigned char)0xFF)
#define EXIT_MAGIC2  ((unsigned char)0xFF)

#define SU_VERSION      "RetroAndroidShell"
#define SU_VERSION_CODE "003"

/* ------------------------------------------------------------------ terminal */

static struct termios saved_termios;
static int            term_raw = 0;

static void restore_term(void) {
    if (term_raw)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
}

static void set_raw(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0) return;
    atexit(restore_term);
    term_raw = 1;
    struct termios raw = saved_termios;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
                   | INLCR  | IGNCR  | ICRNL  | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |=  CS8;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* ------------------------------------------------------------------ helpers */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s                      Open root shell\n"
        "  %s <user|uid>           Open shell as user\n"
        "  %s -c <cmd>             Run command as root\n"
        "  %s <user|uid> -c <cmd>  Run command as user\n"
        "  %s -v                   Print version\n",
        prog, prog, prog, prog, prog);
    exit(1);
}

static int resolve_user(const char *spec,
                        uid_t *uid, gid_t *gid,
                        char *name, int namelen) {
    struct passwd *pw = NULL;
    char *end;
    long val = strtol(spec, &end, 10);
    if (*end == '\0') {
        pw = getpwuid((uid_t)val);
        if (!pw) {
            *uid = (uid_t)val;
            *gid = (gid_t)val;
            snprintf(name, namelen, "uid_%ld", val);
            return 0;
        }
    } else {
        pw = getpwnam(spec);
    }
    if (!pw) { fprintf(stderr, "su: unknown user: %s\n", spec); return -1; }
    *uid = pw->pw_uid;
    *gid = pw->pw_gid;
    strncpy(name, pw->pw_name, namelen - 1);
    name[namelen - 1] = '\0';
    return 0;
}

static int send_kv(int fd, const char *key, const char *value) {
    char buf[BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "%s=%s\n", key, value);
    return write(fd, buf, n) == n ? 0 : -1;
}

/* Exit-packet scanner for interactive mode */
static int exit_code   = 0;
static int parse_state = 0;

static int process_byte(unsigned char c) {
    switch (parse_state) {
    case 0:
        if (c == EXIT_MAGIC1) { parse_state = 1; return 0; }
        write(STDOUT_FILENO, &c, 1);
        return 0;
    case 1:
        if (c == EXIT_MAGIC2) { parse_state = 2; return 0; }
        { unsigned char ff = EXIT_MAGIC1; write(STDOUT_FILENO, &ff, 1); }
        if (c == EXIT_MAGIC1) return 0;
        write(STDOUT_FILENO, &c, 1);
        parse_state = 0;
        return 0;
    case 2:
        exit_code = (int)c;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ fd passing */

static int recv_fds(int sock, int *fds, int max_fds) {
    char dummy;
    struct iovec iov = { &dummy, 1 };

    size_t cmsg_space = CMSG_SPACE(max_fds * sizeof(int));
    char *cbuf = calloc(1, cmsg_space);
    if (!cbuf) return -1;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cbuf;
    msg.msg_controllen = cmsg_space;

    int ret = -1;
    if (recvmsg(sock, &msg, 0) >= 0) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg &&
            cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type  == SCM_RIGHTS) {
            int n = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (n > max_fds) n = max_fds;
            memcpy(fds, CMSG_DATA(cmsg), n * sizeof(int));
            ret = n;
        }
    }
    free(cbuf);
    return ret;
}

/* ------------------------------------------------------------------ connect */

static int connect_daemon(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("su: socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "su: cannot connect to sudaemon (%s): %s\n"
                        "    Is sudaemon running as root?\n",
                SOCKET_PATH, strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[]) {
    uid_t target_uid        = 0;
    gid_t target_gid        = 0;
    char  target_name[64]   = "root";
    char  command[BUF_SIZE] = "";

    int i = 1;

    if (i < argc && argv[i][0] != '-') {
        if (resolve_user(argv[i], &target_uid, &target_gid,
                         target_name, sizeof(target_name)) < 0)
            return 1;
        i++;
    } else {
        struct passwd *pw = getpwnam("root");
        if (pw) { target_uid = pw->pw_uid; target_gid = pw->pw_gid; }
    }

    while (i < argc) {
        if (strcmp(argv[i], "-c") == 0) {
            if (++i >= argc) { fprintf(stderr, "su: -c requires argument\n"); return 1; }
            strncpy(command, argv[i++], sizeof(command) - 1);

        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            printf("%s\n", SU_VERSION);
            return 0;

        } else if (strcmp(argv[i], "-V") == 0) {
            printf("%s\n", SU_VERSION_CODE);
            return 0;

        } else if (strcmp(argv[i], "--mount-master") == 0 ||
                   strcmp(argv[i], "-mm")            == 0) {
            i++; /* silently ignored */

        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);

        } else {
            i++; /* ignore unknown flags for max compatibility */
        }
    }

    int interactive = isatty(STDIN_FILENO);

    /*
     * Save original stdin/stdout/stderr NOW before we do anything else.
     * In pipe mode we'll relay between these and the shell's pipes.
     * In interactive mode we use them directly.
     */
    int orig_stdin  = dup(STDIN_FILENO);
    int orig_stdout = dup(STDOUT_FILENO);
    int orig_stderr = dup(STDERR_FILENO);

    int sock = connect_daemon();
    if (sock < 0) return 1;

    /* Terminal size */
    int cols = 80, rows = 24;
    if (interactive) {
        struct winsize ws;
        if (ioctl(orig_stdin, TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col > 0) cols = ws.ws_col;
            if (ws.ws_row > 0) rows = ws.ws_row;
        }
    }

    char cwd[BUF_SIZE] = "";
    getcwd(cwd, sizeof(cwd));

    char uid_str[32], gid_str[32], cols_str[16], rows_str[16], mode_str[4];
    snprintf(uid_str,  sizeof(uid_str),  "%d", (int)target_uid);
    snprintf(gid_str,  sizeof(gid_str),  "%d", (int)target_gid);
    snprintf(cols_str, sizeof(cols_str), "%d", cols);
    snprintf(rows_str, sizeof(rows_str), "%d", rows);
    snprintf(mode_str, sizeof(mode_str), "%d", interactive ? 1 : 0);

    if (send_kv(sock, "UID",         uid_str)     < 0 ||
        send_kv(sock, "GID",         gid_str)     < 0 ||
        send_kv(sock, "USER",        target_name) < 0 ||
        send_kv(sock, "CMD",         command)     < 0 ||
        send_kv(sock, "COLS",        cols_str)    < 0 ||
        send_kv(sock, "ROWS",        rows_str)    < 0 ||
        send_kv(sock, "CWD",         cwd)         < 0 ||
        send_kv(sock, "INTERACTIVE", mode_str)    < 0 ||
        write(sock, "END\n", 4) != 4) {
        fprintf(stderr, "su: failed to send request\n");
        close(sock);
        return 1;
    }

    /* ============================================================
     * PIPE MODE
     *
     * Receive the shell's pipe fds from the daemon:
     *   fds[0] = write-end of shell's stdin  pipe  (we write caller input here)
     *   fds[1] = read-end  of shell's stdout pipe  (we read shell output here)
     *   fds[2] = read-end  of shell's stderr pipe  (we read shell errors here)
     *
     * Relay:
     *   orig_stdin  → fds[0]   (caller's keystrokes/commands → shell stdin)
     *   fds[1]      → orig_stdout  (shell stdout → caller)
     *   fds[2]      → orig_stderr  (shell stderr → caller)
     * ============================================================ */
    if (!interactive) {
        int fds[3] = { -1, -1, -1 };
        int n = recv_fds(sock, fds, 3);
        if (n < 3) {
            fprintf(stderr, "su: failed to receive shell fds (got %d)\n", n);
            close(sock);
            for (int j = 0; j < n; j++) if (fds[j] >= 0) close(fds[j]);
            return 1;
        }

        int shell_stdin_w  = fds[0];
        int shell_stdout_r = fds[1];
        int shell_stderr_r = fds[2];

        /* Close the socket — we have the fds we need */
        close(sock);

        char buf[BUF_SIZE];
        int caller_in_open  = 1;  /* orig_stdin still readable  */
        int shell_out_open  = 1;  /* shell stdout still readable */
        int shell_err_open  = 1;  /* shell stderr still readable */

        while (caller_in_open || shell_out_open || shell_err_open) {
            fd_set rfds;
            FD_ZERO(&rfds);
            if (caller_in_open)  FD_SET(orig_stdin,    &rfds);
            if (shell_out_open)  FD_SET(shell_stdout_r, &rfds);
            if (shell_err_open)  FD_SET(shell_stderr_r, &rfds);

            /* Find maxfd */
            int maxfd = orig_stdin;
            if (shell_stdout_r > maxfd) maxfd = shell_stdout_r;
            if (shell_stderr_r > maxfd) maxfd = shell_stderr_r;
            maxfd++;

            if (select(maxfd, &rfds, NULL, NULL, NULL) < 0) {
                if (errno == EINTR) continue;
                break;
            }

            /* caller stdin → shell stdin */
            if (caller_in_open && FD_ISSET(orig_stdin, &rfds)) {
                ssize_t nr = read(orig_stdin, buf, sizeof(buf));
                if (nr <= 0) {
                    close(shell_stdin_w);
                    shell_stdin_w = -1;
                    caller_in_open = 0;
                } else {
                    ssize_t off = 0;
                    while (off < nr) {
                        ssize_t nw = write(shell_stdin_w, buf + off, nr - off);
                        if (nw <= 0) { caller_in_open = 0; break; }
                        off += nw;
                    }
                }
            }

            /* shell stdout → caller stdout */
            if (shell_out_open && FD_ISSET(shell_stdout_r, &rfds)) {
                ssize_t nr = read(shell_stdout_r, buf, sizeof(buf));
                if (nr <= 0) {
                    shell_out_open = 0;
                } else {
                    ssize_t off = 0;
                    while (off < nr) {
                        ssize_t nw = write(orig_stdout, buf + off, nr - off);
                        if (nw <= 0) break;
                        off += nw;
                    }
                }
            }

            /* shell stderr → caller stderr */
            if (shell_err_open && FD_ISSET(shell_stderr_r, &rfds)) {
                ssize_t nr = read(shell_stderr_r, buf, sizeof(buf));
                if (nr <= 0) {
                    shell_err_open = 0;
                } else {
                    ssize_t off = 0;
                    while (off < nr) {
                        ssize_t nw = write(orig_stderr, buf + off, nr - off);
                        if (nw <= 0) break;
                        off += nw;
                    }
                }
            }
        }

        if (shell_stdin_w  >= 0) close(shell_stdin_w);
        close(shell_stdout_r);
        close(shell_stderr_r);
        close(orig_stdin);
        close(orig_stdout);
        close(orig_stderr);
        return exit_code; /* daemon sends exit packet but we don't need it here */
    }

    /* ============================================================
     * INTERACTIVE MODE — PTY relay
     * ============================================================ */
    close(orig_stdin);
    close(orig_stdout);
    close(orig_stderr);

    set_raw();
    char buf[BUF_SIZE];
    int stdin_open = 1;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (stdin_open) FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sock, &rfds);

        if (select(sock + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) { shutdown(sock, SHUT_WR); stdin_open = 0; }
            else {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(sock, buf + off, n - off);
                    if (w <= 0) goto done;
                    off += w;
                }
            }
        }
        if (FD_ISSET(sock, &rfds)) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n <= 0) break;
            for (ssize_t j = 0; j < n; j++)
                if (process_byte((unsigned char)buf[j])) goto done;
        }
    }
done:
    restore_term();
    term_raw = 0;
    close(sock);
    return exit_code;
}
