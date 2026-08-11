/*
 * test_system.c - test HANH VI cua ca he thong IoT, khong phai test tung ham.
 *
 * Moi test o day mo ta mot tinh huong co that:
 *   "cam bien doc duoc thi du lieu di den dau"
 *   "doi cau hinh thi ai phai thay doi theo"
 *   "mat mang thi du lieu co mat khong"
 *   "mot dich vu chet giua chung thi he thong con chay khong"
 *
 * Ca he chay don luong voi thoi gian ao: khong sleep, khong thread, khong
 * phan cung, khong mang. Chay het trong vai mili giay va khong bao gio flaky.
 */
#include "ipc_test.h"

#include "common/app_events.h"
#include "ipc_clock.h"
#include "ipc_config.h"
#include "ipc_event.h"
#include "ipc_health.h"
#include "ipc_service.h"
#include "ipc_timer.h"
#include "app.h"
#include "common/services.h"

static ipc_fake_clock_t      g_clock;
static sensor_driver_fake_t  g_sensor;
static cloud_mock_t          g_cloud;
static ipc_cfg_ram_storage_t g_store;

/*
 * Mot "nhip" cua he thong: day thoi gian len, cho timer ban, cho cac looper
 * tieu thu message, roi cho health danh gia. Dung dung thu tu ma tren board
 * cac task se lam, chi khac la o day tuan tu.
 */
static void tick(uint32_t ms)
{
    ipc_fake_clock_advance(&g_clock, ms);
    ipc_timer_step(NULL);
    app_poll_all();
    ipc_health_check();
    app_poll_all();
}

static void tick_n(uint32_t times, uint32_t ms)
{
    for (uint32_t i = 0; i < times; ++i) tick(ms);
}

/* ------------------------------------------------------------------ */
/* Luong du lieu binh thuong                                           */
/* ------------------------------------------------------------------ */

static void test_mau_cam_bien_chay_het_duong_ong(void)
{
    g_sensor.value_mc = 25000;
    int32_t before = app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0);

    tick(1000);   /* dung mot chu ky lay mau */

    /* sensor da doc */
    CHECK_EQ(app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0), before + 1);
    /* processor da loc va cong bo */
    CHECK_EQ(app_service_get(SVC_PROCESSOR, PROCK_LAST_AVG, 0), 25000);
    /* config da luu gia tri moi nhat xuong (file gia trong RAM) */
    CHECK_EQ(app_service_get(SVC_CONFIG, CFGK_LAST_VALUE, 0), 25000);
    /* uploader da nhan va dang gom lo */
    CHECK((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) > 0 || (uint32_t)app_service_get(SVC_UPLOADER, UPK_UPLOADED, 0) > 0);
}

/* Day het phan ton dong de test sau bat dau tu hang doi rong. */
static void drain(void)
{
    for (int i = 0; i < 20 && (uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) > 0; ++i) tick(1000);
}

static void test_day_len_server_khi_du_lo(void)
{
    drain();
    uint32_t uploads_before = g_cloud.upload_count;
    int32_t batch = app_service_get(SVC_CONFIG, CFGK_UPLOAD_BATCH_N, 4);

    /* Chua du lo thi chua duoc day - gom lo la de do so lan bat song. */
    tick_n((uint32_t)batch - 1, 1000);
    CHECK_EQ(g_cloud.upload_count, uploads_before);

    tick(1000);   /* mau lam day lo */
    CHECK_EQ(g_cloud.upload_count, uploads_before + 1);
    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0), 0);
    CHECK(g_cloud.records_received > 0);
}

/* Config thay doi -> sensor tu doi nhip. Khong ai khoi dong lai ai. */
static void test_doi_cau_hinh_thi_sensor_doi_nhip_lay_mau(void)
{
    tick(1000);
    int32_t before = app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0);

    app_service_set(SVC_CONFIG, CFGK_PERIOD_MS, 200);   /* nhanh gap 5 lan */
    app_poll_all();                            /* su kien config lan toi sensor */

    tick_n(10, 100);                           /* 1000ms voi buoc 100ms */
    int32_t after = app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0);

    /* 1000ms / 200ms = 5 mau. Cho phep lech 1 do lam tron bien. */
    CHECK(after - before >= 4);
    CHECK(after - before <= 6);

    app_service_set(SVC_CONFIG, CFGK_PERIOD_MS, 1000);  /* tra lai cho cac test sau */
    app_poll_all();
    tick(1000);
}

