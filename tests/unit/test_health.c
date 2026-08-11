/*
 * test_health.c - test health service.
 *
 * Kiem tra duoc "het heap thi lam gi" ma khong can het heap that, va
 * "khi nao thi reboot" ma khong reboot may - nho probe gia va backend gia.
 */
#include "ipc_test.h"

#include "ipc_clock.h"
#include "ipc_health.h"
#include "ipc_looper.h"
#include "ipc_service.h"

static ipc_fake_clock_t       g_clock;
static ipc_health_fake_probe_t g_probe;
static ipc_wdt_fake_backend_t  g_be;

static int  g_recovery_calls;
static bool g_recovery_result;
static int  g_safe_mode_calls;
static ipc_health_action_t g_last_action;
static char g_last_source[32];
static uint32_t g_last_code;

static bool recovery_fn(const char *svc, ipc_looper_t *lp, void *user)
{
    (void)svc; (void)lp; (void)user;
    g_recovery_calls++;
    return g_recovery_result;
}

static void safe_mode_fn(void *user) { (void)user; g_safe_mode_calls++; }

static void event_fn(const char *source, uint32_t code, ipc_severity_t sev,
                     ipc_health_action_t act, int32_t detail, void *user)
{
    (void)sev; (void)detail; (void)user;
    g_last_action = act;
    g_last_code = code;
    snprintf(g_last_source, sizeof(g_last_source), "%s", source);
}

static void fresh(void)
{
    g_recovery_calls = 0;
    g_recovery_result = true;
    g_safe_mode_calls = 0;
    g_last_action = IPC_ACT_NONE;
    g_last_source[0] = 0;
    g_last_code = 0;

    ipc_health_fake_probe_init(&g_probe, 200000);
    ipc_wdt_fake_backend_init(&g_be);
    ipc_health_clear_rules();

    ipc_health_cfg_t c;
    ipc_health_cfg_default(&c);
    c.probe = &g_probe.base;
    c.backend = &g_be.base;
    c.recovery = recovery_fn;
    c.on_event = event_fn;
    c.on_safe_mode = safe_mode_fn;
    c.own_task = false;              /* test tu goi ipc_health_check() */
    c.queue_depth_warn = 0;          /* tat quet hang doi cho tung test rieng */
    c.pool_free_warn = 0;
    ipc_health_start(&c);

    ipc_health_check();              /* rut sach vong dem con sot */
}

/* ------------------------------------------------------------------ */

static void test_khong_co_luat_thi_khong_lam_gi(void)
{
    fresh();
    ipc_health_report("sensor", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 5);
    CHECK_EQ(ipc_health_check(), 0);      /* khong luat -> khong hanh dong */
    CHECK_EQ(g_last_action, IPC_ACT_NONE);

    ipc_health_status_t st;
    ipc_health_get_status(&st);
    CHECK_EQ(st.exceptions[IPC_SEV_ERROR], 1);  /* nhung van duoc dem */
}

static void test_luat_don_gian_ban_ngay(void)
{
    fresh();
    ipc_health_rule_t r = { .code = IPC_EXC_HW_FAULT, .source = NULL,
                            .min_severity = IPC_SEV_ERROR, .count = 1,
                            .action = IPC_ACT_RESTART_SERVICE };
    CHECK(ipc_health_add_rule(&r));

    ipc_health_report("i2c", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 0);
    CHECK_EQ(ipc_health_check(), 1);
    CHECK_EQ(g_recovery_calls, 1);
    CHECK_EQ(g_last_action, IPC_ACT_RESTART_SERVICE);
    CHECK_EQ(strcmp(g_last_source, "i2c"), 0);
}

static void test_muc_do_thap_hon_thi_khong_kich_hoat(void)
{
    fresh();
    ipc_health_rule_t r = { .code = IPC_EXC_HW_FAULT, .min_severity = IPC_SEV_ERROR,
                            .count = 1, .action = IPC_ACT_REBOOT };
    ipc_health_add_rule(&r);

    ipc_health_report("i2c", IPC_EXC_HW_FAULT, IPC_SEV_WARN, 0);
    CHECK_EQ(ipc_health_check(), 0);
    CHECK_EQ(g_be.reset_count, 0);        /* WARN < ERROR: khong reboot */
}

