# Luồng chạy

Tài liệu này mô tả **chuyện gì xảy ra, theo thứ tự nào, trên task nào**.

Cơ chế quan sát đứng sau các mũi tên ở đây được mô tả riêng trong
[OBSERVER.md](OBSERVER.md).

Ký hiệu: mỗi bước ghi rõ chạy trên context nào — `[task X]` là task của service
X, `[timer]` là task timer engine, `[caller]` là task của người gọi.

---

## 1. Khởi động

`app_start()` chạy trên task gọi nó (`app_main` trên ESP32).

```
[caller] app_start()
  1. ipc_message_pool_init()      pool message tĩnh
  2. ipc_service_manager_init()   bảng tra cứu tên → handler
  3. ipc_bus_init()               event bus
  4. ipc_event_group_create()     nhóm cờ BIT_*
  5. ipc_timer_engine_start()     ── phải trước sensor, vì sensor đặt nhịp
                                     lấy mẫu ngay trong on_create
  6. với mỗi service trong bảng k_services[]:
         app_service_start(svc, &cfg, spawn)
  7. start_supervision()          supervisor + watchdog
  8. chờ BIT_ALL_SERVICES (tối đa 5s)
```

**Thứ tự trong `k_services[]` là thứ tự phụ thuộc, không phải ngẫu nhiên:**

| # | Service | Vì sao ở vị trí này |
|---|---------|---------------------|
| 1 | config | mọi service khác đọc cấu hình của nó trong `on_create` |
| 2 | health | để bắt được sự cố ngay từ lúc khởi động |
| 3 | processor | người tiêu thụ phải sẵn sàng trước người sản xuất |
| 4 | uploader | nghe `DATA_READY` do processor phát |
| 5 | sensor | **cuối cùng** — nó là người bắt đầu sinh dữ liệu |

Đảo ngược thì những mẫu đầu tiên rơi vào khoảng trống: bus không có người
nghe thì sự kiện biến mất không dấu vết.

### Bên trong `app_service_start()`

```
[caller]  tạo looper mang tên service
          tạo task (nếu spawn_tasks)
             │
[task svc]   └─> service_on_start()          ← khung gọi, KHÔNG phải service
                  1. ipc_handler_init + ipc_service_register(name)
                  2. ipc_bus_unsubscribe_service(name)   ← xem mục 4
                  3. svc->on_create(svc)
                  4. svc->on_subscribe(svc)
                  5. ipc_event_group_set(ready_bit)
                  └─> ipc_looper_run()  ← vòng lặp, không bao giờ trả về
```

---

## 2. Một mẫu chạy hết đường ống

```mermaid
sequenceDiagram
    participant T as timer engine
    participant S as task sensor
    participant P as task processor
    participant U as task uploader
    participant C as task config
    participant Cloud as server (mock)

    T->>S: MSG_SENSOR_TICK (timer periodic, TO_SERVICE)
    S->>S: drv->read() → 25000 mC
    S-->>P: publish TOPIC_SENSOR_SAMPLE
    Note over S,P: bus chỉ đẩy message vào hàng đợi<br/>sensor không chờ ai
    P->>P: trung bình trượt cửa sổ 4
    P-->>U: publish TOPIC_DATA_READY
    P-->>C: publish TOPIC_DATA_READY
    C->>C: ipc_cfg_set_int(LAST_VALUE) → hẹn ghi file sau 500ms
    U->>U: queue_push(); đủ lô chưa?
    U->>Cloud: upload("dev=tirex-1;25000;...")
    U-->>P: publish TOPIC_UPLOAD_RESULT
```

Chi tiết từng bước và context:

| # | Context | Việc |
|---|---------|------|
| 1 | `[timer]` | timer đến hạn, phân giải `SVC_SENSOR` **theo tên** → gửi `MSG_SENSOR_TICK` |
| 2 | `[task sensor]` | khung tra bảng định tuyến → `on_tick()` → `drv->read()` → `svc_publish_topic(self, TOPIC_SENSOR_SAMPLE, ...)` |
| 3 | `[task sensor]` | bus tra người nghe, `ipc_message_obtain()`, đẩy vào hàng đợi processor rồi **trả về ngay** |
| 4 | `[task processor]` | `on_sensor_sample()` → trung bình trượt → publish `TOPIC_DATA_READY` (2 người nghe) |
| 5 | `[task processor]` | kiểm ngưỡng; vượt thì publish `TOPIC_ALERT` |
| 6 | `[task config]` | lưu `LAST_VALUE`; **không ghi file ngay** — debounce 500ms |
| 7 | `[task uploader]` | `on_data_ready()` → đẩy vào hàng đợi; đủ `upload.batch` thì `flush()` |
| 8 | `[timer]` | 500ms sau lần set cuối: gửi `CFG_MSG_SAVE` tới looper config |
| 9 | `[task config]` | ghi file: bản tạm → `rename()` |

**Điểm quan trọng**: publish **không** chạy callback của người nghe. Nó chỉ đẩy
message vào hàng đợi của looper đích. Nên sensor không bao giờ bị uploader chậm
làm nghẽn, dù uploader đang chờ mạng.

---

## 3. Đổi cấu hình lúc đang chạy

```
[bất kỳ] app_service_set(SVC_CONFIG, CFGK_PERIOD_MS, 300)
   └─> ipc_cfg_set_int("sample.period_ms", 300)
         ├─> đánh dấu dirty + hẹn giờ ghi file (debounce)
         └─> on_cfg_write() → svc_publish_topic(self, TOPIC_CONFIG_CHANGED, CFGK_PERIOD_MS, 300)
               └─> [task sensor] on_receive
                     └─> arm_sampling_timer(self)
                           ├─> svc_timer_stop_all(self)                   ← hủy nhịp cũ
                           └─> svc_timer_every(self, MSG_SENSOR_TICK, 300)
```

