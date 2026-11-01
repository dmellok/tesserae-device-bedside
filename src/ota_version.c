/*
 * ota_version.c: release version ordering (no Pico dependencies, so tests/
 * can check it on the host). See ota.h.
 */
#include <stdio.h>
#include "ota.h"

int ota_version_cmp(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    if (a[0] == 'v') a++;                 /* tags are "v1.2.3" */
    if (b[0] == 'v') b++;
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]); sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
