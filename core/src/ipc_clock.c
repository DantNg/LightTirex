#include "ipc_clock.h"

static ipc_tick_t system_now(const ipc_clock_t *self)
{
    (void)self;
    return ipc_now_ms();
}

static const ipc_clock_t s_system_clock = {
    .now_ms = system_now,
    .impl   = 0,
    .name   = "system",
};

static const ipc_clock_t *s_active = &s_system_clock;

const ipc_clock_t *ipc_clock_system(void) { return &s_system_clock; }
const ipc_clock_t *ipc_clock_get(void)    { return s_active; }

void ipc_clock_set(const ipc_clock_t *clk)
{
    s_active = clk ? clk : &s_system_clock;
}

/* ---------------- fake ---------------- */

static ipc_tick_t fake_now(const ipc_clock_t *self)
{
    const ipc_fake_clock_t *fc = (const ipc_fake_clock_t *)self->impl;
    return fc->now;
}

void ipc_fake_clock_init(ipc_fake_clock_t *fc, ipc_tick_t start_ms)
{
    fc->now = start_ms;
    fc->base.now_ms = fake_now;
    fc->base.impl = fc;
    fc->base.name = "fake";
}

void ipc_fake_clock_advance(ipc_fake_clock_t *fc, uint32_t ms) { fc->now += ms; }
void ipc_fake_clock_set(ipc_fake_clock_t *fc, ipc_tick_t ms)   { fc->now = ms; }
