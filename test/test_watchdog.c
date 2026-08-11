/*
 * test_watchdog.c - test watchdog voi backend gia lap.
 *
 * Khong reset may that: backend fake chi dem so lan "da dinh reset". Do la
 * loi ich cua viec tach backend ra sau interface (DIP + LSP).
 */
#include "ipc_test.h"

#include "ipc_clock.h"
#include "ipc_looper.h"
#include "ipc_watchdog.h"

static ipc_fake_clock_t      g_clock;
static ipc_wdt_fake_backend_t g_be;

static int  g_recovery_calls;
static bool g_recovery_result;
static char g_last_event[32];
static ipc_wdt_policy_t g_last_action;

static bool recovery_fn(const char *client, ipc_looper_t *lp, void *user)
{
    (void)lp; (void)user; (void)client;
    g_recovery_calls++;
    return g_recovery_result;
}

static void event_fn(const char *client, uint32_t overdue,
                     ipc_wdt_policy_t action, bool recovered, void *user)
{
    (void)overdue; (void)recovered; (void)user;
    snprintf(g_last_event, sizeof(g_last_event), "%s", client);
    g_last_action = action;
}

/* Moi test bat dau tu trang thai sach. */
static void fresh_wdt(uint32_t hw_timeout_ms, uint32_t max_attempts)
{
    ipc_wdt_fake_backend_init(&g_be);
    g_recovery_calls = 0;
    g_recovery_result = true;
    g_last_event[0] = 0;
    g_last_action = IPC_WDT_POLICY_LOG;

    ipc_wdt_cfg_t c;
    ipc_wdt_cfg_default(&c);
    c.backend = &g_be.base;
    c.hw_timeout_ms = hw_timeout_ms;
    c.own_task = false;             /* test tu goi ipc_wdt_check() */
    c.max_recovery_attempts = max_attempts;
    c.recovery = recovery_fn;
    c.on_event = event_fn;
    ipc_wdt_start(&c);
}

/* ------------------------------------------------------------------ */

static void test_kick_deu_thi_cho_hw_an(void)
{
    fresh_wdt(8000, 3);
    ipc_wdt_handle_t h = ipc_wdt_register("pump", 1000, IPC_WDT_POLICY_LOG);
    CHECK(h != NULL);

    for (int i = 0; i < 5; ++i) {
        ipc_fake_clock_advance(&g_clock, 500);
        ipc_wdt_kick(h);
        CHECK_EQ(ipc_wdt_check(), 0);
    }
    CHECK_EQ(g_be.feed_count, 5);   /* khoe -> HW duoc cho an moi vong */
    CHECK_EQ(g_be.reset_count, 0);
    ipc_wdt_unregister(h);
}

static void test_khong_kick_thi_qua_han_va_ngung_cho_an(void)
{
    fresh_wdt(8000, 3);
    ipc_wdt_handle_t h = ipc_wdt_register("pump", 1000, IPC_WDT_POLICY_LOG);

    ipc_fake_clock_advance(&g_clock, 900);
    CHECK_EQ(ipc_wdt_check(), 0);   /* chua qua han */
    uint32_t feeds = g_be.feed_count;

    ipc_fake_clock_advance(&g_clock, 200);
    CHECK_EQ(ipc_wdt_check(), 1);   /* 1100ms > 1000ms */
    CHECK_EQ(g_be.feed_count, feeds); /* co nguoi om -> KHONG cho HW an nua */
    CHECK_EQ(strcmp(g_last_event, "pump"), 0);

    /* Kick lai thi khoe lai ngay. */
    ipc_wdt_kick(h);
    CHECK_EQ(ipc_wdt_check(), 0);
    CHECK_EQ(g_be.feed_count, feeds + 1);
    ipc_wdt_unregister(h);
}

static void test_policy_restart_goi_ham_phuc_hoi(void)
{
    fresh_wdt(8000, 3);
    g_recovery_result = true;
    ipc_wdt_handle_t h = ipc_wdt_register("svc", 1000, IPC_WDT_POLICY_RESTART);

    ipc_fake_clock_advance(&g_clock, 1500);
    ipc_wdt_check();
    CHECK_EQ(g_recovery_calls, 1);
    CHECK_EQ(g_last_action, IPC_WDT_POLICY_RESTART);
    CHECK_EQ(g_be.reset_count, 0);

    /* Phuc hoi thanh cong -> dong ho cho lai tu dau, khong bao lai ngay. */
    CHECK_EQ(ipc_wdt_check(), 0);
    ipc_wdt_unregister(h);
}

