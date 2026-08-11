/*
 * service_api.h - MOT cua duy nhat de dich vu noi chuyen voi the gioi ben ngoai.
 *
 * VAN DE truoc day
 * ----------------
 * Moi dich vu tu goi thang API loi: sensorService goi ipc_bus_publish() de cong
 * bo, ipc_health_report() de bao hong, ipc_timer_send_periodic_to() de dat nhip.
 * Ba module loi khac nhau, ba cach truyen "toi la ai" khac nhau:
 *
 *     ipc_bus_publish(TOPIC_SENSOR_SAMPLE, mc, n);            // khong noi ai gui
 *     ipc_health_report(SVC_SENSOR, EXC_SENSOR_READ, ...);    // ten viet tay
 *     ipc_timer_send_periodic_to(SVC_SENSOR, MSG_TICK, p);    // ten viet tay
 *     ipc_timer_send_delayed(ipc_service_get(SVC_X), ...);    // phai tu tra cuu
 *
 * Ba hau qua:
 *   1. Dich vu phai #include ipc_event.h + ipc_health.h + ipc_timer.h +
 *      ipc_service.h. Doc mot file dich vu la thay bon module loi.
 *   2. Ten dich vu bi viet tay lai o moi cho goi. Sao chep mot ham tu dich vu
 *      nay sang dich vu kia ma quen doi hang so ten -> bao cao hong duoi ten
 *      NGUOI KHAC. Trinh bien dich khong bao gi.
 *   3. Muon them gi vao moi lan cong bo (dem, trace, chan vong lap su kien)
 *      thi phai sua tat ca cho goi.
 *
 * CACH LAM BAY GIO
 * ----------------
 * Dich vu chi goi cac ham svc_* duoi day, ham nao cung nhan `self` lam tham so
 * dau. Danh tinh lay tu self->name, khong con viet tay:
 *
 *     svc_publish_topic(self, TOPIC_SENSOR_SAMPLE, mc, n);
 *     svc_report_warn(self, EXC_SENSOR_READ, streak);
 *     svc_timer_every(self, MSG_SENSOR_TICK, period);
 *
 * Dich vu chi con #include "common/services.h". Muon them trace hay dem cho
 * moi lan cong bo thi sua DUNG MOT cho: service_api.c.
 *
 * QUY UOC DAT TEN
 * ---------------
 *   svc_publish_*   gui ra ngoai, khong biet ai nghe, khong cho tra loi
 *   svc_listen_*    dang ky nghe (chi goi trong on_subscribe)
 *   svc_report_*    bao SU VIEC len health, KHONG tu quyet dinh xu ly
 *   svc_timer_*     hen gio ban message ve chinh minh
 *   svc_flag_*      dat/xoa co trang thai tren event group dung chung
 *
 * Moi ham deu an toan khi self == NULL: tra ve 0/false, khong sap.
 */
#ifndef APP_SERVICE_API_H
#define APP_SERVICE_API_H

#include "common/service_iface.h"
#include "ipc_health.h"   /* ipc_severity_t - kieu tra ve trong chu ky svc_report */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Id timer "khong co". Dich vu giu id timer trong state cua minh thi dung
 * uint32_t + hang so nay, khong can keo ipc_timer.h vao.
 */
#define SVC_TIMER_NONE ((uint32_t)0)

/* ==================== CONG BO ==================== */

/*
 * Cong bo mot su kien len bus.
 *
 * Nguoi nghe nao dang dang ky topic nay se duoc DAT MOT MESSAGE RIENG vao hang
 * doi cua ho. Ham tra ve ngay sau khi dat xong message, KHONG cho ai xu ly.
 *
 * Tra ve: so nguoi nghe thuc su nhan duoc. 0 nghia la khong ai nghe topic nay,
 * hoac het slot trong pool message. Ca hai truong hop deu khong phai loi ma
 * dich vu can xu ly - health se thay qua thong ke bus.
 *
 * Dung cho SU KIEN (chuyen vua xay ra): mot mau vua doc duoc, mot lan day len
 * server vua xong.
 */
uint32_t svc_publish_topic(app_service_t *self, uint32_t topic,
                           int32_t arg1, int32_t arg2);

/*
 * Nhu tren, nhung bus GIU LAI gia tri cuoi cung.
 *
 * Ai dang ky topic nay SAU do se nhan duoc ngay gia tri dang giu, khong phai
 * doi lan cong bo ke tiep. Dung cho TRANG THAI (dieu dang dung): he thong dang
 * khoe hay om, mang dang co hay mat.
 *
 * Khac biet quan trong khi dich vu hoi sinh: dich vu vua song lai dang ky nghe
 * TOPIC_HEALTH_STATE se biet ngay he thong dang the nao, thay vi mu tit cho
 * den lan doi trang thai tiep theo.
 */
