/* ipc_test.h - khung test toi gian, khong phu thuoc thu vien ngoai. */
#ifndef IPC_TEST_H
#define IPC_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_tests_run, g_tests_failed, g_checks_failed;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_checks_failed++;                                                 \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        long _a = (long)(a), _b = (long)(b);                                   \
        if (_a != _b) {                                                        \
            printf("    FAIL %s:%d: %s = %ld, mong doi %ld\n", __FILE__,       \
                   __LINE__, #a, _a, _b);                                      \
            g_checks_failed++;                                                 \
        }                                                                      \
    } while (0)

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        int before = g_checks_failed;                                          \
        g_tests_run++;                                                         \
        printf("  %-42s", #fn);                                                \
        fflush(stdout);                                                        \
        fn();                                                                  \
        if (g_checks_failed != before) {                                       \
            g_tests_failed++;                                                  \
            printf("  <== FAIL\n");                                            \
        } else {                                                               \
            printf("ok\n");                                                    \
        }                                                                      \
    } while (0)

void run_timer_tests(void);
void run_watchdog_tests(void);
void run_config_tests(void);
void run_health_tests(void);

#endif /* IPC_TEST_H */
