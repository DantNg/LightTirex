/*
 * test_timer.c - test timer tren desktop, thoi gian ao, don luong.
 *
 * Khong co sleep(), khong co thread => chay het trong vai micro giay va
 * khong bao gio flaky. Do la ly do ipc_clock_t va ipc_looper_poll() ton tai.
 */
#include "ipc_test.h"

#include "ipc_clock.h"
#include "ipc_looper.h"
#include "ipc_service.h"
#include "ipc_timer.h"

static ipc_fake_clock_t g_clock;
static ipc_looper_t    *g_lp;
static ipc_handler_t    g_h;

static int      g_received;
static uint32_t g_last_what;
static int32_t  g_last_arg1;

static bool test_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    g_received++;
    g_last_what = m->what;
    g_last_arg1 = m->arg1;
    return true;
}

/* Day thoi gian ao len va cho he thong xu ly: timer -> queue -> handler. */
static void advance(uint32_t ms)
{
    ipc_fake_clock_advance(&g_clock, ms);
    ipc_timer_step(NULL);
    ipc_looper_poll(g_lp, 0);
}

/* Day tung buoc nho de timer dinh ky co co hoi ban dung so lan. */
static void advance_stepwise(uint32_t total_ms, uint32_t step_ms)
{
    for (uint32_t t = 0; t < total_ms; t += step_ms) advance(step_ms);
}

static void setup(void)
{
    g_received = 0;
    g_last_what = 0;
    g_last_arg1 = 0;
    ipc_looper_poll(g_lp, 0);   /* don sach message con sot tu test truoc */
    g_received = 0;
}

/* ------------------------------------------------------------------ */

static void test_oneshot_khong_ban_som(void)
{
    setup();
    ipc_timer_id_t id = ipc_timer_send_delayed(&g_h, 100, 7, 1000);
    CHECK(id != IPC_TIMER_NONE);

    advance(999);
    CHECK_EQ(g_received, 0);        /* truoc han: tuyet doi khong duoc ban */

    advance(1);
    CHECK_EQ(g_received, 1);
    CHECK_EQ(g_last_what, 100);
    CHECK_EQ(g_last_arg1, 7);

    advance(5000);
    CHECK_EQ(g_received, 1);        /* oneshot chi ban dung mot lan */
}

static void test_remaining_va_restart(void)
{
    setup();
    ipc_timer_id_t id = ipc_timer_send_delayed(&g_h, 1, 0, 1000);
    CHECK_EQ(ipc_timer_remaining_ms(id), 1000);

    advance(400);
    CHECK_EQ(ipc_timer_remaining_ms(id), 600);

    ipc_timer_restart(id, 0);       /* dat lai tu dau voi delay cu */
    CHECK_EQ(ipc_timer_remaining_ms(id), 1000);

    advance(999);
    CHECK_EQ(g_received, 0);
    advance(1);
    CHECK_EQ(g_received, 1);
}

static void test_stop_truoc_khi_no(void)
{
    setup();
    ipc_timer_id_t id = ipc_timer_send_delayed(&g_h, 1, 0, 500);
    CHECK(ipc_timer_is_active(id));
    CHECK(ipc_timer_stop(id));
    CHECK(!ipc_timer_is_active(id));

    advance(2000);
    CHECK_EQ(g_received, 0);
    ipc_timer_destroy(id);
}

static void test_id_cu_khong_dung_nham_timer_moi(void)
{
    setup();
    ipc_timer_id_t old = ipc_timer_send_delayed(&g_h, 1, 0, 500);
    ipc_timer_destroy(old);

    /* Slot duoc cap phat lai cho timer khac. Id cu phai vo hieu, khong duoc
     * huy nham timer moi (loi ABA kinh dien). */
    ipc_timer_id_t fresh = ipc_timer_send_delayed(&g_h, 2, 0, 500);
    CHECK(old != fresh);
    CHECK(!ipc_timer_is_active(old));
    CHECK(ipc_timer_is_active(fresh));

    ipc_timer_destroy(old);         /* khong duoc dong toi fresh */
    CHECK(ipc_timer_is_active(fresh));
    ipc_timer_destroy(fresh);
}

static void test_periodic_ban_dung_so_lan(void)
{
    setup();
    ipc_timer_id_t id = ipc_timer_send_periodic(&g_h, 42, 100);

    advance_stepwise(1000, 10);
    CHECK_EQ(g_received, 10);
    CHECK_EQ(g_last_what, 42);

    ipc_timer_stop(id);
    advance_stepwise(500, 10);
    CHECK_EQ(g_received, 10);       /* dung roi thi im hoan toan */
    ipc_timer_destroy(id);
}

static void test_periodic_tre_coalesce(void)
{
    setup();
    /* coalesce = true: nhay mot phat 1 giay -> chi ban 1 lan, khong bao
     * mot tran 10 message dồn cuc. */
    ipc_timer_id_t id = ipc_timer_send_periodic(&g_h, 1, 100);

    advance(1000);
    CHECK_EQ(g_received, 1);

    ipc_timer_stats_t st;
    CHECK(ipc_timer_stats(id, &st));
    CHECK(st.missed >= 8);          /* co ghi nhan la da lo nhip */
    CHECK(st.max_lateness_ms >= 900);
    ipc_timer_destroy(id);
}