static void test_phuc_hoi_that_bai_lien_tuc_thi_reset_he_thong(void)
{
    fresh_wdt(8000, 3);
    g_recovery_result = false;      /* phuc hoi luon that bai */
    ipc_wdt_handle_t h = ipc_wdt_register("svc", 1000, IPC_WDT_POLICY_RESTART);

    ipc_fake_clock_advance(&g_clock, 1500);
    ipc_wdt_check();                /* lan 1 */
    CHECK_EQ(g_be.reset_count, 0);
    ipc_wdt_check();                /* lan 2 */
    CHECK_EQ(g_be.reset_count, 0);
    ipc_wdt_check();                /* lan 3 -> cham tran -> reset */
    CHECK_EQ(g_recovery_calls, 3);
    CHECK_EQ(g_be.reset_count, 1);
    CHECK_EQ(g_last_action, IPC_WDT_POLICY_RESET);
    ipc_wdt_unregister(h);
}

static void test_suspend_khong_bao_oan(void)
{
    fresh_wdt(8000, 3);
    ipc_wdt_handle_t h = ipc_wdt_register("flash", 1000, IPC_WDT_POLICY_LOG);

    /* Sap ghi flash/OTA: tam ngung giam sat. */
    ipc_wdt_suspend(h);
    ipc_fake_clock_advance(&g_clock, 10000);
    CHECK_EQ(ipc_wdt_check(), 0);

    /* Resume phai cho lai tu dau, khong phat ngay lap tuc. */
    ipc_wdt_resume(h);
    CHECK_EQ(ipc_wdt_check(), 0);

    ipc_fake_clock_advance(&g_clock, 1500);
    CHECK_EQ(ipc_wdt_check(), 1);
    ipc_wdt_unregister(h);
}

/* Looper khong phai tu kick: watchdog doc nhip tim cua no. */
static bool noop_cb(ipc_handler_t *h, ipc_message_t *m, void *u)
{ (void)h; (void)m; (void)u; return true; }

static void test_giam_sat_looper_qua_nhip_tim(void)
{
    fresh_wdt(8000, 3);

    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, "wsvc");
    lc.heartbeat_timeout_ms = 1000;
    ipc_looper_t *lp = ipc_looper_create(&lc);
    ipc_handler_t h;
    ipc_handler_init(&h, lp, noop_cb, NULL, "wsvc");

    /* timeout=0 -> lay 2x nguong cua looper = 2000ms (watchdog la luoi cuoi,
     * rong hon supervisor de khong gianh viec cua nhau). */
    ipc_wdt_handle_t wh = ipc_wdt_watch_looper(lp, 0, IPC_WDT_POLICY_LOG);
    CHECK(wh != NULL);

    ipc_looper_poll(lp, 0);         /* looper chay -> nhip tim moi */
    ipc_fake_clock_advance(&g_clock, 1500);
    CHECK_EQ(ipc_wdt_check(), 0);   /* 1500 < 2000: van coi la khoe */

    ipc_fake_clock_advance(&g_clock, 1000);
    CHECK_EQ(ipc_wdt_check(), 1);   /* 2500 > 2000: looper im lang qua lau */

    ipc_looper_poll(lp, 0);         /* looper song lai */
    CHECK_EQ(ipc_wdt_check(), 0);

    ipc_wdt_unregister(wh);
    ipc_looper_stop_permanently(lp);
    ipc_looper_destroy(lp);
}

void run_watchdog_tests(void)
{
    ipc_fake_clock_init(&g_clock, 1000);
    ipc_clock_set(&g_clock.base);

    printf("watchdog:\n");
    RUN_TEST(test_kick_deu_thi_cho_hw_an);
    RUN_TEST(test_khong_kick_thi_qua_han_va_ngung_cho_an);
    RUN_TEST(test_policy_restart_goi_ham_phuc_hoi);
    RUN_TEST(test_phuc_hoi_that_bai_lien_tuc_thi_reset_he_thong);
    RUN_TEST(test_suspend_khong_bao_oan);
    RUN_TEST(test_giam_sat_looper_qua_nhip_tim);

    ipc_clock_set(NULL);
}
