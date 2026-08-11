/*
 * test_config.c - test config service voi storage RAM.
 *
 * Khong dong vao o dia that, nhung chay dung code tuan tu hoa + checksum se
 * chay tren MCU. Do la loi cua viec tach backend ra sau interface.
 */
#include "ipc_test.h"

#include "ipc_clock.h"
#include "ipc_config.h"
#include "ipc_looper.h"
#include "ipc_timer.h"

static ipc_fake_clock_t      g_clock;
static ipc_cfg_ram_storage_t g_store;
static int  g_changes;
static char g_last_key[32];

static const ipc_cfg_schema_t g_schema[] = {
    { "wifi.ssid",   IPC_CFG_STR,  0,    "default-ap" },
    { "wifi.pass",   IPC_CFG_STR,  0,    ""           },
    { "sample.hz",   IPC_CFG_INT,  10,   NULL         },
    { "log.enabled", IPC_CFG_BOOL, 1,    NULL         },
    { "tx.power",    IPC_CFG_INT,  -4,   NULL         },
};

static void on_change(const char *key, void *user)
{
    (void)user;
    g_changes++;
    snprintf(g_last_key, sizeof(g_last_key), "%s", key);
}

static void fresh(uint32_t autosave_ms)
{
    g_changes = 0;
    g_last_key[0] = 0;

    ipc_cfg_cfg_t c;
    ipc_cfg_cfg_default(&c);
    c.storage = &g_store.base;
    c.schema = g_schema;
    c.schema_count = sizeof(g_schema) / sizeof(g_schema[0]);
    c.on_change = on_change;
    c.autosave_delay_ms = autosave_ms;
    ipc_cfg_init(&c);
}

/* ------------------------------------------------------------------ */

static void test_mac_dinh_tu_schema(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);

    char ssid[32];
    ipc_cfg_get_str("wifi.ssid", ssid, sizeof(ssid), "?");
    CHECK_EQ(strcmp(ssid, "default-ap"), 0);
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 10);
    CHECK_EQ(ipc_cfg_get_bool("log.enabled", false), true);
    CHECK_EQ(ipc_cfg_get_int("tx.power", 0), -4);   /* so am phai qua duoc */
    CHECK_EQ(ipc_cfg_count(), 5);
    CHECK(!ipc_cfg_is_dirty());
}

static void test_set_get_va_bao_thay_doi(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);

    CHECK_EQ(ipc_cfg_set_int("sample.hz", 50), IPC_CFG_OK);
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 50);
    CHECK_EQ(g_changes, 1);
    CHECK_EQ(strcmp(g_last_key, "sample.hz"), 0);

    /* Dat lai dung gia tri cu = khong doi gi -> khong bao, khong ghi. */
    uint32_t saves = g_store.save_count;
    CHECK_EQ(ipc_cfg_set_int("sample.hz", 50), IPC_CFG_OK);
    CHECK_EQ(g_changes, 1);
    CHECK_EQ(g_store.save_count, saves);
}

static void test_sai_kieu_bi_tu_choi(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);
    CHECK_EQ(ipc_cfg_set_str("sample.hz", "nhanh"), IPC_CFG_ERR_TYPE);
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 10);   /* gia tri cu khong bi pha */
}

static void test_luu_roi_nap_lai(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);

    ipc_cfg_set_str("wifi.ssid", "nha-toi");
    ipc_cfg_set_int("sample.hz", 200);
    ipc_cfg_set_bool("log.enabled", false);
    CHECK_EQ(ipc_cfg_save(), IPC_CFG_OK);
    CHECK(!ipc_cfg_is_dirty());

    /* Mo phong khoi dong lai: reset ve mac dinh roi nap tu storage. */
    fresh(0);
    char ssid[32];
    ipc_cfg_get_str("wifi.ssid", ssid, sizeof(ssid), "?");
    CHECK_EQ(strcmp(ssid, "nha-toi"), 0);
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 200);
    CHECK_EQ(ipc_cfg_get_bool("log.enabled", true), false);
}

