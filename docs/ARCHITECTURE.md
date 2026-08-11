# Kiến trúc

## Bốn tầng

```
┌───────────────────────────────────────────────────────────┐
│ app/          lắp ráp: bảng service, khởi động, main       │
├───────────────────────────────────────────────────────────┤
│ services/     nghiệp vụ: sensor, processor, config,        │
│               uploader, health + driver và server mock     │
├───────────────────────────────────────────────────────────┤
│ core/         framework: looper, message, service manager, │
│               bus, event group, timer, watchdog, health,   │
│               config, supervisor, clock                    │
├───────────────────────────────────────────────────────────┤
│ port/         nền tảng: FreeRTOS hoặc pthreads (desktop)   │
└───────────────────────────────────────────────────────────┘
```

Phụ thuộc chỉ đi **xuống**. `core/` không biết `services/` tồn tại; `services/`
không biết chạy trên FreeRTOS hay desktop.

## Cây thư mục

```
core/include/       API công khai của framework (ipc_*.h)
core/src/           hiện thực
port/               port_freertos.c, port_host.c, wdt_backend_esp32.c
services/common/     app_events.h (bản hợp đồng), service_iface.h/.c (khung),
                     service_api.h/.c (một cửa ra API lõi), services.h
services/drivers/    cảm biến và server mock, đều sau interface
services/sensor/     sensorService.h/.c
services/processor/  processorService.h/.c
services/config/     configService.h/.c
services/uploader/   uploaderService.h/.c
services/health/     healthService.h/.c
app/                app.c (bảng service), app.h, main.c
docs/               tài liệu
tests/unit/         test từng module: timer, watchdog, config, health, bus
tests/behavior/     test hành vi cả hệ thống
tests/support/      khung test
examples/basic_ipc/ dùng trực tiếp looper/supervisor, không qua lớp services
scripts/run_tests.sh
```

## Ánh xạ khái niệm

Framework là bản port mô hình event-driven của Android/Linux sang RTOS:

| Linux / Android | Ở đây | Ghi chú |
|---|---|---|
| process | task | chung address space → "IPC" thành ITC, không copy payload |
| `Looper.loop()` | `ipc_looper_run()` | vòng lặp lấy message theo mốc thời gian |
| `MessageQueue` | danh sách sorted theo `(when_ms, seq)` | hỗ trợ delayed message |
| `ThreadLocal<Looper>` | `ipc_tls_get/set` | biết mình đang chạy trên looper nào |
| `Handler` + `handleMessage` | `ipc_handler_t` + `on_receive` | nhiều handler trên một looper |
| `Message.obtain()` | pool tĩnh | không malloc trong đường chạy nóng |
| Binder + `ServiceManager` | `ipc_service_register/get` | tra cứu **theo tên** |
| `linkToDeath` | `ipc_service_link_to_death` | client biết dịch vụ chết/sống lại |
| `init` respawn | `ipc_supervisor` | phát hiện + tạo lại task lúc runtime |
| `BroadcastReceiver` | `ipc_event.h` | pub/sub theo chủ đề |

## Nguyên tắc chịu lỗi

**`ipc_looper_t` là object độc lập với task.** Task chỉ là "cơ bắp" chạy vòng
lặp. Task chết → object còn nguyên → supervisor tạo task mới gắn vào **đúng
object cũ** → hàng đợi message không mất.

Xem [FLOW.md](FLOW.md) mục 4 để biết chi tiết đường chết/hồi sinh và cái gì
sống sót.

## Ba cơ chế đồng bộ

| Cơ chế | Trả lời câu hỏi | Dùng cho |
|---|---|---|
| Event bus | "có chuyện gì xảy ra" | luồng dữ liệu, nhiều listener, không ai bị chặn |
| Semaphore give/take | "có đúng một việc cần làm" | đánh thức looper (dùng bên trong) |
| Event group | "các điều kiện đã đủ chưa" | trình tự khởi động, trạng thái kéo dài |

