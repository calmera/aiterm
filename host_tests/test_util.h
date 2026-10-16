// Minimal assertion helpers shared by the host tests.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_tests = 0, g_failed = 0;
#define CHECK(cond) do { g_tests++; if (!(cond)) { g_failed++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { g_tests++; const char* _a = (a); const char* _b = (b); if (!_a || !_b || strcmp(_a, _b) != 0) { g_failed++; fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)
#define CHECK_INT(a, b) do { g_tests++; long _a = (long)(a); long _b = (long)(b); if (_a != _b) { g_failed++; fprintf(stderr, "  FAIL %s:%d: %ld != %ld\n", __FILE__, __LINE__, _a, _b); } } while (0)
#define TEST_MAIN_END(name) do { printf("%s: %d checks, %d failed\n", name, g_tests, g_failed); return g_failed ? 1 : 0; } while (0)