static void test_vuot_nguong_thi_co_canh_bao(void)
{
    uint32_t before = (uint32_t)app_service_get(SVC_PROCESSOR, PROCK_ALERTS, 0);

    g_sensor.value_mc = 45000;    /* 45 do C, tren nguong 30 */
    tick_n(6, 1000);              /* du de trung binh truot vuot nguong */
    CHECK((uint32_t)app_service_get(SVC_PROCESSOR, PROCK_ALERTS, 0) > before);

    g_sensor.value_mc = 25000;    /* ve binh thuong */
    tick_n(6, 1000);
    uint32_t settled = (uint32_t)app_service_get(SVC_PROCESSOR, PROCK_ALERTS, 0);
    tick_n(3, 1000);
    CHECK_EQ((uint32_t)app_service_get(SVC_PROCESSOR, PROCK_ALERTS, 0), settled);  /* het vuot thi het bao */
}

/* ------------------------------------------------------------------ */
/* Duong hong: mang                                                    */
/* ------------------------------------------------------------------ */

static void test_mat_mang_thi_du_lieu_nam_lai_khong_mat(void)
{
    tick_n(8, 1000);                       /* day sach hang doi truoc */
    uint32_t uploaded_before = (uint32_t)app_service_get(SVC_UPLOADER, UPK_UPLOADED, 0);

    g_cloud.online = false;                /* rut day mang */
    tick_n(6, 1000);

    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_UPLOADED, 0), uploaded_before);  /* khong day duoc */
    CHECK((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) > 0);                   /* nhung con giu */
    uint32_t held = (uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0);

    g_cloud.online = true;                 /* co mang lai */
    ipc_bus_publish(TOPIC_NET_STATE, 1, 0);
    app_poll_all();

    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0), 0);
    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_UPLOADED, 0), uploaded_before + held);
}

static void test_loi_tam_thoi_thi_thu_lai(void)
{
    tick_n(8, 1000);
    uint32_t uploads_before = g_cloud.upload_count;

    g_cloud.fail_next = 1;                 /* mot lan day bi loi */
    tick_n(4, 1000);                       /* du mot lo */
    CHECK((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) > 0);   /* giu lai, chua mat */

    tick_n(3, 1000);                       /* het thoi gian cho -> thu lai */
    CHECK(g_cloud.upload_count > uploads_before);
    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0), 0);
}

static void test_hang_doi_tran_thi_bo_ban_ghi_cu_nhat_va_bao_len(void)
{
    tick_n(8, 1000);
    uint32_t dropped_before = (uint32_t)app_service_get(SVC_UPLOADER, UPK_DROPPED, 0);

    g_cloud.online = false;
    tick_n(30, 1000);                      /* nhieu hon suc chua cua hang doi */

    CHECK((uint32_t)app_service_get(SVC_UPLOADER, UPK_DROPPED, 0) > dropped_before);  /* co mat, va duoc dem */
    CHECK((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) <= 16);             /* khong tran bo nho */

    g_cloud.online = true;
    ipc_bus_publish(TOPIC_NET_STATE, 1, 0);
    app_poll_all();
    tick(1000);
}

/* ------------------------------------------------------------------ */
/* Duong hong: dich vu chet                                            */
/* ------------------------------------------------------------------ */

/*
 * Kich ban: task processor chet giua chung va duoc hoi sinh.
 * Yeu cau: luong du lieu chay tiep, va moi mau chi duoc xu ly DUNG MOT LAN.
 * Neu on_start khong huy dang ky cu truoc khi dang ky lai, sau khi hoi sinh
 * moi mau se bi xu ly hai lan - loi rat kho thay khi chay that.
 */