static void test_nguong_trong_cua_so_thoi_gian(void)
{
    fresh();
    /* 3 lan loi trong 1 giay moi coi la hong that; loi le te thi bo qua. */
    ipc_health_rule_t r = { .code = IPC_EXC_TIMEOUT, .min_severity = IPC_SEV_WARN,
                            .count = 3, .window_ms = 1000,
                            .action = IPC_ACT_RESTART_SERVICE };
    ipc_health_add_rule(&r);

    ipc_health_report("uart", IPC_EXC_TIMEOUT, IPC_SEV_WARN, 0);
    ipc_health_report("uart", IPC_EXC_TIMEOUT, IPC_SEV_WARN, 0);
    CHECK_EQ(ipc_health_check(), 0);
    CHECK_EQ(g_recovery_calls, 0);

    ipc_health_report("uart", IPC_EXC_TIMEOUT, IPC_SEV_WARN, 0);
    CHECK_EQ(ipc_health_check(), 1);
    CHECK_EQ(g_recovery_calls, 1);
}

static void test_loi_thua_thot_khong_kich_hoat(void)
{
    fresh();
    ipc_health_rule_t r = { .code = IPC_EXC_TIMEOUT, .min_severity = IPC_SEV_WARN,
                            .count = 3, .window_ms = 1000,
                            .action = IPC_ACT_RESTART_SERVICE };
    ipc_health_add_rule(&r);

    /* 5 loi nhung cach nhau 2 giay -> khong bao gio du 3 trong 1 cua so. */
    for (int i = 0; i < 5; ++i) {
        ipc_health_report("uart", IPC_EXC_TIMEOUT, IPC_SEV_WARN, 0);
        ipc_health_check();
        ipc_fake_clock_advance(&g_clock, 2000);
    }
    CHECK_EQ(g_recovery_calls, 0);
}

static void test_luat_hep_thang_luat_rong(void)
{
    fresh();
    /* Dang ky luat hep truoc: chi rieng "gps" thi bo qua, con lai thi reboot. */
    ipc_health_rule_t narrow = { .code = IPC_EXC_ANY, .source = "gps",
                                 .min_severity = IPC_SEV_ERROR, .count = 1,
                                 .action = IPC_ACT_LOG };
    ipc_health_rule_t wide = { .code = IPC_EXC_ANY, .source = NULL,
                               .min_severity = IPC_SEV_ERROR, .count = 1,
                               .action = IPC_ACT_REBOOT };
    ipc_health_add_rule(&narrow);
    ipc_health_add_rule(&wide);

    ipc_health_report("gps", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 0);
    ipc_health_check();
    CHECK_EQ(g_be.reset_count, 0);        /* gps duoc mien */
    CHECK_EQ(g_last_action, IPC_ACT_LOG);

    ipc_health_report("modem", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 0);
    ipc_health_check();
    CHECK_EQ(g_be.reset_count, 1);        /* nguon khac thi khong */
}

/*
 * Luat dau tien khop la nguoi QUYET DINH, ke ca khi no quyet dinh "chua du
 * nguong, khoan da". Luat rong phia sau khong duoc nhay vao lam thay.
 * Neu khong, mot loi le te cua uart se keo theo reboot ca board.
 */
static void test_luat_hep_chua_du_nguong_van_chan_luat_rong(void)
{
    fresh();
    ipc_health_rule_t narrow = { .code = IPC_EXC_ANY, .source = "uart",
                                 .min_severity = IPC_SEV_ERROR, .count = 3,
                                 .window_ms = 1000,
                                 .action = IPC_ACT_RESTART_SERVICE };
    ipc_health_rule_t wide = { .code = IPC_EXC_ANY, .source = NULL,
                               .min_severity = IPC_SEV_ERROR, .count = 1,
                               .action = IPC_ACT_REBOOT };
    ipc_health_add_rule(&narrow);
    ipc_health_add_rule(&wide);

    ipc_health_report("uart", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 0);
    CHECK_EQ(ipc_health_check(), 0);
    CHECK_EQ(g_be.reset_count, 0);      /* mot loi uart KHONG duoc lam reboot */
    CHECK_EQ(g_recovery_calls, 0);

    /* Du 3 lan thi luat hep moi ra tay - va van la luat hep, khong phai reboot. */
    ipc_health_report("uart", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 0);
    ipc_health_report("uart", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, 0);
    CHECK_EQ(ipc_health_check(), 1);
    CHECK_EQ(g_recovery_calls, 1);
    CHECK_EQ(g_be.reset_count, 0);
}

static void test_fatal_thi_reboot(void)
{
    fresh();
    ipc_health_rule_t r = { .code = IPC_EXC_ANY, .min_severity = IPC_SEV_FATAL,
                            .count = 1, .action = IPC_ACT_REBOOT };
    ipc_health_add_rule(&r);

    ipc_health_report("core", IPC_EXC_ASSERT, IPC_SEV_FATAL, -1);
    ipc_health_check();
    CHECK_EQ(g_be.reset_count, 1);

    ipc_health_status_t st;
    ipc_health_get_status(&st);
    CHECK_EQ(st.reboots, 1);
}

