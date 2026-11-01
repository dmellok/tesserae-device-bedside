/*
 * test.h: a minimal test harness for the host tests. Each test file has a
 * main() that calls RUN(fn) for its tests and returns test_summary().
 */
#pragma once

#include <stdio.h>
#include <string.h>
#include <math.h>

static int t_fail, t_checks, t_tests, t_cur_failed;
static const char *t_name;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_fail++; t_cur_failed = 1; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, t_name, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); t_checks++; if (_a != _b) { t_fail++; t_cur_failed = 1; \
    printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, t_name, #a, #b, _a, _b); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); t_checks++; if (fabs(_a - _b) > (eps)) { t_fail++; t_cur_failed = 1; \
    printf("  FAIL %s:%d in %s: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, t_name, #a, #b, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { t_checks++; if (strcmp((a), (b))) { t_fail++; t_cur_failed = 1; \
    printf("  FAIL %s:%d in %s: \"%s\" == \"%s\"\n", __FILE__, __LINE__, t_name, (a), (b)); } } while (0)

#define RUN(fn) do { t_name = #fn; t_cur_failed = 0; t_tests++; fn(); printf("%s %s\n", t_cur_failed ? "not ok" : "ok    ", #fn); } while (0)

static int test_summary(const char *suite)
{
    printf("%s: %d tests, %d checks, %d failed\n", suite, t_tests, t_checks, t_fail);
    return t_fail ? 1 : 0;
}
