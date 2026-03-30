/*
 * su.c - Su client for Android (runs as normal user)
 *
 * Puts the local terminal into raw mode and relays to/from sudaemon.
 * Output is forwarded immediately with no holdback.
 * Exit code is received as a framed packet: 0xFF 0xFF <code>
 *
 * Usage:
 *   su                      open root shell
 *   su <user|uid>           open shell as user
 *   su -c "cmd"             run command as root
 *   su <user|uid> -c "cmd"  run command as user
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
#define EXIT_MAGIC1  0xFF
#define EXIT_MAGIC2  0xFF

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
        "  %s <user|uid> -c <cmd>  Run command as user\n",
        prog, prog, prog, prog);
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
        if (c == EXIT_MAGIC1) { return 0; }
        write(STDOUT_FILENO, &c, 1);
        parse_state = 0;
        return 0;
    case 2:
        exit_code = (int)c;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[]) {
    uid_t target_uid   = 0;
    gid_t target_gid   = 0;
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
            if (++i >= argc) { fprintf(stderr, "su: -c requires an argument\n"); return 1; }
            strncpy(command, argv[i++], sizeof(command) - 1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
        } else {
            fprintf(stderr, "su: unknown option: %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    /* Connect */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("su: socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "su: cannot connect to sudaemon (%s): %s\n"
                        "    Is sudaemon running as root?\n",
                SOCKET_PATH, strerror(errno));
        close(sock);
        return 1;
    }

    /* Terminal size */
    int cols = 80, rows = 24;
    if (isatty(STDIN_FILENO)) {
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col > 0) cols = ws.ws_col;
            if (ws.ws_row > 0) rows = ws.ws_row;
        }
    }

    /* Current working directory */
    char cwd[BUF_SIZE] = "";
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        cwd[0] = '\0'; /* daemon will ignore and use its own cwd */

    /* Send header */
    char uid_str[32], gid_str[32], cols_str[16], rows_str[16];
    snprintf(uid_str,  sizeof(uid_str),  "%d", (int)target_uid);
    snprintf(gid_str,  sizeof(gid_str),  "%d", (int)target_gid);
    snprintf(cols_str, sizeof(cols_str), "%d", cols);
    snprintf(rows_str, sizeof(rows_str), "%d", rows);

    if (send_kv(sock, "UID",  uid_str)     < 0 ||
        send_kv(sock, "GID",  gid_str)     < 0 ||
        send_kv(sock, "USER", target_name) < 0 ||
        send_kv(sock, "CMD",  command)     < 0 ||
        send_kv(sock, "COLS", cols_str)    < 0 ||
        send_kv(sock, "ROWS", rows_str)    < 0 ||
        send_kv(sock, "CWD",  cwd)         < 0 ||
        write(sock, "END\n", 4) != 4) {
        fprintf(stderr, "su: failed to send request\n");
        close(sock);
        return 1;
    }

    set_raw();

    int stdin_open = 1;
    char buf[BUF_SIZE];

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
            if (n <= 0) {
                shutdown(sock, SHUT_WR);
                stdin_open = 0;
            } else {
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
            for (ssize_t j = 0; j < n; j++) {
                if (process_byte((unsigned char)buf[j]))
                    goto done;
            }
        }
    }

done:
    restore_term();
    term_raw = 0;
    close(sock);
    return exit_code;
}
