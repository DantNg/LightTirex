/*
 * drivers.h - hai thu "ben ngoai" cua he thong: cam bien va server.
 *
 * Ca hai deu la interface. Tren board that ta cam driver I2C va HTTP client
 * vao; trong test ta cam ban gia de ep loi xay ra dung luc minh muon
 * ("cam bien hong o mau thu 5", "mat mang trong 3 lan day"). Khong co lop
 * nay thi khong the test duoc kich ban hong hoc ma khong co phan cung.
 */
#ifndef APP_DRIVERS_H
#define APP_DRIVERS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- cam bien ---------------- */

typedef struct sensor_driver {
    const char *name;
    /* Tra false neu doc that bai (I2C nack, timeout...). */
    bool (*read)(struct sensor_driver *self, int32_t *out_milli_c);
    void *impl;
} sensor_driver_t;

/* Ban gia lap "that": sinh song rang cua quanh 25 do C. */
sensor_driver_t *sensor_driver_sim(void);

/* Ban dieu khien duoc, cho test. */
typedef struct {
    sensor_driver_t base;
    int32_t  value_mc;      /* gia tri se tra ve */
    uint32_t fail_next;     /* so lan doc ke tiep se that bai */
    bool     always_fail;
    uint32_t read_count;
    uint32_t fail_count;
} sensor_driver_fake_t;

void sensor_driver_fake_init(sensor_driver_fake_t *f, int32_t initial_mc);

/* ---------------- server (mock) ---------------- */

typedef struct cloud_client {
    const char *name;
    bool (*is_online)(struct cloud_client *self);
    /* Tra 0 neu day thanh cong, so am neu that bai. */
    int (*upload)(struct cloud_client *self, const char *payload, uint32_t len);
    void *impl;
} cloud_client_t;

/*
 * Server gia. He thong dang chay trong phong lab khong co mang, nen "day len
 * server" chi la ghi lai da day cai gi. Nhung duong that bai thi phai that:
 * offline, loi tam thoi - de con test duoc hang doi va co che thu lai.
 */
typedef struct {
    cloud_client_t base;
    bool     online;
    uint32_t fail_next;        /* n lan day ke tiep se loi */
    uint32_t upload_count;     /* so lan day thanh cong */
    uint32_t fail_count;
    uint32_t records_received; /* tong so ban ghi da nhan */
    char     last_payload[256];
} cloud_mock_t;

void cloud_mock_init(cloud_mock_t *m);

#ifdef __cplusplus
}
#endif
#endif /* APP_DRIVERS_H */