static void test_periodic_tre_bu_du_nhip(void)
{
    setup();
    ipc_timer_cfg_t c;
    ipc_timer_cfg_default(&c);
    c.name = "catchup";
    c.mode = IPC_TIMER_PERIODIC;
    c.delivery = IPC_TIMER_TO_HANDLER;
    c.handler = &g_h;
    c.what = 5;
    c.delay_ms = 100;
    c.period_ms = 100;
    c.coalesce_missed = false;      /* giu dung pha, khong bo nhip */
    ipc_timer_id_t id = ipc_timer_create(&c);

    /* Nhay 1 giay: engine ban 1 lan moi step, nhung deadline duoc keo len
     * bu du, nen cac lan sau khong bi troi pha. */
    advance(1000);
    CHECK_EQ(g_received, 1);
    CHECK_EQ(ipc_timer_remaining_ms(id), 100);  /* nhip ke tiep dung pha */

    ipc_timer_destroy(id);
}

static int g_cb_hits;
static void bump_cb(void *arg) { (void)arg; g_cb_hits++; }

static void test_callback_chay_tren_engine(void)
{
    setup();
    g_cb_hits = 0;

    /* Cau hinh sai phai bi tu choi ngay luc tao, khong im lang roi hong sau. */
    CHECK_EQ(ipc_timer_call_after(NULL, NULL, 100), IPC_TIMER_NONE);

    ipc_timer_call_after(bump_cb, NULL, 100);
    advance(99);
    CHECK_EQ(g_cb_hits, 0);
    advance(1);
    CHECK_EQ(g_cb_hits, 1);
    /* Callback chay thang tren engine, khong di qua message pool. */
    CHECK_EQ(g_received, 0);
}

/* Dich vu "chet" roi "hoi sinh" voi handler moi: timer TO_SERVICE phai bam
 * theo ten chu khong bam theo con tro cu. */
static ipc_handler_t g_svc_h1, g_svc_h2;
static int g_svc1_count, g_svc2_count;

static bool svc1_cb(ipc_handler_t *h, ipc_message_t *m, void *u)
{ (void)h; (void)m; (void)u; g_svc1_count++; return true; }
static bool svc2_cb(ipc_handler_t *h, ipc_message_t *m, void *u)
{ (void)h; (void)m; (void)u; g_svc2_count++; return true; }

static void test_timer_theo_ten_song_sot_qua_restart(void)
{
    setup();
    g_svc1_count = g_svc2_count = 0;

    ipc_handler_init(&g_svc_h1, g_lp, svc1_cb, NULL, "v1");
    ipc_service_register("worker", &g_svc_h1);

    ipc_timer_id_t id = ipc_timer_send_periodic_to("worker", 9, 100);
    advance_stepwise(300, 10);
    CHECK_EQ(g_svc1_count, 3);

    /* Mo phong: task chet, hoi sinh, dang ky lai handler MOI cung ten. */
    ipc_handler_init(&g_svc_h2, g_lp, svc2_cb, NULL, "v2");
    ipc_service_register("worker", &g_svc_h2);

    advance_stepwise(300, 10);
    CHECK_EQ(g_svc1_count, 3);      /* handler cu khong con nhan */
    CHECK_EQ(g_svc2_count, 3);      /* handler moi nhan tiep, khong mat nhip */

    ipc_timer_destroy(id);
    ipc_service_unregister("worker");
}

static void test_cancel_theo_handler(void)
{
    setup();
    ipc_timer_send_delayed(&g_h, 1, 0, 500);
    ipc_timer_send_delayed(&g_h, 2, 0, 600);
    ipc_timer_send_periodic(&g_h, 3, 700);

    CHECK_EQ(ipc_timer_cancel_for_handler(&g_h), 3);
    advance(5000);
    CHECK_EQ(g_received, 0);
}

static void test_nhieu_timer_dung_thu_tu(void)
{
    setup();
    static uint32_t order[4];
    static int n;
    n = 0;
    memset(order, 0, sizeof(order));

    /* Tao theo thu tu nguoc voi thu tu den han. */
    ipc_timer_send_delayed(&g_h, 30, 0, 300);
    ipc_timer_send_delayed(&g_h, 10, 0, 100);
    ipc_timer_send_delayed(&g_h, 20, 0, 200);

    advance(100); CHECK_EQ(g_last_what, 10);
    advance(100); CHECK_EQ(g_last_what, 20);
    advance(100); CHECK_EQ(g_last_what, 30);
    CHECK_EQ(g_received, 3);
    (void)order; (void)n;
}

void run_timer_tests(void)
{
    ipc_fake_clock_init(&g_clock, 1000);
    ipc_clock_set(&g_clock.base);

    ipc_message_pool_init();
    ipc_service_manager_init();

    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, "test");
    lc.heartbeat_timeout_ms = 0;     /* khong giam sat trong test timer */
    g_lp = ipc_looper_create(&lc);
    ipc_handler_init(&g_h, g_lp, test_cb, NULL, "test");

    ipc_timer_engine_cfg_t ec;
    ipc_timer_engine_cfg_default(&ec);
    ec.own_task = false;             /* test tu goi ipc_timer_step() */
    ipc_timer_engine_start(&ec);

    printf("timer:\n");
    RUN_TEST(test_oneshot_khong_ban_som);
    RUN_TEST(test_remaining_va_restart);
    RUN_TEST(test_stop_truoc_khi_no);
    RUN_TEST(test_id_cu_khong_dung_nham_timer_moi);
    RUN_TEST(test_periodic_ban_dung_so_lan);
    RUN_TEST(test_periodic_tre_coalesce);
    RUN_TEST(test_periodic_tre_bu_du_nhip);
    RUN_TEST(test_callback_chay_tren_engine);
    RUN_TEST(test_timer_theo_ten_song_sot_qua_restart);
    RUN_TEST(test_cancel_theo_handler);
    RUN_TEST(test_nhieu_timer_dung_thu_tu);

    ipc_timer_cancel_for_handler(&g_h);
    ipc_looper_destroy(g_lp);
    ipc_clock_set(NULL);
}
