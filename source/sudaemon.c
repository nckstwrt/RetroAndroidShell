/*
 * sudaemon.c - Su Daemon for Android (runs as root)
 *
 * PTY mode  (INTERACTIVE=1): full PTY relay for terminal use
 * Pipe mode (INTERACTIVE=0): forks a root shell, passes its stdin/stdout/stderr
 *                            fds back to su via SCM_RIGHTS, then exits.
 *                            su splices them directly onto its own stdio with
 *                            zero buffering — exactly what libsu expects.
 *
 * Build (NDK, Windows):
 *   aarch64-linux-android21-clang sudaemon.c -o sudaemon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
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

static void resolve_shell(struct passwd *pw, char *shell, int shelllen) {
    const char *candidates[] = {
        pw ? pw->pw_shell : NULL,
        "/system/bin/sh",
        "/bin/sh",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (candidates[i] && candidates[i][0] &&
            access(candidates[i], X_OK) == 0) {
            strncpy(shell, candidates[i], shelllen - 1);
            shell[shelllen - 1] = '\0';
            return;
        }
    }
    strncpy(shell, "/system/bin/sh", shelllen - 1);
}

static int drop_privs(const char *username, uid_t uid, gid_t gid) {
    if (initgroups(username, gid) < 0)
        setgroups(0, NULL);
    if (setgid(gid) < 0) {
        fprintf(stderr, "sudaemon: setgid(%d): %s\n", (int)gid, strerror(errno));
        return -1;
    }
    if (setuid(uid) < 0) {
        fprintf(stderr, "sudaemon: setuid(%d): %s\n", (int)uid, strerror(errno));
        return -1;
    }
    return 0;
}

static void send_exit(int fd, int status) {
    unsigned char code = (unsigned char)(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    unsigned char pkt[3] = { EXIT_MAGIC1, EXIT_MAGIC2, code };
    write(fd, pkt, 3);
}

/* ----------------------------------------------------------------
 * send_fds: send up to 3 file descriptors over a Unix socket
 * using SCM_RIGHTS ancillary data.
 * ---------------------------------------------------------------- */
static int send_fds(int sock, int *fds, int nfds) {
    char dummy = 0;
    struct iovec iov = { &dummy, 1 };

    size_t cmsg_space = CMSG_SPACE(nfds * sizeof(int));
    char *cbuf = calloc(1, cmsg_space);
    if (!cbuf) return -1;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cbuf;
    msg.msg_controllen = cmsg_space;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(nfds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, nfds * sizeof(int));

    int ret = (sendmsg(sock, &msg, 0) >= 0) ? 0 : -1;
    free(cbuf);
    return ret;
}

/* ============================================================
 * PTY mode — full relay for interactive terminal use
 * ============================================================ */
static void handle_pty(int cfd,
                       uid_t uid, gid_t gid,
                       const char *username, const char *shell,
                       const char *command, const char *cwd,
                       int cols, int rows,
                       struct passwd *pw) {
    char slave_name[64];
    int master = open_ptm(slave_name, sizeof(slave_name));
    if (master < 0) {
        fprintf(stderr, "sudaemon: open_ptm: %s\n", strerror(errno));
        return;
    }

    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid < 0) { close(master); return; }

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

        if (drop_privs(username, uid, gid) < 0) _exit(1);
        if (cwd[0]) chdir(cwd);

        setenv("USER",    username, 1);
        setenv("LOGNAME", username, 1);
        setenv("SHELL",   shell,    1);
        setenv("TERM",    "xterm",  1);
        setenv("PATH", "/system/bin:/system/xbin:/vendor/bin:/data/local/tmp", 1);
        if (pw && pw->pw_dir) setenv("HOME", pw->pw_dir, 1);

        if (command[0]) {
            char *argv[] = { (char*)shell, "-c", (char*)command, NULL };
            execv(shell, argv);
        } else {
            char *argv[] = { (char*)shell, NULL };
            execv(shell, argv);
        }
        _exit(127);
    }

    char buf[BUF_SIZE];
    int cfd_open = 1;
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (cfd_open) FD_SET(cfd,    &rfds);
        FD_SET(master, &rfds);
        int maxfd = (master > cfd ? master : cfd) + 1;

        if (select(maxfd, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) { waitpid(pid, NULL, WNOHANG); continue; }
            break;
        }
        if (cfd_open && FD_ISSET(cfd, &rfds)) {
            ssize_t n = read(cfd, buf, sizeof(buf));
            if (n <= 0) { cfd_open = 0; write(master, "\x04", 1); }
            else {
                ssize_t off = 0;
                while (off < n) { ssize_t w = write(master, buf+off, n-off); if (w<=0) break; off+=w; }
            }
        }
        if (FD_ISSET(master, &rfds)) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n <= 0) break;
            ssize_t off = 0;
            while (off < n) { ssize_t w = write(cfd, buf+off, n-off); if (w<=0) goto pty_done; off+=w; }
        }
    }
pty_done:
    close(master);
    int status = 0;
    waitpid(pid, &status, 0);
    send_exit(cfd, status);
}

