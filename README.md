# lightTirex — Looper/Handler/Message IPC port sang RTOS

Port mô hình event-driven của Android/Linux (`Looper`, `Handler`, `Message`,
`MessageQueue`, `ServiceManager`/Binder, `init` respawn) sang môi trường RTOS
đơn-không-gian-địa-chỉ (FreeRTOS), với **multithread + event driven** và
**khả năng hồi sinh task chết lúc runtime**.

## Ánh xạ khái niệm

| Linux / Android            | Ở đây                        | Ghi chú |
|---------------------------|------------------------------|---------|
| process                   | task (thread)                | chung address space → "IPC" thành ITC, không copy payload |
| `Looper.loop()`           | `ipc_looper_run()`           | vòng lặp lấy message theo mốc thời gian |
| `MessageQueue`            | danh sách liên kết sorted theo `(when_ms, seq)` | hỗ trợ delayed message, không cần timer riêng |
| `ThreadLocal<Looper>`     | `ipc_tls_get/set`            | biết mình đang chạy trên looper nào |
| `Handler` + `handleMessage` | `ipc_handler_t` + callback  | nhiều handler trên cùng một looper |
| `Message.obtain()` pool   | `ipc_message_obtain()`       | pool tĩnh, không malloc trong đường chạy nóng |
| Binder + `ServiceManager` | `ipc_service_register/get`   | tra cứu **theo tên**, không giữ con trỏ thô |
| `linkToDeath`             | `ipc_service_link_to_death`  | client biết dịch vụ chết/sống lại |
| `init` respawn / zygote   | `ipc_supervisor`             | phát hiện + tạo lại task lúc runtime |
| socket/binder từ kernel   | `ipc_handler_send_from_isr`  | ISR bắn event vào looper |

## Nguyên tắc thiết kế chịu lỗi

Điểm mấu chốt: **`ipc_looper_t` là object độc lập với task**. Task chỉ là "cơ
bắp" chạy vòng lặp. Khi task chết:

1. Object looper vẫn còn nguyên → hàng đợi message **không mất**.
2. Supervisor tạo task mới gắn vào **đúng object cũ**, `generation++`.
3. `on_start` chạy trong task mới → dịch vụ tự khởi tạo lại state và
   `ipc_service_register` lại chính nó.
4. Client tra cứu theo tên nên không cầm con trỏ chết; ai cần biết thì dùng
   `link_to_death`.

### Các đường chết đã cover

| Kiểu chết | Phát hiện | Xử lý |
|-----------|-----------|-------|
| Task tự `vTaskDelete` / bị xóa ngoài | heartbeat dừng cập nhật | tạo task mới |
| Treo vô hạn trong handler (infinite loop, deadlock) | heartbeat quá hạn | `ipc_task_delete` xác cũ rồi tạo lại |
| Thoát có trật tự (`ipc_looper_quit`) | cờ `task_gone` + state `STOPPED` | không hồi sinh |
| Message đang dispatch lúc chết | `in_flight` được lưu | recycle về pool, không rò rỉ |
| Mutex chết cùng chủ sở hữu | `ipc_mutex_lock(50ms)` thất bại | hủy và tạo lại mutex |
| Caller đang `send_sync` chờ task vừa chết | timeout | sync token đánh dấu `abandoned`, bên kia dọn |
| Crash lặp vô hạn | `max_restarts` trong `restart_window_ms` | dừng hẳn, purge queue, báo qua `on_event` |

Heartbeat được cập nhật **cả khi rảnh lẫn khi bận**: vòng lặp chặn tối đa
`heartbeat_timeout_ms / 2` rồi thức dậy làm mới nhịp. Nhờ vậy "im lặng" luôn
đồng nghĩa với "chết", không cần phân biệt idle với hang.

## API cốt lõi

