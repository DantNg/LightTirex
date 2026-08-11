#include "drivers.h"

#include <stdio.h>
#include <string.h>

/* ---------------- cam bien mo phong ---------------- */

static bool sim_read(sensor_driver_t *self, int32_t *out)
{
    (void)self;
    static int32_t step;
    /* Song rang cua 24.0 -> 26.0 do C, buoc 0.1 do. */
    int32_t v = 24000 + (step % 21) * 100;
    step++;
    *out = v;
    return true;
}

static sensor_driver_t s_sim = { .name = "sim", .read = sim_read, .impl = 0 };

sensor_driver_t *sensor_driver_sim(void) { return &s_sim; }

/* ---------------- cam bien gia (test) ---------------- */

static bool fake_read(sensor_driver_t *self, int32_t *out)
{
    sensor_driver_fake_t *f = (sensor_driver_fake_t *)self->impl;
    f->read_count++;
    if (f->always_fail || f->fail_next > 0) {
        if (f->fail_next > 0) f->fail_next--;
        f->fail_count++;
        return false;
    }
    *out = f->value_mc;
    return true;
}

void sensor_driver_fake_init(sensor_driver_fake_t *f, int32_t initial_mc)
{
    memset(f, 0, sizeof(*f));
    f->value_mc = initial_mc;
    f->base.name = "fake";
    f->base.read = fake_read;
    f->base.impl = f;
}

/* ---------------- server gia ---------------- */

static bool mock_online(cloud_client_t *self)
{
    return ((cloud_mock_t *)self->impl)->online;
}

static int mock_upload(cloud_client_t *self, const char *payload, uint32_t len)
{
    cloud_mock_t *m = (cloud_mock_t *)self->impl;
    if (!m->online) { m->fail_count++; return -1; }
    if (m->fail_next > 0) { m->fail_next--; m->fail_count++; return -2; }

    snprintf(m->last_payload, sizeof(m->last_payload), "%.*s",
             (int)(len < sizeof(m->last_payload) - 1 ? len
                                                     : sizeof(m->last_payload) - 1),
             payload);
    /* Dem so ban ghi bang so dau ';' phan cach trong payload. */
    uint32_t recs = 0;
    for (uint32_t i = 0; i < len; ++i) if (payload[i] == ';') recs++;
    m->records_received += recs;
    m->upload_count++;
    return 0;
}

void cloud_mock_init(cloud_mock_t *m)
{
    memset(m, 0, sizeof(*m));
    m->online = true;
    m->base.name = "cloud-mock";
    m->base.is_online = mock_online;
    m->base.upload = mock_upload;
    m->base.impl = m;
}
