# Các module của core

Chi tiết từng module framework. Kiến trúc tổng thể xem
[ARCHITECTURE.md](ARCHITECTURE.md), luồng chạy xem [FLOW.md](FLOW.md).

## Timer

Timer có trách nhiệm **duy nhất**: biết "đến lúc nào thì phát một sự kiện".
Nó không chạy logic nghiệp vụ — đến hạn thì giao việc sang looper đích dưới
dạng message. Nhờ vậy task timer không bao giờ bị một callback nặng làm nghẽn,
và code xử lý vẫn chạy đúng trên context nó thuộc về.

Ba kiểu giao việc:

| Kiểu | Dùng khi |
|------|----------|
| `TO_HANDLER` | biết chính xác handler đích |
| `TO_SERVICE` | phân giải theo **tên** tại thời điểm nổ → dịch vụ chết rồi hồi sinh vẫn nhận được, không mất nhịp |
| `TO_CALLBACK` | việc cực ngắn (toggle GPIO, kick watchdog). Cấm block |

```c
ipc_timer_send_delayed(&h, MSG_TIMEOUT, 0, 1000);      // sau 1s gửi message
ipc_timer_send_periodic(&h, MSG_TICK, 100);            // 10Hz
ipc_timer_send_periodic_to("worker", MSG_POLL, 2000);  // bám theo tên
ipc_timer_call_after(blink_off, NULL, 50);
ipc_timer_restart(id, 0);          // đá lại từ đầu (kiểu inactivity timeout)
ipc_timer_cancel_for_handler(&h);  // dịch vụ tắt thì dọn sạch timer của nó
```

Hai ngữ nghĩa khi engine bị trễ nhiều chu kỳ — phải chọn đúng:

- `coalesce_missed = true` (mặc định): nhảy một phát 1s với chu kỳ 100ms chỉ
  bắn **1 lần** rồi căn giờ lại. Chọn cho heartbeat, polling, UI refresh —
  tránh dội một tràng message dồn cục.
- `coalesce_missed = false`: giữ đúng pha ban đầu, đếm đủ số nhịp đã lỡ. Chọn
  cho đo thời gian và lấy mẫu.

`ipc_timer_stats()` cho `fired / missed / dropped / max_lateness_ms` — đủ để
biết engine có đang bị bỏ đói hay không.

Id timer có mã hoá generation bên trong nên hủy nhầm một slot đã được cấp phát
lại cho người khác là **không thể** (lỗi ABA kinh điển, đã có test riêng).

## Watchdog

Hai tầng:

1. **Phần mềm** — mỗi client phải kick định kỳ. Quá hạn → escalate theo policy
   (`LOG` / `RESTART` / `RESET`). Looper thì không cần kick tay: watchdog đọc
   thẳng nhịp tim của nó.
2. **Phần cứng** — chỉ khi **tất cả** client còn khỏe, core mới cho HW WDT ăn.
   Nếu chính task watchdog chết hoặc treo, HW WDT không được ăn → chip tự
   reset. Đó là câu trả lời cho "ai canh người gác đền?".

Leo thang: `RESTART` gọi hàm phục hồi được **tiêm vào** (thường là
`ipc_supervisor_force_restart`); thất bại liên tiếp `max_recovery_attempts` lần
thì chuyển sang reset cả hệ thống.

```c
static bool recover(const char *name, ipc_looper_t *lp, void *u) {
    return lp ? ipc_supervisor_force_restart(lp) : false;
}

ipc_wdt_cfg_t w; ipc_wdt_cfg_default(&w);
w.backend = ipc_wdt_backend_esp32();   // desktop: noop hoặc fake
w.hw_timeout_ms = 10000;
w.recovery = recover;                  // watchdog KHÔNG phụ thuộc supervisor
w.max_recovery_attempts = 3;
ipc_wdt_start(&w);

ipc_wdt_watch_looper(worker_lp, 0, IPC_WDT_POLICY_RESTART);
ipc_wdt_handle_t h = ipc_wdt_register("dma_pump", 500, IPC_WDT_POLICY_RESET);
ipc_wdt_kick(h);                       // rẻ: không lấy mutex, gọi được từ ISR
ipc_wdt_suspend(h); /* ghi flash/OTA */ ipc_wdt_resume(h);
```

Ba tầng phát hiện lỗi không giẫm chân nhau, ngưỡng nới rộng dần:

```
looper heartbeat  →  supervisor (heartbeat_timeout)  →  watchdog (2×)  →  HW WDT
```

## Health service

Watchdog chỉ hỏi một câu: "còn sống không?". Health service hỏi "có đang ốm
không?" — thu thập nhiều tín hiệu rồi **quyết định** theo một bảng luật.