static void test_processor_chet_roi_hoi_sinh_khong_xu_ly_trung(void)
{
    tick(1000);
    ipc_looper_t *lp = app_looper(SVC_PROCESSOR);
    CHECK(lp != NULL);
    uint32_t gen_before = ipc_looper_generation(lp);

    CHECK(ipc_looper_restart_inplace(lp));   /* mo phong chet + hoi sinh */
    CHECK_EQ(ipc_looper_generation(lp), gen_before + 1);

    /* Dem so lan processor cong bo DATA_READY qua bien dem cua uploader. */
    uint32_t up_before = (uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) + (uint32_t)app_service_get(SVC_UPLOADER, UPK_UPLOADED, 0);
    tick(1000);
    uint32_t up_after = (uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0) + (uint32_t)app_service_get(SVC_UPLOADER, UPK_UPLOADED, 0);

    CHECK_EQ(up_after - up_before, 1);   /* dung mot ban ghi, khong phai hai */
}

/*
 * Kich ban: uploader chet khi trong hang doi con du lieu chua day len.
 * Yeu cau: du lieu khong bien mat theo task.
 */
static void test_uploader_chet_thi_du_lieu_cho_van_con(void)
{
    tick_n(8, 1000);
    g_cloud.online = false;
    tick_n(3, 1000);

    uint32_t pending = (uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0);
    CHECK(pending > 0);

    ipc_looper_t *lp = app_looper(SVC_UPLOADER);
    CHECK(ipc_looper_restart_inplace(lp));
    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0), pending);   /* con nguyen */

    g_cloud.online = true;
    ipc_bus_publish(TOPIC_NET_STATE, 1, 0);
    app_poll_all();
    CHECK_EQ((uint32_t)app_service_get(SVC_UPLOADER, UPK_PENDING, 0), 0);         /* va day duoc het */
}

/*
 * Kich ban: cam bien hong lien tuc.
 * Yeu cau: hong le te thi bo qua, hong 3 lan lien thi health khoi dong lai
 * dich vu cam bien - dung bang luat trong svc_health.c.
 */
static void test_cam_bien_hong_lien_tuc_thi_health_khoi_dong_lai(void)
{
    tick(1000);
    ipc_looper_t *lp = app_looper(SVC_SENSOR);
    uint32_t gen_before = ipc_looper_generation(lp);

    g_sensor.fail_next = 1;             /* mot lan hong: khong ai dong cham */
    tick(1000);
    CHECK_EQ(ipc_looper_generation(lp), gen_before);

    g_sensor.fail_next = 3;             /* hong lien tuc -> vuot nguong luat */
    tick_n(3, 1000);
    CHECK_EQ(ipc_looper_generation(lp), gen_before + 1);

    /* Va sau khi hoi sinh, nhip lay mau phai chay tiep. */
    int32_t s_before = app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0);
    tick_n(2, 1000);
    CHECK(app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0) > s_before);
}

/*
 * Bang dinh tuyen: moi loai message di den dung ham cua no, va message
 * khong ai nhan thi phai duoc DEM chu khong bi nuot im lang - message la
 * gan nhu luon la dau hieu dang ky bus sai ma `what`.
 */
static void test_message_khong_ai_nhan_thi_duoc_dem(void)
{
    tick(1000);
    app_service_t *svc = app_service_find(SVC_PROCESSOR);
    CHECK(svc != NULL);
    CHECK(svc->route_count > 0);

    uint32_t handled_before = svc->handled;
    uint32_t unhandled_before = svc->unhandled;

    /* Ma `what` khong co trong bang dinh tuyen cua processor. */
    ipc_handler_t *h = ipc_service_get(SVC_PROCESSOR);
    CHECK(h != NULL);
    ipc_handler_send_empty(h, 0xDEAD);
    app_poll_all();

    CHECK_EQ(svc->unhandled, unhandled_before + 1);
    CHECK_EQ(svc->handled, handled_before);   /* khong tinh la da xu ly */

    /* Message dung dia chi van chay binh thuong. */
    tick(1000);
    CHECK(svc->handled > handled_before);
}

