#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "ioctlops.h"

struct command {
    const char    *name;
    unsigned long  ioc;
    int            print_ok;
};

static const struct command commands[] = {
    { "protect",    OBSIDIAN_IOC_PROTECT,     1 },
    { "listdentry", OBSIDIAN_IOC_DENTRY_TEST, 0 },
    { "rollback",   OBSIDIAN_IOC_REVERT,      1 },
    { "forward",    OBSIDIAN_IOC_FORWARD,     1 },
};

struct stats {
    unsigned long ok;
    unsigned long failed;
};

static void apply_to_file(const char *path, const struct command *cmd, struct stats *st)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        fprintf(stderr, "Error open(%s): %s\n", path, strerror(errno));
        st->failed++;
        return;
    }

    if (ioctl(fd, cmd->ioc) < 0) {
        fprintf(stderr, "Error ioctl %s on %s: %s\n", cmd->name, path, strerror(errno));
        st->failed++;
        close(fd);
        return;
    }

    if (cmd->print_ok)
        printf("%s: %s\n", cmd->name, path);
    st->ok++;
    close(fd);
}

static void apply_recursive(const char *dirpath, const struct command *cmd, struct stats *st)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "Error opendir(%s): %s\n", dirpath, strerror(errno));
        st->failed++;
        return;
    }

    char        **names = NULL;
    size_t        count = 0, cap = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        if (count == cap) {
            size_t newcap = cap ? cap * 2 : 16;
            char **tmp = realloc(names, newcap * sizeof(*names));
            if (!tmp) {
                fprintf(stderr, "Out of memory reading %s\n", dirpath);
                st->failed++;
                break;
            }
            names = tmp;
            cap = newcap;
        }

        names[count] = strdup(de->d_name);
        if (!names[count]) {
            fprintf(stderr, "Out of memory reading %s\n", dirpath);
            st->failed++;
            break;
        }
        count++;
    }
    closedir(dir);

    for (size_t i = 0; i < count; i++) {
        char child[PATH_MAX];
        int  n = snprintf(child, sizeof(child), "%s/%s", dirpath, names[i]);

        if (n < 0 || (size_t)n >= sizeof(child)) {
            fprintf(stderr, "Path too long: %s/%s\n", dirpath, names[i]);
            st->failed++;
            free(names[i]);
            continue;
        }
        free(names[i]);

        struct stat sb;
        if (lstat(child, &sb) < 0) {
            fprintf(stderr, "Error lstat(%s): %s\n", child, strerror(errno));
            st->failed++;
            continue;
        }

        if (S_ISDIR(sb.st_mode))
            apply_recursive(child, cmd, st);
        else if (S_ISREG(sb.st_mode))
            apply_to_file(child, cmd, st);
    }
    free(names);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <protect|listdentry|rollback|forward> <file|dir>\n", argv[0]);
        fprintf(stderr, "  If <dir> is a directory, the command is applied recursively\n"
                        "  to every regular file it contains.\n");
        return 1;
    }

    const struct command *cmd = NULL;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(argv[1], commands[i].name) == 0) {
            cmd = &commands[i];
            break;
        }
    }
    if (!cmd) {
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        return 1;
    }

    struct stat sb;
    if (lstat(argv[2], &sb) < 0) {
        fprintf(stderr, "Error lstat(%s): %s\n", argv[2], strerror(errno));
        return 1;
    }

    struct stats st = { 0, 0 };

    if (S_ISDIR(sb.st_mode)) {
        apply_recursive(argv[2], cmd, &st);
        printf("%s: %lu file(s) done, %lu failure(s)\n", cmd->name, st.ok, st.failed);
    } else if (S_ISREG(sb.st_mode)) {
        apply_to_file(argv[2], cmd, &st);
    } else {
        fprintf(stderr, "%s is neither a regular file nor a directory\n", argv[2]);
        return 1;
    }

    return st.failed ? 1 : 0;
}