Event group tồn tại vì semaphore không thay được: "đợi đến khi **cả** config
**và** network sẵn sàng" bằng semaphore phải đếm thủ công và rất dễ sai.

Cùng với `linkToDeath` (quan sát dịch vụ chết/sống lại), health probe và
heartbeat, hệ có năm cơ chế quan sát khác nhau — xem [OBSERVER.md](OBSERVER.md)
để biết dùng cái nào cho việc gì.

## SOLID

| Nguyên tắc | Hiện ra ở đâu |
|---|---|
| **SRP** | clock giữ giờ · timer canh giờ · looper điều phối · watchdog phát hiện · health chẩn đoán · supervisor phục hồi · config lưu trạng thái |
| **OCP** | thêm nền tảng = thêm một port; thêm service = thêm một dòng vào `k_services[]`; `wdt_backend_esp32.c` là file mà `ipc_watchdog.c` không hề biết là có tồn tại |
| **LSP** | fake clock, fake WDT backend, fake health probe, RAM storage, fake sensor, cloud mock đều thay thế được bản thật ở mọi chỗ |
| **ISP** | `ipc_clock_t` 1 phương thức, `ipc_cfg_storage_t` 2, `ipc_wdt_backend_t` 3 |
| **DIP** | core phụ thuộc interface chứ không phụ thuộc FreeRTOS/ESP-IDF — đó là lý do bộ test chạy được trên PC |

## Ranh giới trách nhiệm giữa các service

Service **báo sự việc**; health **quyết định**. `sensorService` cố ý *không* tự
leo thang mức độ theo số lần hỏng liên tiếp — ngưỡng "3 lần trong 10 giây thì
restart" nằm trong bảng luật ở `healthService.c`. Làm cả hai nơi thì ngưỡng bị
chia đôi và không ai đọc code đoán được lúc nào service bị khởi động lại.

## Giới hạn đã biết

- `ipc_task_is_alive()` dùng `eTaskGetState()` — sau `vTaskDelete` trên task
  cấp phát động, handle là dangling. Supervisor **chỉ** dùng heartbeat làm
  nguồn sự thật; hàm đó để chẩn đoán.
- Trên ESP32, lỗi phần cứng (LoadProhibited, stack overflow) panic cả hệ thống
  chứ không chỉ giết một task. Lớp này phục hồi các kiểu chết ở mức phần mềm.
  Muốn chịu cả panic thì cần HW watchdog + `esp_reset_reason()` để khôi phục
  state sau reboot.
- Chung address space nên một task hỏng có thể ghi đè bộ nhớ task khác — không
  có ranh giới bảo vệ như process trên Linux. Muốn chặt hơn thì bật FreeRTOS-MPU.
- Port desktop **không giết được thread khác** (pthreads không có cách an toàn
  và di động). `ipc_task_delete(other)` chỉ đánh dấu chết. Kịch bản "task treo
  bị supervisor giết" chỉ chạy thật trên MCU.
- Danh sách timer là linked list sorted: chèn O(n), nổ O(1). Vài chục timer thì
  không đáng kể; lên hàng trăm thì thay bằng timer wheel — chỉ sửa trong
  `ipc_timer.c`, API giữ nguyên.
- Pool message dùng chung (`IPC_MESSAGE_POOL_SIZE`, mặc định 64): một looper
  ngập message có thể làm đói cả hệ thống. Theo dõi bằng
  `ipc_message_pool_stats()`.

## Tham số biên dịch

`IPC_MESSAGE_POOL_SIZE`, `IPC_MAX_LOOPERS`, `IPC_MAX_SERVICES`,
`IPC_MAX_SYNC_TOKENS`, `IPC_MAX_TIMERS`, `IPC_MAX_WDT_CLIENTS`,
`IPC_MAX_SUBSCRIPTIONS`, `IPC_MAX_RETAINED_TOPICS`, `IPC_CFG_MAX_ENTRIES`,
`IPC_HEALTH_MAX_RULES`, `IPC_TLS_INDEX`.