/* ============================================================
 * Pipe mode — for libsu and programmatic callers
 *
 * Strategy:
 *   1. Fork a root shell with two pipes (in_pipe, out_pipe).
 *   2. Send the three pipe fds (write-end of in_pipe,
 *      read-end of out_pipe, read-end of out_pipe for stderr)
 *      back to su via SCM_RIGHTS on the socket.
 *   3. Wait for the shell to exit, send exit code.
 *
 * su then closes the socket, dups those fds onto its own
 * stdin/stdout/stderr, and becomes completely transparent —
 * the calling app talks directly to the shell with zero
 * buffering or protocol overhead.
 * ============================================================ */
static void handle_pipe(int cfd,
                        uid_t uid, gid_t gid,
                        const char *username, const char *shell,
                        const char *command, const char *cwd,
                        struct passwd *pw) {
    int in_pipe[2], out_pipe[2], err_pipe[2];

    if (pipe(in_pipe)  < 0) return;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]);  close(in_pipe[1]);  return; }
    if (pipe(err_pipe) < 0) { close(in_pipe[0]);  close(in_pipe[1]);
                               close(out_pipe[0]); close(out_pipe[1]); return; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return;
    }

    if (pid == 0) {
        /* child: shell process */
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        close(cfd);

        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[1]);

        if (drop_privs(username, uid, gid) < 0) _exit(1);
        if (cwd[0]) chdir(cwd);

        setenv("USER",    username, 1);
        setenv("LOGNAME", username, 1);
        setenv("SHELL",   shell,    1);
        setenv("PATH", "/system/bin:/system/xbin:/vendor/bin:/data/local/tmp", 1);
        if (pw && pw->pw_dir) setenv("HOME", pw->pw_dir, 1);

        if (command[0]) {
            char *argv[] = { (char*)shell, "-c", (char*)command, NULL };
            execv(shell, argv);
        } else {
            char *argv[] = { (char*)shell, NULL };
            execv(shell, argv);
        }
        _exit(127);
    }

    /* parent: close the ends the child owns */
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);

    /*
     * Send the three fds to su:
     *   fds[0] = write end of in_pipe  → su dups to its stdout then writes → shell stdin
     *   fds[1] = read  end of out_pipe → su dups to its stdin  then reads  ← shell stdout
     *   fds[2] = read  end of err_pipe → su dups to its stderr             ← shell stderr
     *
     * Actually we send: [in_pipe[1], out_pipe[0], err_pipe[0]]
     * su will: dup2(fds[0], STDIN)  ← no, su dups them as:
     *   STDIN_FILENO  = out_pipe[0]  (read shell output)
     *   STDOUT_FILENO = in_pipe[1]   (write to shell stdin)
     *   STDERR_FILENO = err_pipe[0]  (read shell stderr)
     *
     * See su.c for the exact dup2 ordering.
     */
    int fds[3] = { in_pipe[1], out_pipe[0], err_pipe[0] };
    if (send_fds(cfd, fds, 3) < 0) {
        /* Failed to send fds — kill the shell and give up */
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(err_pipe[0]);
        return;
    }

    /* Close our copies — su now owns them */
    close(in_pipe[1]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    /* Wait for the shell to exit, report exit code */
    int status = 0;
    waitpid(pid, &status, 0);
    send_exit(cfd, status);
}

/* ============================================================ */

static void handle_client(int cfd) {
    char line[BUF_SIZE];
    uid_t target_uid    = 0;
    gid_t target_gid    = 0;
    char username[64]   = "root";
    char command[BUF_SIZE] = "";
    char cwd[BUF_SIZE]     = "";
    int  cols = 80, rows = 24;
    int  interactive = 1;

    while (1) {
        if (read_line(cfd, line, sizeof(line)) < 0) { close(cfd); return; }
        if (strcmp(line, "END") == 0) break;
        if      (strncmp(line, "UID=",         4)  == 0) target_uid   = (uid_t)atoi(line + 4);
        else if (strncmp(line, "GID=",         4)  == 0) target_gid   = (gid_t)atoi(line + 4);
        else if (strncmp(line, "USER=",        5)  == 0) strncpy(username, line + 5, sizeof(username) - 1);
        else if (strncmp(line, "CMD=",         4)  == 0) strncpy(command,  line + 4, sizeof(command)  - 1);
        else if (strncmp(line, "CWD=",         4)  == 0) strncpy(cwd,      line + 4, sizeof(cwd)      - 1);
        else if (strncmp(line, "COLS=",        5)  == 0) cols        = atoi(line + 5);
        else if (strncmp(line, "ROWS=",        5)  == 0) rows        = atoi(line + 5);
        else if (strncmp(line, "INTERACTIVE=", 12) == 0) interactive = atoi(line + 12);
    }

    struct passwd *pw = getpwnam(username);
    if (!pw) pw = getpwuid(target_uid);
    if (pw) { target_uid = pw->pw_uid; target_gid = pw->pw_gid; }

    char shell[128];
    resolve_shell(pw, shell, sizeof(shell));

    if (interactive)
        handle_pty(cfd, target_uid, target_gid, username, shell,
                   command, cwd, cols, rows, pw);
    else
        handle_pipe(cfd, target_uid, target_gid, username, shell,
                    command, cwd, pw);

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
