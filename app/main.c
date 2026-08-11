/*
 * main.c - diem vao cua ung dung.
 *
 * Tren ESP-IDF: app_main() duoc goi tu dong.
 * Tren desktop: chay ./app_demo de xem he thong chay that voi task that.
 */
#include "app.h"
#include "services.h"

#include "ipc_event.h"
#include "ipc_port.h"

#include <stdio.h>

static void print_line(const char *line) { printf("%s\n", line); }

/* Nghe TOPIC_DATA_READY de in ra man hinh - chi la mot nguoi nghe nua tren
 * bus, khong dich vu nao phai biet den su ton tai cua no. */
static ipc_looper_t  *g_console_lp;
static ipc_handler_t  g_console_h;

static bool console_cb(ipc_handler_t *h, ipc_message_t *m, void *user)
{
    (void)h; (void)user;
    switch (m->topic) {
    case TOPIC_DATA_READY:
        printf("[data ] trung binh = %ld.%03ld C (mau #%ld)\n",
               (long)(m->arg1 / 1000), (long)(m->arg1 % 1000), (long)m->arg2);
        break;
    case TOPIC_ALERT:
        printf("[canh bao] %ld.%03ld C %s nguong\n",
               (long)(m->arg1 / 1000), (long)(m->arg1 % 1000),
               m->arg2 > 0 ? "tren" : "duoi");
        break;
    case TOPIC_UPLOAD_RESULT:
        printf("[server] %ld ban ghi -> %s\n", (long)m->arg1,
               m->arg2 ? "da day" : "THAT BAI, giu lai trong hang doi");
        break;
    default:
        break;
    }
    return true;
}

static void start_console(void)
{
    ipc_looper_cfg_t lc;
    ipc_looper_cfg_default(&lc, "console");
    lc.priority = 3;
    g_console_lp = ipc_looper_create(&lc);
    ipc_handler_init(&g_console_h, g_console_lp, console_cb, NULL, "console");
    ipc_looper_start(g_console_lp);

    ipc_bus_subscribe(TOPIC_DATA_READY, &g_console_h, 1);
    ipc_bus_subscribe(TOPIC_ALERT, &g_console_h, 2);
    ipc_bus_subscribe(TOPIC_UPLOAD_RESULT, &g_console_h, 3);
}

int app_run(void)
{
    app_cfg_t cfg;
    app_cfg_default(&cfg);
    cfg.spawn_tasks = true;   /* task that; driver va server dung ban mac dinh */

    if (!app_start(&cfg)) {
        printf("khoi dong that bai\n");
        return 1;
    }
    start_console();
    printf("he thong da chay. Cac dich vu:\n");
    app_dump(print_line);

    /* Doi cau hinh luc dang chay: sensor tu doi nhip, khong ai khoi dong lai. */
    ipc_sleep_ms(5000);
    printf("\n-- doi chu ky lay mau xuong 300ms --\n");
    app_service_set(SVC_CONFIG, CFGK_PERIOD_MS, 300);

    ipc_sleep_ms(5000);
    printf("\n-- trang thai --\n");
    app_dump(print_line);
    return 0;
}

#ifdef ESP_PLATFORM
void app_main(void) { app_run(); }
#else
int main(void)
{
    int rc = app_run();
    /* Tren desktop: de cac task chay them mot lat roi thoat. */
    ipc_sleep_ms(3000);
    return rc;
}
#endif
