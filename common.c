/* common.c -- see common.h */
#include "common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

void sample_list_init(SampleList *list) {
    list->data = NULL;
    list->len = 0;
    list->cap = 0;
}

void sample_list_free(SampleList *list) {
    free(list->data);
    list->data = NULL;
    list->len = 0;
    list->cap = 0;
}

static void sample_list_ensure_cap(SampleList *list, size_t min_cap) {
    if (list->cap >= min_cap) return;
    size_t new_cap = list->cap ? list->cap * 2 : 4096;
    while (new_cap < min_cap) new_cap *= 2;
    list->data = realloc(list->data, new_cap * sizeof(Sample));
    list->cap = new_cap;
}

void sample_list_append(SampleList *list, Sample s) {
    sample_list_ensure_cap(list, list->len + 1);
    list->data[list->len++] = s;
}

void sample_list_extend(SampleList *list, const Sample *src, size_t n) {
    if (n == 0) return;
    sample_list_ensure_cap(list, list->len + n);
    memcpy(list->data + list->len, src, n * sizeof(Sample));
    list->len += n;
}

int jeu_mkdir_p(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

char *jeu_cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    char base[4096];
    if (xdg && xdg[0]) {
        snprintf(base, sizeof(base), "%s", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return NULL;
        snprintf(base, sizeof(base), "%s/.cache", home);
    }
    char *full = malloc(strlen(base) + strlen("/jetson-energy-usage") + 1);
    if (!full) return NULL;
    sprintf(full, "%s/jetson-energy-usage", base);
    if (jeu_mkdir_p(full) != 0) {
        free(full);
        return NULL;
    }
    return full;
}
