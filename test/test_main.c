#include "ipc_test.h"

int g_tests_run, g_tests_failed, g_checks_failed;

int main(void)
{
    printf("== lightTirex host tests ==\n");
    run_timer_tests();
    run_watchdog_tests();
    run_config_tests();
    run_health_tests();
    run_bus_tests();
    run_system_tests();

    printf("\n%d test, %d that bai (%d check hong)\n",
           g_tests_run, g_tests_failed, g_checks_failed);
    return g_tests_failed ? 1 : 0;
}