```c
ipc_looper_cfg_t cfg;
ipc_looper_cfg_default(&cfg, "worker");
cfg.heartbeat_timeout_ms = 3000;   // phải > thời gian xử lý message lâu nhất
cfg.max_restarts         = 10;
cfg.on_start             = worker_on_start;   // chạy TRONG task mới
ipc_looper_t *lp = ipc_looper_create(&cfg);
ipc_looper_start(lp);

// trong on_start:
ipc_handler_init(&h, lp, worker_cb, NULL, "worker");
ipc_service_register("worker", &h);

// từ task khác:
ipc_service_send("worker", MSG_WORK, 42, 0);
ipc_service_call_sync("worker", MSG_QUERY, 0, &buf, 500);  // có timeout
ipc_handler_send_empty_delayed(&h, MSG_TICK, 1000);        // hẹn giờ
ipc_handler_post(&h, my_runnable, arg);                    // Handler.post
ipc_handler_remove(&h, MSG_TICK);                          // hủy message chờ

// supervisor:
ipc_supervisor_cfg_t s; ipc_supervisor_cfg_default(&s);
s.priority = 20;                    // PHẢI cao hơn mọi looper được giám sát
ipc_supervisor_start(&s);
```

## Quy tắc dùng (quan trọng)

- **Khởi tạo state trong `on_start`, không phải trước `ipc_looper_start`** —
  vì `on_start` chạy lại mỗi lần hồi sinh.
- **`heartbeat_timeout_ms` phải lớn hơn message chậm nhất**, nếu không
  supervisor sẽ giết oan một task đang làm việc thật.
- **Supervisor phải có priority cao hơn mọi looper**, nếu không một looper
  priority cao bị treo sẽ chặn luôn supervisor.
- **Không gọi `ipc_handler_send_sync` từ chính looper đích** — hàm trả `false`
  ngay để chặn tự khóa; gọi chéo hai chiều A→B và B→A vẫn có thể deadlock cho
  đến khi hết timeout.
- **Payload heap phải đặt `payload_free`**, nếu không sẽ rò rỉ khi message bị
  purge lúc restart.
- Handler nên là biến `static`/toàn cục (không nằm trên stack task), vì hàng
  đợi giữ con trỏ tới nó qua chu kỳ chết/sống.

## Giới hạn đã biết

- `ipc_task_is_alive()` dùng `eTaskGetState()` — sau `vTaskDelete` trên task
  cấp phát động, handle là dangling. Vì vậy supervisor **chỉ** dùng heartbeat
  làm nguồn sự thật; hàm đó để chẩn đoán.
- Trên ESP32, lỗi phần cứng (LoadProhibited, stack overflow) panic cả hệ thống
  chứ không chỉ giết một task — lớp này phục hồi các kiểu chết ở mức phần mềm
  (self-delete, hang, deadlock, assert có bắt). Muốn chịu cả panic thì cần
  kết hợp với hardware watchdog + `esp_reset_reason()` để khôi phục state sau
  reboot.
- Chung address space nên một task hỏng có thể ghi đè bộ nhớ task khác —
  không có ranh giới bảo vệ như process trên Linux. Muốn chặt hơn thì bật MPU
  (FreeRTOS-MPU) và cấp vùng nhớ riêng cho từng looper.
- Port desktop **không giết được thread khác** (pthreads không có cách an toàn
  và di động để làm việc đó) — `ipc_task_delete(other)` chỉ đánh dấu chết. Vì
  vậy kịch bản "task treo bị supervisor giết" chỉ chạy thật trên MCU; trên PC
  hãy test bằng fake clock + `ipc_looper_poll()` như bộ test hiện tại.
- Danh sách timer là linked list sorted: chèn O(n), nổ O(1). Với vài chục timer
  thì không đáng kể; nếu lên hàng trăm thì thay bằng timer wheel — chỉ phải sửa
  trong `ipc_timer.c`, API giữ nguyên.
- `ipc_timer_step()` xử lý tối đa `TMR_MAX_FIRE_PER_STEP` (12) timer mỗi nhịp
  để một vòng lặp trễ không làm đóng băng engine; phần còn lại chạy nhịp sau.
- Pool message dùng chung (`IPC_MESSAGE_POOL_SIZE`, mặc định 64): một looper
  ngập message có thể làm đói cả hệ thống. Theo dõi bằng
  `ipc_message_pool_stats()`; nếu cần cách ly thì tách pool theo looper.

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

## Ứng dụng mẫu: hệ IoT trong `services/`

Một hệ thật chạy trên khung này: đọc cảm biến → lọc nhiễu → lưu xuống file →
đẩy lên server (mock, vì phòng lab không có mạng).

```
[sensor] --SENSOR_SAMPLE--> [processor] --DATA_READY--> [uploader] --> server
                                 |                          |
                                 +------DATA_READY------> [config]  (lưu file)
                                 +--ALERT--> [health] --HEALTH_STATE-->
[config] --CONFIG_CHANGED--> (sensor đổi chu kỳ lấy mẫu, uploader đổi cỡ lô)
```

