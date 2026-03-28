#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <errno.h>

extern char **environ;

void setup_env(struct passwd *pw, int login_shell) {
    if (login_shell) {
        clearenv();
    }

    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);

    if (login_shell) {
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    }
}

int main(int argc, char *argv[]) {
    struct passwd *pw = getpwuid(0); // default root
    const char *command = NULL;
    int login_shell = 0;

    // --- argument parsing ---
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0) {
            login_shell = 1;
        } else {
            pw = getpwnam(argv[i]);
            if (!pw) {
                fprintf(stderr, "Unknown user: %s\n", argv[i]);
                return 1;
            }
        }
    }

    // --- initialize groups ---
    if (initgroups(pw->pw_name, pw->pw_gid) != 0) {
        perror("initgroups failed");
        return 1;
    }

    // --- set gid/uid ---
    if (setgid(pw->pw_gid) != 0) {
        perror("setgid failed");
        return 1;
    }

    if (setuid(pw->pw_uid) != 0) {
        perror("setuid failed");
        return 1;
    }

    // --- setup environment ---
    setup_env(pw, login_shell);

    // --- change directory ---
    if (chdir(pw->pw_dir) != 0) {
        fprintf(stderr, "Warning: could not chdir to %s: %s\n",
                pw->pw_dir, strerror(errno));
    }

    // --- exec ---
    if (command) {
        execl(pw->pw_shell, pw->pw_shell, "-c", command, NULL);
    } else {
        if (login_shell) {
            // prepend '-' to indicate login shell
            char shell_name[256];
            snprintf(shell_name, sizeof(shell_name), "-%s",
                     strrchr(pw->pw_shell, '/') ?
                     strrchr(pw->pw_shell, '/') + 1 :
                     pw->pw_shell);

            execl(pw->pw_shell, shell_name, NULL);
        } else {
            execl(pw->pw_shell, pw->pw_shell, NULL);
        }
    }

    perror("exec failed");
    return 1;
}