static void test_du_lieu_hong_thi_ve_mac_dinh(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);
    ipc_cfg_set_int("sample.hz", 999);
    ipc_cfg_save();

    /* Lat mot byte trong than file -> checksum sai. */
    g_store.buf[g_store.len - 3] ^= 0x20;

    ipc_cfg_cfg_t c;
    ipc_cfg_cfg_default(&c);
    c.storage = &g_store.base;
    c.schema = g_schema;
    c.schema_count = sizeof(g_schema) / sizeof(g_schema[0]);
    c.autosave_delay_ms = 0;
    CHECK_EQ(ipc_cfg_init(&c), IPC_CFG_ERR_CORRUPT);

    /* Quan trong: hong thi chay voi mac dinh, khong chay voi rac. */
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 10);
}

static void test_khoa_la_trong_file_bi_bo_qua(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);
    ipc_cfg_set_int("sample.hz", 77);
    ipc_cfg_save();
    uint32_t before = ipc_cfg_count();

    fresh(0);
    CHECK_EQ(ipc_cfg_count(), before);          /* khong sinh them khoa la */
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 77);
}

static void test_loi_ghi_thi_giu_co_ban(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);   /* autosave_delay = 0: set xong la ghi ngay */

    /* Bom loi IO vao dung lan ghi cua set. */
    g_store.fail_next_save = true;
    ipc_cfg_set_int("sample.hz", 33);
    /* Ghi hong thi van phai nho la con no viec, de con thu lai. */
    CHECK(ipc_cfg_is_dirty());

    CHECK_EQ(ipc_cfg_save(), IPC_CFG_OK);
    CHECK(!ipc_cfg_is_dirty());
}

static void test_chuoi_dai_bi_cat_khong_tran(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(0);
    char longstr[IPC_CFG_STR_LEN * 2];
    memset(longstr, 'x', sizeof(longstr) - 1);
    longstr[sizeof(longstr) - 1] = '\0';

    CHECK_EQ(ipc_cfg_set_str("wifi.pass", longstr), IPC_CFG_OK);
    char out[IPC_CFG_STR_LEN * 2];
    int n = ipc_cfg_get_str("wifi.pass", out, sizeof(out), "");
    CHECK_EQ(n, IPC_CFG_STR_LEN - 1);   /* cat gon, khong tran bo dem */
    CHECK_EQ(ipc_cfg_save(), IPC_CFG_OK);
}

/* Debounce: 100 lan set lien tiep chi ton DUNG MOT lan ghi flash. */
static void test_gop_nhieu_lan_ghi(void)
{
    ipc_cfg_storage_ram_init(&g_store);
    fresh(500);

    for (int i = 0; i < 100; ++i) {
        ipc_cfg_set_int("sample.hz", i + 1);
        ipc_fake_clock_advance(&g_clock, 5);   /* tong 500ms, moi lan lui han */
        ipc_timer_step(NULL);
    }
    CHECK_EQ(g_store.save_count, 0);           /* chua lang du lau -> chua ghi */

    ipc_fake_clock_advance(&g_clock, 500);
    ipc_timer_step(NULL);
    CHECK_EQ(g_store.save_count, 1);           /* dung mot lan ghi */
    CHECK(!ipc_cfg_is_dirty());
    CHECK_EQ(ipc_cfg_get_int("sample.hz", 0), 100);
}

void run_config_tests(void)
{
    ipc_fake_clock_init(&g_clock, 1000);
    ipc_clock_set(&g_clock.base);

    ipc_timer_engine_cfg_t ec;
    ipc_timer_engine_cfg_default(&ec);
    ec.own_task = false;
    ipc_timer_engine_start(&ec);

    printf("config:\n");
    RUN_TEST(test_mac_dinh_tu_schema);
    RUN_TEST(test_set_get_va_bao_thay_doi);
    RUN_TEST(test_sai_kieu_bi_tu_choi);
    RUN_TEST(test_luu_roi_nap_lai);
    RUN_TEST(test_du_lieu_hong_thi_ve_mac_dinh);
    RUN_TEST(test_khoa_la_trong_file_bi_bo_qua);
    RUN_TEST(test_loi_ghi_thi_giu_co_ban);
    RUN_TEST(test_chuoi_dai_bi_cat_khong_tran);
    RUN_TEST(test_gop_nhieu_lan_ghi);

    ipc_clock_set(NULL);
}