Điểm cốt lõi: **`svc_uploader` không biết `svc_sensor` tồn tại.** Nó chỉ biết
chủ đề `TOPIC_DATA_READY`. Thêm cảm biến thứ hai, thêm màn hình hiển thị, hay
bỏ hẳn uploader — không file nào khác phải sửa. Toàn bộ giao kèo giữa các dịch
vụ nằm gọn trong [app_events.h](services/app_events.h); các file `svc_*.c`
không include lẫn nhau.

### Ba cơ chế đồng bộ, dùng đúng chỗ

| Cơ chế | Câu hỏi nó trả lời | Dùng cho |
|--------|--------------------|----------|
| Event bus ([ipc_event.h](include/ipc_event.h)) | "có chuyện gì xảy ra" | luồng dữ liệu, nhiều người nghe, không ai bị chặn |
| Semaphore give/take | "có đúng một việc cần làm" | đánh thức looper (dùng bên trong) |
| Event group ([ipc_event_group.h](include/ipc_event_group.h)) | "các điều kiện đã đủ chưa" | trình tự khởi động (`BIT_ALL_SERVICES`), trạng thái kéo dài (`BIT_NET_ONLINE`) |

Event group tồn tại vì semaphore không thay được: "đợi đến khi **cả** config
**và** network sẵn sàng" bằng semaphore phải đếm thủ công và rất dễ sai.

Hai chi tiết khiến mô hình này sống sót qua chết/hồi sinh:

- **Đăng ký nghe theo TÊN** (`ipc_bus_subscribe_service`) — bus phân giải
  handler tại đúng thời điểm công bố, nên dịch vụ hồi sinh với handler mới vẫn
  nhận tiếp.
- **Giá trị giữ lại** (`publish_retained`) — dịch vụ vừa sống lại lấy được
  ngay bức tranh hiện tại (trạng thái mạng, tình trạng sức khỏe) thay vì chạy
  mù đến nhịp cập nhật sau.

### Ranh giới trách nhiệm

Service chỉ **báo sự việc**; health **quyết định**. `svc_sensor` cố ý *không*
tự leo thang mức độ theo số lần hỏng liên tiếp — ngưỡng "3 lần trong 10 giây
thì restart" nằm trong bảng luật ở [svc_health.c](services/svc_health.c). Nếu
làm cả hai nơi thì ngưỡng bị chia đôi và không ai đọc code đoán được lúc nào
dịch vụ sẽ bị khởi động lại.

### Chạy trên board vs chạy trong test

`app_cfg_t.spawn_tasks` quyết định: `true` thì mỗi dịch vụ có task RTOS thật;
`false` thì chỉ tạo looper, test tự bơm nhịp bằng `app_poll_all()`. Cùng một
code, cùng đường `on_start`. Cảm biến và server đều ở sau interface trong
[drivers.h](services/drivers.h) nên test ép được lỗi đúng lúc mình muốn
("cảm biến hỏng ở mẫu thứ 5", "mất mạng trong 3 lần đẩy").

## SOLID ở đây cụ thể là gì

| Nguyên tắc | Hiện ra ở đâu |
|-----------|---------------|
| **SRP** | clock giữ giờ · timer canh giờ · looper điều phối · watchdog phát hiện · health chẩn đoán · supervisor phục hồi · config lưu trạng thái. Health không biết cách reset chip, cũng không biết cách hồi sinh task |
| **OCP** | thêm nền tảng mới = thêm một backend/port, không sửa core. `wdt_backend_esp32.c` là file mà `ipc_watchdog.c` không hề biết là có tồn tại |
| **LSP** | fake clock, fake WDT backend, fake health probe, RAM storage đều thay thế được bản thật ở mọi chỗ; `port_host.c` thay được `port_freertos.c` |
| **ISP** | `ipc_clock_t` 1 phương thức, `ipc_cfg_storage_t` 2, `ipc_wdt_backend_t` 3. Người viết backend không cần biết message hay looper là gì |
| **DIP** | core phụ thuộc `ipc_clock_t` / `ipc_wdt_backend_t` / `ipc_wdt_recovery_fn`, không phụ thuộc FreeRTOS hay ESP-IDF. Đó là lý do bộ test chạy được trên PC |