Tín hiệu vào: exception do code báo lên (`ipc_health_report`), heap free, độ sâu
hàng đợi từng looper, số message còn trong pool. Hành động ra: `LOG` /
`RESTART_SERVICE` / `KILL_SERVICE` (giết hẳn, supervisor **không** hồi sinh) /
`SAFE_MODE` / `REBOOT`.

```c
ipc_health_report("i2c", IPC_EXC_HW_FAULT, IPC_SEV_ERROR, err_code);  // gọi được từ ISR
```

`report()` chỉ ghi vào vòng đệm rồi trả về ngay — đánh giá luật làm ở task
health. Vòng đệm đầy thì **bỏ báo cáo mới**, giữ báo cáo cũ: báo cáo đầu tiên
của một sự cố thường có giá trị chẩn đoán cao nhất. Số bị mất được đếm vào
`dropped_reports` chứ không im lặng.

Bảng luật — đăng ký **hẹp trước, rộng sau**, luật đầu tiên khớp là người quyết
định:

```c
ipc_health_rule_t rules[] = {
    /* Lỗi I2C lẻ tẻ thì bỏ qua; 5 lần trong 10s mới là hỏng thật */
    { IPC_EXC_HW_FAULT, "i2c",   IPC_SEV_WARN,  5, 10000, IPC_ACT_RESTART_SERVICE },
    { IPC_EXC_PROTOCOL, "modem", IPC_SEV_ERROR, 3, 30000, IPC_ACT_KILL_SERVICE },
    { IPC_EXC_LOW_HEAP, NULL,    IPC_SEV_ERROR, 1, 0,     IPC_ACT_SAFE_MODE },
    { IPC_EXC_ANY,      NULL,    IPC_SEV_FATAL, 1, 0,     IPC_ACT_REBOOT },
};
```

Ngưỡng theo cửa sổ thời gian là điểm quan trọng nhất: **lỗi lẻ tẻ không được
kéo theo reboot cả board**. Và khi một luật hẹp khớp nhưng chưa đủ ngưỡng, nó
vẫn chặn luật rộng phía sau — nếu không, một lỗi UART vặt sẽ rơi xuống luật
`ANY → REBOOT`. Có test riêng cho đúng nhánh này (mutation-tested).

Health không biết cách reset chip (mượn `ipc_wdt_backend_t`) và không biết cách
hồi sinh task (nhận `recovery` tiêm vào). Đọc tài nguyên qua `ipc_health_probe_t`
nên test được "hết heap thì làm gì" mà không cần hết heap thật.

## Config service

Key-value bền vững, phân tầng: core giữ bảng + tuần tự hóa + checksum + gộp
ghi; backend chỉ chuyển byte. File stdio dùng được cho cả desktop lẫn
SPIFFS/FATFS trên MCU; backend RAM để test.

```c
static const ipc_cfg_schema_t schema[] = {
    { "sensor.hz",   IPC_CFG_INT,  10, NULL      },
    { "log.enabled", IPC_CFG_BOOL, 1,  NULL      },
    { "device.name", IPC_CFG_STR,  0,  "tirex-1" },
};

ipc_cfg_cfg_t c; ipc_cfg_cfg_default(&c);
c.storage = ipc_cfg_storage_file("/spiffs/app.cfg");
c.schema = schema; c.schema_count = 3;
c.autosave_delay_ms = 1000;      // gộp nhiều lần set thành một lần ghi
c.writer_looper = worker_lp;     // ghi file trên looper, không nghẽn task timer
ipc_cfg_init(&c);

ipc_cfg_set_int("sensor.hz", 50);          // dirty + hẹn giờ ghi
int hz = ipc_cfg_get_int("sensor.hz", 10);
```

Bốn quyết định đáng nói:

- **Schema là bản hợp đồng.** Chỉ khóa đã khai báo mới được nạp từ file — file
  cũ hoặc file bị sửa tay không thể bơm khóa lạ vào hệ thống. Thiếu khóa thì về
  mặc định.
- **Checksum FNV-1a.** File hỏng → `IPC_CFG_ERR_CORRUPT` và chạy với **mặc
  định**, không chạy với rác.
- **Ghi nguyên tố.** Ghi bản tạm rồi `rename()`. Mất điện giữa chừng thì file
  cũ vẫn nguyên vẹn.
- **Debounce.** 100 lần `set` liên tiếp chỉ tốn **đúng một** lần ghi flash — có
  test đếm chính xác `save_count`. Ghi lỗi thì giữ nguyên cờ dirty để còn thử
  lại.

