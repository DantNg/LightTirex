/*
 * test_bus.c - event bus (observer) va event group.
 */
#include "ipc_test.h"

#include "ipc_clock.h"
#include "ipc_event.h"
#include "ipc_event_group.h"
#include "ipc_looper.h"
#include "ipc_service.h"

#define TOPIC_A 900
#define TOPIC_B 901

static ipc_fake_clock_t g_clock;
static ipc_looper_t    *g_lp;
static ipc_handler_t    g_h1, g_h2;
static int g_n1, g_n2;
static int32_t g_last1, g_last2;
static uint32_t g_topic1;

static bool cb1(ipc_handler_t *h, ipc_message_t *m, void *u)
{ (void)h; (void)u; g_n1++; g_last1 = m->arg1; g_topic1 = m->topic; return true; }
static bool cb2(ipc_handler_t *h, ipc_message_t *m, void *u)
{ (void)h; (void)u; g_n2++; g_last2 = m->arg1; return true; }

static void reset(void)
{
    ipc_bus_unsubscribe_handler(&g_h1);
    ipc_bus_unsubscribe_handler(&g_h2);
    ipc_bus_unsubscribe_service("svc-x");
    ipc_bus_clear_retained(TOPIC_A);
    ipc_bus_clear_retained(TOPIC_B);
    ipc_looper_poll(g_lp, 0);
    g_n1 = g_n2 = 0;
    g_last1 = g_last2 = 0;
    g_topic1 = 0;
}

/* ------------------------------------------------------------------ */

static void test_mot_su_kien_toi_nhieu_nguoi_nghe(void)
{
    reset();
    ipc_bus_subscribe(TOPIC_A, &g_h1, 1);
    ipc_bus_subscribe(TOPIC_A, &g_h2, 2);

    CHECK_EQ(ipc_bus_publish(TOPIC_A, 42, 0), 2);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);
    CHECK_EQ(g_n2, 1);
    CHECK_EQ(g_last1, 42);
    CHECK_EQ(g_last2, 42);
    CHECK_EQ(g_topic1, TOPIC_A);   /* nguoi nghe biet su kien tu chu de nao */
}

static void test_khong_ai_nghe_thi_khong_phai_loi(void)
{
    reset();
    CHECK_EQ(ipc_bus_publish(TOPIC_B, 1, 0), 0);
    CHECK_EQ(ipc_looper_poll(g_lp, 0), 0);
}

static void test_chi_nhan_dung_chu_de_minh_dang_ky(void)
{
    reset();
    ipc_bus_subscribe(TOPIC_A, &g_h1, 1);
    ipc_bus_subscribe(TOPIC_B, &g_h2, 2);

    ipc_bus_publish(TOPIC_A, 7, 0);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);
    CHECK_EQ(g_n2, 0);
}

static void test_huy_dang_ky_thi_ngung_nhan(void)
{
    reset();
    ipc_sub_id_t id = ipc_bus_subscribe(TOPIC_A, &g_h1, 1);
    ipc_bus_publish(TOPIC_A, 1, 0);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);

    ipc_bus_unsubscribe(id);
    ipc_bus_publish(TOPIC_A, 2, 0);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);             /* khong tang nua */
}

/* Gia tri giu lai: nguoi den sau van biet trang thai hien tai. */
static void test_gia_tri_giu_lai_giao_ngay_khi_dang_ky(void)
{
    reset();
    ipc_bus_publish_retained(TOPIC_A, 99, 0);   /* chua ai nghe */

    ipc_bus_subscribe(TOPIC_A, &g_h1, 1);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);             /* nhan ngay, khong cho su kien ke tiep */
    CHECK_EQ(g_last1, 99);
}

/* Dang ky theo TEN: dich vu doi handler (hoi sinh) van nhan duoc. */
static void test_dang_ky_theo_ten_bam_theo_dich_vu(void)
{
    reset();
    ipc_handler_init(&g_h1, g_lp, cb1, NULL, "v1");
    ipc_service_register("svc-x", &g_h1);
    ipc_bus_subscribe_service(TOPIC_A, "svc-x", 1);

    ipc_bus_publish(TOPIC_A, 10, 0);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);

    /* Dich vu "chet" va "hoi sinh" voi handler khac. */
    ipc_handler_init(&g_h2, g_lp, cb2, NULL, "v2");
    ipc_service_register("svc-x", &g_h2);

    ipc_bus_publish(TOPIC_A, 20, 0);
    ipc_looper_poll(g_lp, 0);
    CHECK_EQ(g_n1, 1);             /* handler cu khong con nhan */
    CHECK_EQ(g_n2, 1);             /* handler moi nhan tiep */
    CHECK_EQ(g_last2, 20);
    ipc_service_unregister("svc-x");
}