uint32_t svc_publish_state(app_service_t *self, uint32_t topic,
                           int32_t arg1, int32_t arg2);

/* ==================== DANG KY NGHE ==================== */

/*
 * Dang ky nghe mot topic. CHI goi trong on_subscribe().
 *
 * `as_what` la ma message se hien trong bang routes[] cua dich vu. Bus khong
 * gui thang topic vao `what` vi mot dich vu co the muon hai topic khac nhau
 * cung chay ve mot ham xu ly, hoac nguoc lai.
 *
 * Dang ky theo TEN dich vu (self->name) chu khong theo con tro handler. Dich vu
 * chet va hoi sinh thi handler la con tro khac, nhung ten khong doi - dang ky
 * van tro dung noi.
 *
 * Khong can huy dang ky cu: khung da goi ipc_bus_unsubscribe_service() truoc
 * khi goi on_subscribe(). Xem so do vong doi trong service_iface.h.
 */
bool svc_listen_topic(app_service_t *self, uint32_t topic, uint32_t as_what);

/* ==================== BAO SU VIEC LEN HEALTH ==================== */

/*
 * Bao mot su viec bat thuong.
 *
 * RANH GIOI TRACH NHIEM - doc ky truoc khi dung:
 *
 *   Dich vu bao SU VIEC.       "lan doc thu 3 lien tiep that bai"
 *   healthService quyet DINH.  "3 lan trong 10 giay -> khoi dong lai"
 *
 * Dich vu KHONG tu leo thang muc do theo so lan hong, KHONG tu goi restart,
 * KHONG tu quyet dinh bo qua. Neu lam ca hai noi thi nguong bi chia doi: dich
 * vu doi 3 lan moi bao ERROR, health doi 3 ERROR moi xu ly -> thuc te phai
 * hong 9 lan. Doc code o ca hai file cung khong doan ra con so 9 do.
 *
 * `detail` la so lieu kem theo (lan thu may, con lai bao nhieu ban ghi...),
 * health ghi lai de xem lai chu khong dung de quyet dinh.
 */
void svc_report(app_service_t *self, uint32_t code, ipc_severity_t sev,
                int32_t detail);

/* Duong tat cho hai muc hay dung nhat. */

/* Hong nhung con chay duoc: doc cam bien loi mot lan, day len server that bai. */
void svc_report_warn(app_service_t *self, uint32_t code, int32_t detail);

/* Hong den muc khong lam tiep duoc: het bo nho, file cau hinh vo. */
void svc_report_error(app_service_t *self, uint32_t code, int32_t detail);

/* ==================== HEN GIO ==================== */

/*
 * Ban `what` ve chinh dich vu nay sau `delay_ms`, dung mot lan.
 *
 * Tra ve id timer de con huy (svc_timer_stop). IPC_TIMER_NONE neu that bai.
 *
 * Message ban ve se di qua bang routes[] y het message tu bus - dich vu khong
 * can phan biet "nay la timer cua toi" hay "nay la su kien nguoi khac gui".
 */
uint32_t svc_timer_after(app_service_t *self, uint32_t what, int32_t arg1,
                         uint32_t delay_ms);

/*
 * Ban `what` ve chinh dich vu nay moi `period_ms`, lap lai mai.
 *
 * Gui theo TEN nen nhip SONG SOT qua chu ky chet/hoi sinh: dich vu chet, task
 * moi len, handler moi - nhip van ban vao dung dich vu do, khong dut.
 */
uint32_t svc_timer_every(app_service_t *self, uint32_t what, uint32_t period_ms);

/* Huy mot timer theo id. Bo qua neu id la IPC_TIMER_NONE. */
void svc_timer_stop(app_service_t *self, uint32_t timer_id);

/*
 * Huy MOI timer dang ban ve dich vu nay.
 *
 * Goi o dau on_create() truoc khi dat nhip moi: on_create chay lai sau moi lan
 * hoi sinh, khong don thi se co hai timer cung ban ve mot cho.
 * Tra ve so timer da huy.
 */
uint32_t svc_timer_stop_all(app_service_t *self);

/* ==================== CO TRANG THAI ==================== */

/*
 * Dat/xoa co tren event group dung chung cua ung dung.
 *
 * Khac bus o cho: co la TRANG THAI BEN VUNG, ai hoi luc nao cung duoc, va co
 * the CHO tren no (ipc_event_group_wait). Bus la su kien thoang qua, khong doi
 * duoc. Dung co cho "mang dang online", dung bus cho "vua day xong mot goi".
 */
void svc_flag_set(app_service_t *self, uint32_t bits);
void svc_flag_clear(app_service_t *self, uint32_t bits);

#ifdef __cplusplus
}
#endif
#endif /* APP_SERVICE_API_H */
