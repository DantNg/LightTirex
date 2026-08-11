# lightTirex

Framework IPC event-driven cho RTOS, kèm một hệ IoT hoàn chỉnh chạy trên nó.

Port mô hình `Looper` / `Handler` / `Message` / `ServiceManager` của
Android/Linux sang môi trường RTOS đơn-không-gian-địa-chỉ, với **khả năng hồi
sinh task chết lúc runtime**. Chạy trên ESP-IDF/FreeRTOS, và chạy được nguyên
vẹn trên desktop để test.

## Tài liệu

| Tài liệu | Nội dung |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | bốn tầng, cây thư mục, ánh xạ khái niệm, SOLID, giới hạn |
| [docs/FLOW.md](docs/FLOW.md) | **luồng chạy cụ thể**: khởi động, một mẫu chạy hết đường ống, đổi cấu hình, chết/hồi sinh, mất mạng |
| [docs/OBSERVER.md](docs/OBSERVER.md) | **năm cơ chế quan sát**: event bus, linkToDeath, event group, health, heartbeat |
| [docs/SERVICE_API.md](docs/SERVICE_API.md) | khuôn API chung của service: móc đời, `get`/`set` |
| [docs/MODULES.md](docs/MODULES.md) | chi tiết timer, watchdog, health, config |

## Cây thư mục

```
core/       framework độc lập nền tảng  (include/ + src/)
port/       nền tảng: FreeRTOS hoặc pthreads
services/   hệ IoT: sensor, processor, config, uploader, health
app/        lắp ráp (bảng service) + main
docs/       tài liệu
tests/      unit/ + behavior/ + support/
examples/   dùng trực tiếp looper/supervisor
scripts/    run_tests.sh
```

Phụ thuộc chỉ đi xuống: `core/` không biết `services/` tồn tại, `services/`
không biết chạy trên FreeRTOS hay desktop.

## Chạy thử

```sh
# Linux / macOS
sh scripts/run_tests.sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/app_demo          # hệ IoT chạy thật với task thật

# Windows (MinGW)
CC=/c/Qt/Tools/mingw1310_64/bin/gcc.exe sh scripts/run_tests.sh
```

Đã chạy thật trên gcc 13.3 (Ubuntu 24.04) và MinGW 13.1 (Windows).

## Hệ IoT mẫu

Đọc cảm biến → lọc nhiễu → lưu xuống file → đẩy lên server (mock, vì phòng lab
không có mạng).

```
[sensor] --SENSOR_SAMPLE--> [processor] --DATA_READY--> [uploader] --> server
                                 |                          |
                                 +------DATA_READY------> [config]  (lưu file)
                                 +--ALERT--> [health] --HEALTH_STATE-->
[config] --CONFIG_CHANGED--> (sensor đổi chu kỳ lấy mẫu, uploader đổi cỡ lô)
```

**`svc_uploader` không biết `svc_sensor` tồn tại** — nó chỉ biết chủ đề
`TOPIC_DATA_READY`. Thêm cảm biến thứ hai, thêm màn hình hiển thị, hay bỏ hẳn
uploader đều không đụng file nào khác. Toàn bộ giao kèo nằm trong
[app_events.h](services/include/app_events.h); các file `svc_*.c` không include
lẫn nhau.

Toàn bộ danh sách service nằm trong một bảng duy nhất
([app.c](app/app.c)) — thêm service là thêm một dòng:

```c
static service_factory_fn const k_services[] = {
    svc_config,     // trước tiên: mọi service khác đọc cấu hình của nó
    svc_health,     // thứ hai: bắt được sự cố ngay từ lúc khởi động
    svc_processor,  // người tiêu thụ sẵn sàng trước
    svc_uploader,
    svc_sensor,     // cuối cùng: nó là người bắt đầu sinh dữ liệu
};
```

Mỗi service khai báo một bảng `app_service_t` với móc đời thống nhất
(`on_create` / `on_subscribe` / bảng định tuyến `routes[]`) và một cặp
`get`/`set` chung —
xem [SERVICE_API.md](docs/SERVICE_API.md).

```c
int32_t pending = app_service_get(SVC_UPLOADER, UPK_PENDING, 0);
bool    ok      = app_service_set(SVC_CONFIG, CFGK_PERIOD_MS, 300);
```

## Chịu lỗi

**`ipc_looper_t` là object độc lập với task.** Task chỉ là "cơ bắp" chạy vòng
lặp. Task chết → object còn nguyên → supervisor tạo task mới gắn vào **đúng
object cũ** → hàng đợi message không mất, `generation++`, `on_create` chạy lại.

Bốn tầng phát hiện lỗi, ngưỡng nới rộng dần nên không giẫm chân nhau:

```
service báo lỗi → health (ngưỡng theo cửa sổ) → restart / kill / reboot
looper heartbeat → supervisor → tạo lại task → watchdog (2×) → HW WDT
```

Chi tiết từng đường chết và cái gì sống sót qua hồi sinh:
[FLOW.md mục 4](docs/FLOW.md).

## Test

**61 test** chạy trên desktop với thời gian ảo, đơn luồng, không `sleep()`,
không thread, không phần cứng — hết trong vài mili giây và không bao giờ flaky.

```
tests/unit/       timer (11), watchdog (6), config (9), health (11), bus (12)
tests/behavior/   hành vi cả hệ thống (12)
```

Nhóm `behavior/` mô tả tình huống có thật chứ không test từng hàm: *"mất mạng
thì dữ liệu có mất không"*, *"processor chết giữa chừng có xử lý trùng không"*,
*"cảm biến hỏng liên tục thì ai khởi động lại nó"*.

Tất cả đã mutation-test để chắc chắn chúng thật sự bắt lỗi chứ không phải xanh
giả. Ví dụ: bỏ dòng hủy đăng ký cũ trong khuôn khởi động service → test bắt
được ngay việc mỗi mẫu bị xử lý hai lần sau khi hồi sinh.