static void test_dich_vu_bien_mat_thi_dem_la_rot(void)
{
    reset();
    ipc_bus_subscribe_service(TOPIC_A, "khong-ton-tai", 1);
    ipc_bus_stats_t before, after;
    ipc_bus_get_stats(&before);
    CHECK_EQ(ipc_bus_publish(TOPIC_A, 1, 0), 0);
    ipc_bus_get_stats(&after);
    CHECK(after.dropped > before.dropped);   /* mat thi phai dem, khong im lang */
    ipc_bus_unsubscribe_service("khong-ton-tai");
}

/* ---------------- event group ---------------- */

static void test_event_group_cho_tat_ca(void)
{
    ipc_event_group_t *g = ipc_event_group_create();
    CHECK(g != NULL);

    CHECK_EQ(ipc_event_group_wait(g, 0x3, true, false, 0), 0);  /* chua du */
    ipc_event_group_set(g, 0x1);
    CHECK_EQ(ipc_event_group_wait(g, 0x3, true, false, 0), 0);  /* van chua du */
    ipc_event_group_set(g, 0x2);
    CHECK_EQ(ipc_event_group_wait(g, 0x3, true, false, 0), 0x3);/* du ca hai */

    ipc_event_group_destroy(g);
}

static void test_event_group_cho_bat_ky(void)
{
    ipc_event_group_t *g = ipc_event_group_create();
    ipc_event_group_set(g, 0x4);
    CHECK_EQ(ipc_event_group_wait(g, 0x6, false, false, 0), 0x4); /* co 1 bit la du */
    ipc_event_group_destroy(g);
}

static void test_event_group_xoa_khi_thoat(void)
{
    ipc_event_group_t *g = ipc_event_group_create();
    ipc_event_group_set(g, 0x5);
    CHECK_EQ(ipc_event_group_wait(g, 0x1, true, true, 0), 0x5);
    CHECK_EQ(ipc_event_group_get(g), 0x4);    /* bit 0 da bi xoa, bit 2 con */
    ipc_event_group_destroy(g);
}

static void test_event_group_het_gio_thi_bao_that_bai(void)
{
    ipc_event_group_t *g = ipc_event_group_create();
    /* Timeout that (khong phai fake clock) vi no chan tren semaphore RTOS.
     * 20ms du ngan de test khong cham, du dai de khong flaky. */
    CHECK_EQ(ipc_event_group_wait(g, 0x8, true, false, 20), 0);
    ipc_event_group_destroy(g);
}

void run_bus_tests(void)
{
    ipc_fake_clock_init(&g_clock, 1000);
    ipc_clock_set(&g_clock.base);
    ipc_message_pool_init();
    ipc_service_manager_init();
    ipc_bus_init();

    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, "bustest");
    lc.heartbeat_timeout_ms = 0;
    g_lp = ipc_looper_create(&lc);
    ipc_handler_init(&g_h1, g_lp, cb1, NULL, "h1");
    ipc_handler_init(&g_h2, g_lp, cb2, NULL, "h2");

    printf("bus + event group:\n");
    RUN_TEST(test_mot_su_kien_toi_nhieu_nguoi_nghe);
    RUN_TEST(test_khong_ai_nghe_thi_khong_phai_loi);
    RUN_TEST(test_chi_nhan_dung_chu_de_minh_dang_ky);
    RUN_TEST(test_huy_dang_ky_thi_ngung_nhan);
    RUN_TEST(test_gia_tri_giu_lai_giao_ngay_khi_dang_ky);
    RUN_TEST(test_dang_ky_theo_ten_bam_theo_dich_vu);
    RUN_TEST(test_dich_vu_bien_mat_thi_dem_la_rot);
    RUN_TEST(test_event_group_cho_tat_ca);
    RUN_TEST(test_event_group_cho_bat_ky);
    RUN_TEST(test_event_group_xoa_khi_thoat);
    RUN_TEST(test_event_group_het_gio_thi_bao_that_bai);

    reset();
    ipc_looper_stop_permanently(g_lp);
    ipc_looper_destroy(g_lp);
    ipc_clock_set(NULL);
}
