/*
 * service_api.c - hien thuc lop cua chung cho dich vu.
 *
 * Moi ham o day deu rat mong: no dich mot loi goi "theo goc nhin dich vu"
 * thanh mot loi goi API loi, va dien danh tinh (self->name) vao cho can.
 *
 * Gia tri cua lop mong nay khong nam o dong code no chay, ma o cho: day la
 * DUY NHAT mot noi trong toan bo tang services cham vao ipc_event / ipc_health
 * / ipc_timer / ipc_service. Muon dem so lan cong bo, in trace, hay chan vong
 * lap su kien - sua o day, khong dich vu nao phai doi.
 */
#include "common/service_api.h"

#include "common/app_events.h"
#include "ipc_event.h"
#include "ipc_event_group.h"
#include "ipc_service.h"
#include "ipc_timer.h"

/*
 * Danh tinh cua dich vu, lay tu bang mo ta chu khong phai hang so viet tay.
 * Tra NULL neu self hong - moi ham goi deu phai kiem tra.
 */
static const char *service_name(app_service_t *self)
{
    return (self && self->name) ? self->name : NULL;
}

/*
 * Handler dang song cua dich vu, TRA CUU THEO TEN moi lan can.
 *
 * Co ve thua vi self->handler nam san trong bang mo ta. Nhung sau mot lan hoi
 * sinh, self->handler la vung nho da duoc khoi tao LAI - con ma nguon co the
 * dang giu con tro cu tu truoc do (vi du trong mot bien static luu tam). Tra
 * cuu qua ServiceManager luon cho ra handler cua vong doi HIEN TAI.
 */
static ipc_handler_t *service_handler(app_service_t *self)
{
    const char *name = service_name(self);
    return name ? ipc_service_get(name) : NULL;
}

/* ==================== CONG BO ==================== */

uint32_t svc_publish_topic(app_service_t *self, uint32_t topic,
                           int32_t arg1, int32_t arg2)
{
    if (!service_name(self)) return 0;
    return ipc_bus_publish(topic, arg1, arg2);
}

uint32_t svc_publish_state(app_service_t *self, uint32_t topic,
                           int32_t arg1, int32_t arg2)
{
    if (!service_name(self)) return 0;
    return ipc_bus_publish_retained(topic, arg1, arg2);
}

/* ==================== DANG KY NGHE ==================== */

bool svc_listen_topic(app_service_t *self, uint32_t topic, uint32_t as_what)
{
    const char *name = service_name(self);
    if (!name) return false;
    /* Dang ky theo TEN: song sot qua chu ky chet/hoi sinh cua dich vu. */
    return ipc_bus_subscribe_service(topic, name, as_what) != 0;
}

/* ==================== BAO SU VIEC ==================== */

void svc_report(app_service_t *self, uint32_t code, ipc_severity_t sev,
                int32_t detail)
{
    const char *name = service_name(self);
    if (!name) return;
    /* Nguon bao cao lay tu self->name - khong the bao nham duoi ten dich vu
     * khac khi sao chep code tu file nay sang file kia. */
    ipc_health_report(name, code, sev, detail);
}

void svc_report_warn(app_service_t *self, uint32_t code, int32_t detail)
{
    svc_report(self, code, IPC_SEV_WARN, detail);
}

void svc_report_error(app_service_t *self, uint32_t code, int32_t detail)
{
    svc_report(self, code, IPC_SEV_ERROR, detail);
}

/* ==================== HEN GIO ==================== */

uint32_t svc_timer_after(app_service_t *self, uint32_t what, int32_t arg1,
                         uint32_t delay_ms)
{
    /* Mot lan duy nhat -> ban theo con tro handler hien tai la du: neu dich vu
     * chet truoc khi timer no thi cai hen do cung khong con y nghia. */
    ipc_handler_t *h = service_handler(self);
    if (!h) return IPC_TIMER_NONE;
    return ipc_timer_send_delayed(h, what, arg1, delay_ms);
}

uint32_t svc_timer_every(app_service_t *self, uint32_t what, uint32_t period_ms)
{
    const char *name = service_name(self);
    if (!name) return IPC_TIMER_NONE;
    /* Lap lai -> ban theo TEN: nhip phai song sot qua chu ky chet/hoi sinh,
     * neu khong thi dich vu hoi sinh xong se nam im mai mai. */
    return ipc_timer_send_periodic_to(name, what, period_ms);
}

void svc_timer_stop(app_service_t *self, uint32_t timer_id)
{
    (void)self;
    if (timer_id != IPC_TIMER_NONE) ipc_timer_destroy(timer_id);
}

uint32_t svc_timer_stop_all(app_service_t *self)
{
    const char *name = service_name(self);
    uint32_t n = 0;
    if (!name) return 0;
    /* Huy ca hai duong: timer ban theo ten, va timer ban theo con tro handler
     * cua vong doi hien tai. Bo mot duong la con timer mo coi tiep tuc ban. */
    n += ipc_timer_cancel_for_service(name);
    ipc_handler_t *h = ipc_service_get(name);
    if (h) n += ipc_timer_cancel_for_handler(h);
    return n;
}

/* ==================== CO TRANG THAI ==================== */

void svc_flag_set(app_service_t *self, uint32_t bits)
{
    if (!service_name(self) || !app_bits()) return;
    ipc_event_group_set(app_bits(), bits);
}

void svc_flag_clear(app_service_t *self, uint32_t bits)
{
    if (!service_name(self) || !app_bits()) return;
    ipc_event_group_clear(app_bits(), bits);
}