static void test_het_heap_sinh_exception(void)
{
    fresh();
    ipc_health_cfg_t c;
    ipc_health_cfg_default(&c);
    c.probe = &g_probe.base;
    c.backend = &g_be.base;
    c.on_event = event_fn;
    c.recovery = recovery_fn;
    c.on_safe_mode = safe_mode_fn;
    c.own_task = false;
    c.heap_warn_bytes = 50000;
    c.heap_critical_bytes = 10000;
    c.queue_depth_warn = 0;
    c.pool_free_warn = 0;
    ipc_health_start(&c);
    ipc_health_clear_rules();

    ipc_health_rule_t r = { .code = IPC_EXC_LOW_HEAP, .min_severity = IPC_SEV_ERROR,
                            .count = 1, .action = IPC_ACT_SAFE_MODE };
    ipc_health_add_rule(&r);

    g_probe.heap = 200000;
    CHECK_EQ(ipc_health_check(), 0);       /* du heap: im lang */

    g_probe.heap = 30000;                  /* duoi nguong canh bao */
    CHECK_EQ(ipc_health_check(), 0);       /* WARN thoi, chua den ERROR */
    CHECK_EQ(g_safe_mode_calls, 0);

    g_probe.heap = 5000;                   /* duoi nguong nguy hiem */
    CHECK_EQ(ipc_health_check(), 1);
    CHECK_EQ(g_safe_mode_calls, 1);
    CHECK_EQ(g_last_code, (uint32_t)IPC_EXC_LOW_HEAP);
}

/* Giet han thread: dich vu bi go khoi ServiceManager va looper dung vinh vien
 * de supervisor KHONG hoi sinh no. */
static bool dummy_cb(ipc_handler_t *h, ipc_message_t *m, void *u)
{ (void)h; (void)m; (void)u; return true; }

static void test_kill_service_khong_hoi_sinh(void)
{
    fresh();
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, "rogue");
    lc.heartbeat_timeout_ms = 0;
    ipc_looper_t *lp = ipc_looper_create(&lc);
    static ipc_handler_t h;
    ipc_handler_init(&h, lp, dummy_cb, NULL, "rogue");
    ipc_service_register("rogue", &h);
    ipc_looper_poll(lp, 0);                 /* dua looper vao trang thai chay */

    ipc_health_rule_t r = { .code = IPC_EXC_PROTOCOL, .source = "rogue",
                            .min_severity = IPC_SEV_ERROR, .count = 1,
                            .action = IPC_ACT_KILL_SERVICE };
    ipc_health_add_rule(&r);

    ipc_health_report("rogue", IPC_EXC_PROTOCOL, IPC_SEV_ERROR, 0);
    ipc_health_check();

    CHECK_EQ(ipc_looper_state(lp), IPC_LOOPER_STOPPED);
    CHECK(ipc_service_get("rogue") == NULL); /* khong ai gui viec vao ho den nua */

    ipc_looper_destroy(lp);
}

static void test_vong_dem_day_thi_dem_so_bao_cao_mat(void)
{
    fresh();
    /* Bao nhieu hon suc chua ma khong goi check() -> phai dem so bi mat,
     * khong duoc im lang lam nhu khong co gi. */
    for (int i = 0; i < IPC_HEALTH_RING_SIZE + 10; ++i)
        ipc_health_report("spam", IPC_EXC_PROTOCOL, IPC_SEV_INFO, i);

    ipc_health_check();
    ipc_health_status_t st;
    ipc_health_get_status(&st);
    CHECK(st.dropped_reports >= 10);
}

void run_health_tests(void)
{
    ipc_fake_clock_init(&g_clock, 1000);
    ipc_clock_set(&g_clock.base);
    ipc_service_manager_init();

    printf("health:\n");
    RUN_TEST(test_khong_co_luat_thi_khong_lam_gi);
    RUN_TEST(test_luat_don_gian_ban_ngay);
    RUN_TEST(test_muc_do_thap_hon_thi_khong_kich_hoat);
    RUN_TEST(test_nguong_trong_cua_so_thoi_gian);
    RUN_TEST(test_loi_thua_thot_khong_kich_hoat);
    RUN_TEST(test_luat_hep_thang_luat_rong);
    RUN_TEST(test_luat_hep_chua_du_nguong_van_chan_luat_rong);
    RUN_TEST(test_fatal_thi_reboot);
    RUN_TEST(test_het_heap_sinh_exception);
    RUN_TEST(test_kill_service_khong_hoi_sinh);
    RUN_TEST(test_vong_dem_day_thi_dem_so_bao_cao_mat);

    ipc_clock_set(NULL);
}