## Test trên desktop

Thời gian ảo, đơn luồng, không `sleep()`, không thread → chạy hết trong vài
micro giây và không bao giờ flaky:

```sh
# Linux / macOS
sh test/build_and_run.sh
cmake -B build && cmake --build build && ctest --test-dir build

# Windows (MinGW)
CC=/c/Qt/Tools/mingw1310_64/bin/gcc.exe sh test/build_and_run.sh
```

Đã chạy thật trên cả hai: gcc 13.3 (Ubuntu 24.04) và MinGW 13.1 (Windows).
`port_host.c` định nghĩa `_GNU_SOURCE`/`_POSIX_C_SOURCE` trước mọi `#include`
— thiếu nó thì `-std=c11` làm glibc bật `__STRICT_ANSI__` và
`PTHREAD_MUTEX_RECURSIVE`, `CLOCK_MONOTONIC`, `nanosleep` biến mất trên Linux
(MinGW không kiểm tra feature macro nên lỗi này không lộ ra trên Windows).

Công thức: `ipc_fake_clock_advance()` để đẩy giờ, `ipc_timer_step()` /
`ipc_wdt_check()` để chạy một nhịp engine không chặn, `ipc_looper_poll()` để
looper tiêu thụ message — tất cả trong cùng một luồng.

```c
ipc_fake_clock_t clk;
ipc_fake_clock_init(&clk, 1000);
ipc_clock_set(&clk.base);          // cầm kim đồng hồ

ipc_fake_clock_advance(&clk, 999);
ipc_timer_step(NULL); ipc_looper_poll(lp, 0);
CHECK_EQ(received, 0);             // trước hạn: tuyệt đối không được bắn
```

Bộ test hiện tại: **59 test** — 11 timer, 6 watchdog, 9 config, 11 health,
11 bus/event-group, 11 hành vi hệ thống. Nhóm cuối mô tả tình huống có thật
chứ không test từng hàm: "mất mạng thì dữ liệu có mất không", "processor chết
giữa chừng thì có xử lý trùng không", "cảm biến hỏng liên tục thì ai khởi động
lại nó".

Tất cả đã mutation-test để chắc chắn chúng thật sự bắt lỗi chứ không phải xanh
giả. Ví dụ: bỏ dòng hủy đăng ký cũ trong `on_start` của processor → test bắt
được ngay việc mỗi mẫu bị xử lý hai lần sau khi hồi sinh. Một mutation từng
sống sót đã được xử lý bằng cách bổ sung test, không phải bằng cách làm ngơ.

## Cấu trúc

```
include/ipc_port.h        lớp trừu tượng RTOS (đổi RTOS chỉ sửa 1 file .c)
include/ipc_clock.h       nguồn thời gian tiêm được — trục testability
include/ipc_message.h     Message + pool tĩnh
include/ipc_looper.h      Looper, MessageQueue, Handler, poll()
include/ipc_service.h     ServiceManager + linkToDeath
include/ipc_timer.h       timer one-shot / periodic / delayed message
include/ipc_watchdog.h    watchdog 2 tầng + backend interface
include/ipc_health.h      giám sát hệ thống + bảng luật xử lý sự cố
include/ipc_config.h      key-value bền vững, storage tiêm được
include/ipc_supervisor.h  giám sát và hồi sinh
src/port_freertos.c       port FreeRTOS / ESP-IDF
src/port_host.c           port desktop (pthreads), build với -DIPC_PORT_HOST
src/wdt_backend_esp32.c   backend HW WDT cho ESP-IDF
include/ipc_event.h       event bus (observer), đăng ký theo tên + retained
include/ipc_event_group.h nhóm cờ sự kiện, chờ AND/OR
services/                 ứng dụng IoT thật: sensor, processor, config,
                          uploader, health + driver và server mock
example/main.c            demo 3 dịch vụ + timer + watchdog + kịch bản crash
test/                     test host, thời gian ảo
```

Tham số biên dịch: `IPC_MESSAGE_POOL_SIZE`, `IPC_MAX_LOOPERS`,
`IPC_MAX_SERVICES`, `IPC_MAX_SYNC_TOKENS`, `IPC_MAX_TIMERS`,
`IPC_MAX_WDT_CLIENTS`, `IPC_TLS_INDEX`.
