/*
 * chown.c - Simple chown for Android
 *
 * Supports:
 *   chown user file...
 *   chown user:group file...
 *   chown user. file...        (same as user:group, group looked up from passwd)
 *   chown :group file...       (group only, leave owner unchanged)
 *   chown -R ...               (recursive)
 *   chown -h ...               (affect symlinks themselves, not targets)
 *   chown --reference=REF file (copy owner:group from REF)
 *   numeric uid/gid accepted   (e.g. chown 2000:2000 file)
 *
 * Build (NDK, Windows):
 *   aarch64-linux-android21-clang chown.c -o chown
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

static int  flag_recursive  = 0;
static int  flag_symlink    = 0;  /* -h: act on symlink itself */
static int  flag_verbose    = 0;  /* -v */
static int  flag_changes    = 0;  /* -c: verbose only when changed */
static int  errors          = 0;

/* ------------------------------------------------------------------ helpers */

static uid_t resolve_uid(const char *s) {
    struct passwd *pw = getpwnam(s);
    if (pw) return pw->pw_uid;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end == '\0') return (uid_t)v;
    fprintf(stderr, "chown: invalid user: '%s'\n", s);
    exit(1);
}

static gid_t resolve_gid(const char *s) {
    struct group *gr = getgrnam(s);
    if (gr) return gr->gr_gid;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end == '\0') return (gid_t)v;
    fprintf(stderr, "chown: invalid group: '%s'\n", s);
    exit(1);
}

/*
 * Parse "user", "user:group", "user:", ":group", "user."
 * Sets *uid / *gid; leaves unchanged (keeps as (uid_t)-1) if not specified.
 */
static void parse_owner(const char *spec, uid_t *uid, gid_t *gid) {
    *uid = (uid_t)-1;
    *gid = (gid_t)-1;

    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Find separator: ':' or '.' */
    char *sep = strchr(buf, ':');
    if (!sep) sep = strchr(buf, '.');

    if (!sep) {
        /* "user" only */
        if (buf[0]) *uid = resolve_uid(buf);
        return;
    }

    *sep = '\0';
    char *user_part  = buf;
    char *group_part = sep + 1;

    if (user_part[0])  *uid = resolve_uid(user_part);

    if (group_part[0]) {
        *gid = resolve_gid(group_part);
    } else if (user_part[0]) {
        /* "user:" with no group — use user's primary group */
        struct passwd *pw = getpwnam(user_part);
        if (!pw) {
            /* numeric uid — no way to look up primary group, leave gid alone */
        } else {
            *gid = pw->pw_gid;
        }
    }
}

/* ------------------------------------------------------------------ apply */

static void apply(const char *path, uid_t uid, gid_t gid) {
    int ret;

    if (flag_symlink) {
        ret = lchown(path, uid, gid);
    } else {
        ret = chown(path, uid, gid);
    }

    if (ret < 0) {
        fprintf(stderr, "chown: changing ownership of '%s': %s\n",
                path, strerror(errno));
        errors = 1;
        return;
    }

    if (flag_verbose) {
        /* print what we did */
        struct stat st;
        if (lstat(path, &st) == 0) {
            fprintf(stdout, "changed ownership of '%s' to %d:%d\n",
                    path, (int)st.st_uid, (int)st.st_gid);
        }
    }
}

/* ------------------------------------------------------------------ recurse */

static void process(const char *path, uid_t uid, gid_t gid);

static void recurse(const char *path, uid_t uid, gid_t gid) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "chown: cannot open directory '%s': %s\n",
                path, strerror(errno));
        errors = 1;
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) continue;

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        process(child, uid, gid);
    }
    closedir(d);
}

static void process(const char *path, uid_t uid, gid_t gid) {
    apply(path, uid, gid);

    if (flag_recursive) {
        struct stat st;
        /* Use lstat so we don't follow symlinks when checking type */
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
            recurse(path, uid, gid);
    }
}

/* ------------------------------------------------------------------ main */

static void usage(void) {
    fprintf(stderr,
        "Usage: chown [OPTION]... [OWNER][:[GROUP]] FILE...\n"
        "       chown [OPTION]... --reference=RFILE FILE...\n"
        "\n"
        "Options:\n"
        "  -R, --recursive       operate on files and directories recursively\n"
        "  -h, --no-dereference  affect symbolic links instead of referenced files\n"
        "  -v, --verbose         output a diagnostic for every file processed\n"
        "  -c, --changes         like verbose but report only when a change is made\n"
        "  --reference=RFILE     use RFILE owner and group instead of specifying values\n"
        "  --help                display this help\n"
    );
    exit(1);
}

int main(int argc, char *argv[]) {
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;
    int   use_reference = 0;

    if (argc < 2) usage();

    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-') break;  /* end of options */

        if (strcmp(argv[i], "--") == 0) { i++; break; }

        if (strcmp(argv[i], "-R") == 0 ||
            strcmp(argv[i], "--recursive") == 0) {
            flag_recursive = 1;

        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--no-dereference") == 0) {
            flag_symlink = 1;

        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--verbose") == 0) {
            flag_verbose = 1;

        } else if (strcmp(argv[i], "-c") == 0 ||
                   strcmp(argv[i], "--changes") == 0) {
            flag_changes = 1;
            flag_verbose = 1;

        } else if (strncmp(argv[i], "--reference=", 12) == 0) {
            const char *ref = argv[i] + 12;
            struct stat st;
            if (stat(ref, &st) < 0) {
                fprintf(stderr, "chown: failed to get attributes of '%s': %s\n",
                        ref, strerror(errno));
                return 1;
            }
            uid = st.st_uid;
            gid = st.st_gid;
            use_reference = 1;

        } else if (strcmp(argv[i], "--help") == 0) {
            usage();

        } else {
            /* Handle combined short flags like -Rv, -Rh etc */
            const char *p = argv[i] + 1;
            while (*p) {
                switch (*p) {
                case 'R': flag_recursive = 1; break;
                case 'h': flag_symlink   = 1; break;
                case 'v': flag_verbose   = 1; break;
                case 'c': flag_changes   = 1; flag_verbose = 1; break;
                default:
                    fprintf(stderr, "chown: invalid option -- '%c'\n", *p);
                    usage();
                }
                p++;
            }
        }
    }

    /* Next arg is owner spec (unless --reference was used) */
    if (!use_reference) {
        if (i >= argc) usage();
        parse_owner(argv[i++], &uid, &gid);
    }

    if (i >= argc) {
        fprintf(stderr, "chown: missing file operand\n");
        return 1;
    }

    /* Apply to each file */
    for (; i < argc; i++)
        process(argv[i], uid, gid);

    return errors ? 1 : 0;
}