Không service nào bị khởi động lại. `configService` không biết `sensorService` tồn
tại — nó chỉ công bố một sự kiện.

---

## 4. Service chết và hồi sinh

Hai đường, tùy vì sao chết:

### 4a. Task chết hoặc treo → supervisor phát hiện

```
[task X]      chết (self-delete / treo trong handler / deadlock)
              → heartbeat ngừng cập nhật
[supervisor]  quét mỗi 250ms → ipc_looper_check_alive() = false
              → ipc_service_notify_state(lp, false)   (linkToDeath)
              → ipc_looper_revive():
                   1. giết xác cũ nếu còn (trường hợp treo)
                   2. thu hồi message đang dispatch → không rò rỉ pool
                   3. tạo lại mutex nếu nó chết cùng chủ sở hữu
                   4. HÀNG ĐỢI GIỮ NGUYÊN
                   5. tạo task mới gắn vào ĐÚNG object looper cũ
[task X mới]  service_on_start() chạy lại từ đầu (mục 1)
              → generation++
```

### 4b. Lỗi nghiệp vụ lặp lại → health quyết định

```
[task sensor] đọc hỏng → svc_report_warn(self, EXC_SENSOR_READ, streak)
                          → chỉ ghi vào vòng đệm, trả về ngay (gọi được từ ISR)
[task health] ipc_health_check() mỗi 500ms:
                 rút vòng đệm → duyệt bảng luật
                 luật {EXC_SENSOR_READ, sensor, WARN, 3 lần / 10s} → đủ ngưỡng
                 → IPC_ACT_RESTART_SERVICE → recover() → supervisor
```

**Ai chịu trách nhiệm gì**: `sensorService` chỉ *báo sự việc*. Ngưỡng "3 lần trong
10 giây" nằm trong bảng luật ở `healthService.c`. Làm cả hai nơi thì ngưỡng bị
chia đôi và không ai đọc code đoán được lúc nào service bị khởi động lại.

### Cái gì sống sót qua hồi sinh, cái gì không

| Giữ nguyên | Vì sao |
|---|---|
| Hàng đợi message của looper | công việc chưa làm không mất theo task |
| Hàng đợi dữ liệu chưa đẩy của uploader | dữ liệu là tài sản |
| Cửa sổ lọc của processor | dữ liệu đo vẫn còn hiệu lực; xóa đi sẽ tạo bước nhảy giả |
| Đăng ký nghe **theo tên** trên bus | bus phân giải handler lúc công bố, không giữ con trỏ |
| Timer đặt kiểu `TO_SERVICE` | cùng lý do |

| Làm lại | Vì sao |
|---|---|
| Handler (con trỏ mới) | thuộc về vòng đời mới |
| Đăng ký nghe cũ trên bus | **khung tự hủy trước khi `on_subscribe`** |
| Nhịp timer của service | `on_create` hủy nhịp cũ rồi đặt lại |

Bước tự hủy đăng ký ở khung là điểm dễ sai nhất: nếu service tự làm và quên,
sau mỗi lần hồi sinh nó nhận **mọi sự kiện hai lần**. Có test riêng bắt lỗi này
(`test_processor_chet_roi_hoi_sinh_khong_xu_ly_trung`).

---

## 5. Mất mạng

```
[task uploader] flush() → cloud->is_online() = false
                  └─> schedule_retry()  (500ms → 1s → 2s → ... tối đa 8s)
                      dữ liệu VẪN nằm trong hàng đợi

hàng đợi đầy (16 bản ghi):
                  └─> bỏ bản ghi CŨ NHẤT + svc_report_warn(self, EXC_QUEUE_OVERFLOW, n)
                      (trong đo lường, dữ liệu mới có giá trị hơn dữ liệu cũ)

có mạng lại:
[bất kỳ] app_service_set(SVC_UPLOADER, UPK_ONLINE, 1)
           └─> publish TOPIC_NET_STATE(1)
                 └─> [task uploader] set BIT_NET_ONLINE + flush() ngay
```

Không có đường nào làm mất dữ liệu mà im lặng: mọi lần mất đều được đếm
(`UPK_DROPPED`) và báo lên health.

---

## 6. Bốn tầng phát hiện lỗi

Ngưỡng nới rộng dần nên các tầng không giẫm chân nhau:

```
service tự báo lỗi   →  health (ngưỡng theo cửa sổ)   →  restart / kill / reboot
looper heartbeat     →  supervisor (heartbeat_timeout) →  tạo lại task
                     →  watchdog (2× ngưỡng trên)      →  restart, rồi reset
                     →  HW watchdog (chỉ được cho ăn khi mọi client khỏe)
```

Tầng cuối trả lời câu "ai canh người gác đền": nếu chính task watchdog chết,
HW WDT không được cho ăn → chip tự reset.

---

## 7. Chạy trong test

`spawn_tasks = false`: không tạo task nào, test tự bơm nhịp. Cùng một code,
cùng đường `on_create`/`on_subscribe`/`on_receive`.

```c
static void tick(uint32_t ms) {
    ipc_fake_clock_advance(&g_clock, ms);  // đẩy thời gian ảo
    ipc_timer_step(NULL);                  // timer đến hạn thì bắn
    app_poll_all();                        // các looper tiêu thụ message
    ipc_health_check();                    // health đánh giá
    app_poll_all();                        // hệ quả của health
}
```

`app_poll_all()` lặp đến khi hệ lặng hẳn, vì một sự kiện đẻ ra sự kiện khác
(sensor → processor → uploader). Chặn trên 32 vòng để một vòng lặp sự kiện vô
tận không treo bộ test.
