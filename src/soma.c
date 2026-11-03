/*
 * soma.c: see soma.h.
 */
#include "soma.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static void copy_trim(char *dst, size_t cap, const char *s)
{
    if (!s) { dst[0] = 0; return; }
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    if (n >= cap) { n = cap - 1; if (n > 3) { memcpy(dst, s, n - 3); memcpy(dst + n - 3, "...", 3); dst[n] = 0; return; } }
    memcpy(dst, s, n); dst[n] = 0;
}

int soma_parse(const char *json, soma_channel_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "channels");
    int n = 0;
    if (cJSON_IsArray(list)) {
        const cJSON *c;
        cJSON_ArrayForEach(c, list) {
            if (n >= max) break;
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(c, "id"), *title = cJSON_GetObjectItemCaseSensitive(c, "title");
            if (!cJSON_IsString(id) || !id->valuestring[0] || !cJSON_IsString(title)) continue;
            copy_trim(out[n].id, sizeof out[n].id, id->valuestring);
            copy_trim(out[n].title, sizeof out[n].title, title->valuestring);
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(c, "description");
            copy_trim(out[n].desc, sizeof out[n].desc, cJSON_IsString(d) ? d->valuestring : "");
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

void soma_url(const char *id, char *url, size_t cap)
{
    snprintf(url, cap, "http://ice1.somafm.com/%s-128-mp3", id);
}
