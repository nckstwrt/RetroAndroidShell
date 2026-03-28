#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define PROP_VALUE_MAX 92

int resetprop(const char *name, const char *value) {
    char maps_line[512];
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) { perror("open maps"); return -1; }

    while (fgets(maps_line, sizeof(maps_line), maps)) {
        if (!strstr(maps_line, "__properties__")) continue;

        uintptr_t start, end;
        char perms[8], filepath[256];
        filepath[0] = 0;
        sscanf(maps_line, "%lx-%lx %s %*s %*s %*s %s", &start, &end, perms, filepath);
        if (!filepath[0]) continue;

        uint8_t *area = (uint8_t *)start;
        size_t size = end - start;

        for (size_t i = 0; i < size - PROP_VALUE_MAX; i++) {
            if (strcmp((char *)area + i, name) == 0) {
                printf("Found '%s' in %s at offset 0x%zx\n", name, filepath, i);

                if (i < 96) continue;
                char *val_ptr = (char *)area + i - 96 + 4;
                printf("Current value: '%s'\n", val_ptr);

                // Write directly to the file instead of mprotect
                int fd = open(filepath, O_RDWR);
                if (fd < 0) { perror("open prop file"); continue; }

                off_t val_offset = (off_t)((uintptr_t)val_ptr - start);
                if (lseek(fd, val_offset, SEEK_SET) < 0) {
                    perror("lseek");
                    close(fd);
                    continue;
                }

                char buf[PROP_VALUE_MAX] = {0};
                strncpy(buf, value, PROP_VALUE_MAX - 1);
                if (write(fd, buf, PROP_VALUE_MAX) < 0) {
                    perror("write");
                    close(fd);
                    continue;
                }

                // Increment serial
                off_t serial_offset = (off_t)((uintptr_t)(val_ptr - 4) - start);
                lseek(fd, serial_offset, SEEK_SET);
                uint32_t serial;
                read(fd, &serial, 4);
                serial++;
                lseek(fd, serial_offset, SEEK_SET);
                write(fd, &serial, 4);

                close(fd);
                printf("Value updated to '%s'\n", value);
                fclose(maps);
                return 0;
            }
        }
    }
    fclose(maps);
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