#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>

#define PROP_VALUE_MAX   92
#define PROP_DIR         "/dev/__properties__"

int resetprop(const char *name, const char *value) {
    DIR *dir = opendir(PROP_DIR);
    if (!dir) { perror("opendir " PROP_DIR); return -1; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        // skip the non-context files
        if (strcmp(ent->d_name, "properties_serial") == 0) continue;
        if (strcmp(ent->d_name, "property_info")     == 0) continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", PROP_DIR, ent->d_name);

        // Open R/W straight away — we need it for writing later anyway
        int fd = open(filepath, O_RDWR);
        if (fd < 0) {
            // might be permission-denied on some contexts even as root; skip
            continue;
        }

        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size == 0) { close(fd); continue; }
        size_t size = (size_t)st.st_size;

        // Map the region so we can scan it
        uint8_t *area = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (area == MAP_FAILED) { close(fd); continue; }

        printf("Scanning: %s\n", filepath);

        for (size_t i = 0; i + PROP_VALUE_MAX < size; i++) {
            if (strcmp((char *)area + i, name) != 0) continue;

            printf("Found '%s' in %s at offset 0x%zx\n", name, filepath, i);
            if (i < 96) { continue; }

            // Value sits at: node_base + 4  (serial u32), then 4 bytes len, then value
            // The name offset relative to its prop_bt node start:
            //   node_base = i - 96  (name is at +96 within prop_info / trie leaf)
            // Actually layout is: [serial u32][value_len u32? no — legacy]
            // Modern prop_info (Android 8+) layout relative to the found name:
            //   name is at offset +8 within prop_info: serial(4) + value(92) + name...
            //   so:  prop_info_base = (name offset) - 8 - 92  ... wait:
            //
            // Actual prop_info struct layout:
            //   uint32_t serial;          // +0
            //   char     value[92];       // +4
            //   char     name[];          // +96  (variable length, null terminated)
            //
            // So: prop_info_base = i - 96
            //     serial  @ prop_info_base + 0
            //     value   @ prop_info_base + 4
            char *prop_base = (char *)area + i - 96;
            char *val_ptr   = prop_base + 4;

            printf("Current value: '%s'\n", val_ptr);

            // Write new value
            off_t val_offset = (off_t)(val_ptr - (char *)area);
            if (lseek(fd, val_offset, SEEK_SET) < 0) {
                perror("lseek value"); goto next;
            }
            {
                char buf[PROP_VALUE_MAX] = {0};
                strncpy(buf, value, PROP_VALUE_MAX - 1);
                if (write(fd, buf, PROP_VALUE_MAX) != PROP_VALUE_MAX) {
                    perror("write value"); goto next;
                }
            }

            // Increment serial so readers see the update
            {
                off_t serial_offset = (off_t)(prop_base - (char *)area);
                uint32_t serial;
                lseek(fd, serial_offset, SEEK_SET);
                if (read(fd, &serial, 4) == 4) {
                    serial++;
                    lseek(fd, serial_offset, SEEK_SET);
                    write(fd, &serial, 4);
                }
            }

            munmap(area, size);
            close(fd);
            closedir(dir);
            printf("Value updated to '%s'\n", value);
            return 0;

        next:
            break; // try next file on error
        }

        munmap(area, size);
        close(fd);
    }

    closedir(dir);
    fprintf(stderr, "Property '%s' not found\n", name);
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <prop_name> <value>\n", argv[0]);
        return 1;
    }
    if (getuid() != 0) {
        fprintf(stderr, "Must run as root\n");
        return 1;
    }
    return resetprop(argv[1], argv[2]);
}