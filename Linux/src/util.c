#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

uint32_t bit_slice(uint32_t val, int hi, int lo)
{
    int width = hi - lo + 1;
    if (width <= 0 || width > 32) return 0;
    uint32_t mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    return (val >> lo) & mask;
}

int read_file_string(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)sz, f)) { fclose(f); buf[0] = '\0'; return 0; }
    fclose(f);
    /* strip trailing whitespace */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
        buf[--len] = '\0';
    return 1;
}

int read_file_bytes(const char *path, uint8_t *buf, size_t sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, sz, f);
    fclose(f);
    return n;
}

float read_float_file(const char *path)
{
    char buf[64];
    if (!read_file_string(path, buf, sizeof(buf))) return 0.0f;
    return strtof(buf, NULL);
}

int read_int_file(const char *path)
{
    char buf[64];
    if (!read_file_string(path, buf, sizeof(buf))) return 0;
    return (int)strtol(buf, NULL, 10);
}

char *run_command(const char *cmd)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