/*
 * Kich ban: health quyet dinh giet han mot dich vu (khong hoi sinh).
 * Yeu cau: dich vu do bien mat khoi ServiceManager, nhung phan con lai cua
 * he thong van chay binh thuong.
 * Test nay pha huy nen dat cuoi cung.
 */
static void test_giet_han_mot_dich_vu_phan_con_lai_van_chay(void)
{
    tick(1000);

    /*
     * Them luat: uploader noi sai giao thuc mot lan la giet han.
     * Dung ma IPC_EXC_PROTOCOL chu khong phai EXC_UPLOAD_FAIL, vi bang luat
     * cua svc_health da co mot luat LOG cho EXC_UPLOAD_FAIL dang ky TRUOC -
     * luat dau tien khop se thang, luat them sau se khong bao gio toi luot.
     */
    ipc_health_rule_t kill_rule = { .code = IPC_EXC_PROTOCOL, .source = SVC_UPLOADER,
                                    .min_severity = IPC_SEV_ERROR, .count = 1,
                                    .window_ms = 0,
                                    .action = IPC_ACT_KILL_SERVICE };
    CHECK(ipc_health_add_rule(&kill_rule));

    ipc_health_report(SVC_UPLOADER, IPC_EXC_PROTOCOL, IPC_SEV_ERROR, 0);
    ipc_health_check();

    CHECK(ipc_service_get(SVC_UPLOADER) == NULL);   /* khong ai gui viec vao nua */
    CHECK_EQ(ipc_looper_state(app_looper(SVC_UPLOADER)), IPC_LOOPER_STOPPED);

    /* Phan con lai: cam bien van doc, processor van xu ly, config van luu. */
    int32_t s_before = app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0);
    g_sensor.value_mc = 26500;
    tick_n(2, 1000);
    CHECK(app_service_get(SVC_SENSOR, SENSORK_SAMPLES, 0) > s_before);
    CHECK(app_service_get(SVC_CONFIG, CFGK_LAST_VALUE, 0) != 0);
}

void run_system_tests(void)
{
    ipc_fake_clock_init(&g_clock, 1000);
    ipc_clock_set(&g_clock.base);

    sensor_driver_fake_init(&g_sensor, 25000);
    cloud_mock_init(&g_cloud);
    ipc_cfg_storage_ram_init(&g_store);

    app_cfg_t ac;
    app_cfg_default(&ac);
    ac.spawn_tasks = false;         /* don luong, do test tu bom nhip */
    ac.sensor = &g_sensor.base;
    ac.cloud = &g_cloud.base;
    ac.cfg_storage = &g_store.base;

    printf("he thong IoT:\n");
    if (!app_start(&ac)) {
        printf("  KHONG KHOI DONG DUOC\n");
        g_checks_failed++;
        return;
    }

    RUN_TEST(test_mau_cam_bien_chay_het_duong_ong);
    RUN_TEST(test_day_len_server_khi_du_lo);
    RUN_TEST(test_doi_cau_hinh_thi_sensor_doi_nhip_lay_mau);
    RUN_TEST(test_vuot_nguong_thi_co_canh_bao);
    RUN_TEST(test_mat_mang_thi_du_lieu_nam_lai_khong_mat);
    RUN_TEST(test_loi_tam_thoi_thi_thu_lai);
    RUN_TEST(test_hang_doi_tran_thi_bo_ban_ghi_cu_nhat_va_bao_len);
    RUN_TEST(test_processor_chet_roi_hoi_sinh_khong_xu_ly_trung);
    RUN_TEST(test_uploader_chet_thi_du_lieu_cho_van_con);
    RUN_TEST(test_cam_bien_hong_lien_tuc_thi_health_khoi_dong_lai);
    RUN_TEST(test_message_khong_ai_nhan_thi_duoc_dem);
    RUN_TEST(test_giet_han_mot_dich_vu_phan_con_lai_van_chay);

    app_stop();
    ipc_clock_set(NULL);
}
