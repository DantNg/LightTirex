# Khuôn API của service

Mọi service đều theo cùng một khuôn: khai báo một bảng `app_service_t` và điền
vào các móc đời. Khung lo phần lặp lại.

## Bảng mô tả

```c
static app_service_t s_svc = {
    /* khai báo */
    .name                 = SVC_SENSOR,      // trùng tên đăng ký ServiceManager
    .priority             = 7,
    .stack_words          = 0,               // 0 → mặc định 3072
    .heartbeat_timeout_ms = 4000,            // 0 → tắt giám sát
    .ready_bit            = BIT_SENSOR_READY,

    /* móc đời */
    .on_create    = sensor_on_create,        // khởi tạo lại state, cắm driver
    .on_subscribe = sensor_on_subscribe,     // đăng ký nghe bus
    .on_receive   = sensor_on_receive,       // xử lý message
    .on_destroy   = NULL,                    // tùy chọn

    /* đọc/ghi tham số */
    .get = sensor_get,
    .set = sensor_set,
};

app_service_t *svc_sensor(void) { return &s_svc; }
```

Thêm service mới = viết một file theo khuôn này, khai báo `svc_xxx()` trong
[services.h](../services/include/services.h), thêm một dòng vào bảng
`k_services[]` trong [app.c](../app/app.c). Không sửa gì khác.

## Các móc đời

| Móc | Khi nào chạy | Dùng để |
|-----|--------------|---------|
| `on_create` | trong task, mỗi lần khởi động **và mỗi lần hồi sinh** | dựng lại state, cắm driver, đặt nhịp timer |
| `on_subscribe` | ngay sau `on_create` | `ipc_bus_subscribe_service(...)` |
| `on_receive` | mỗi message tới | xử lý; trả `true` nếu đã xong |
| `on_destroy` | khi bị dừng hẳn | dọn tài nguyên ngoài (tùy chọn) |

### Khung làm gì trước khi gọi bạn

```
1. ipc_handler_init + ipc_service_register(name)   ← khung
2. ipc_bus_unsubscribe_service(name)               ← khung
3. on_create()                                      ← bạn
4. on_subscribe()                                   ← bạn
5. ipc_event_group_set(ready_bit)                  ← khung
```

**Bước 2 là lý do chính khung tồn tại.** Nếu service tự đăng ký nghe mà quên
hủy đăng ký cũ, thì sau mỗi lần hồi sinh nó nhận **mọi sự kiện hai lần** — lỗi
rất khó thấy khi chạy thật. Khung làm việc đó, không ai quên được.

### Quy tắc viết móc đời

- **Đừng đăng ký nghe trong `on_create`** — để `on_subscribe` làm, để thứ tự
  luôn đúng.
- **`on_create` phải chịu được gọi nhiều lần.** Nó *sẽ* chạy lại sau mỗi lần
  hồi sinh. State nào cần giữ qua chu kỳ chết/sống thì để ở biến `static`
  ngoài, đừng khởi tạo lại trong này (ví dụ: hàng đợi chưa đẩy của uploader,
  cửa sổ lọc của processor).
- **`on_receive` không được block lâu.** Vượt `heartbeat_timeout_ms` thì
  supervisor coi như treo và giết task.

## `get` / `set`

Một chữ ký chung cho mọi service, thay cho một rừng hàm `svc_x_peek_y()`:

```c
int32_t (*get)(app_service_t *self, uint32_t key, int32_t def);
bool    (*set)(app_service_t *self, uint32_t key, int32_t value);
```

Gọi trực tiếp, hoặc theo tên từ bất kỳ đâu:

```c
int32_t pending  = app_service_get(SVC_UPLOADER, UPK_PENDING, 0);
bool    ok       = app_service_set(SVC_CONFIG, CFGK_PERIOD_MS, 300);
```

Quy ước:

- `get` trả `SVC_VALUE_INVALID` (= `INT32_MIN`) nếu **không biết khóa**. Nhờ
  vậy phân biệt được với giá trị thật bằng 0.
- `set` trả `false` nếu khóa không hợp lệ **hoặc giá trị bị từ chối**.
- Khóa chỉ đọc thì `set` trả `false`. Ví dụ `SENSORK_SAMPLES` là số liệu, không
  ai được ghi đè.
- Khóa của mỗi service khai báo trong
  [app_events.h](../services/include/app_events.h), mỗi service một không gian
  khóa riêng bắt đầu từ 1.

### Vì sao `set` đôi khi không sửa state trực tiếp

`svc_uploader.set(UPK_ONLINE, 1)` không gán thẳng biến trong uploader — nó
publish `TOPIC_NET_STATE`. Lý do: "có mạng lại" là sự kiện của cả hệ thống, ai
quan tâm cũng cần biết, không riêng uploader. Sửa lút state bên trong thì
những người nghe khác không hay biết gì.

Tương tự, `svc_sensor.set()` luôn trả `false` cho chu kỳ lấy mẫu: đó là cấu
hình của hệ thống, phải đi qua `svc_config` để mọi người cùng biết và để giá
trị được lưu xuống file.

## Bản hợp đồng chung

Tất cả những gì các service dùng để nói chuyện với nhau nằm trong
[app_events.h](../services/include/app_events.h): tên service, chủ đề sự kiện,
khóa `get`/`set`, cờ đồng bộ, mã exception.

Các file `svc_*.c` **không include lẫn nhau**. Đó là thứ giữ cho hệ thống tháo
lắp được: `svc_uploader` không biết `svc_sensor` tồn tại, nó chỉ biết chủ đề
`TOPIC_DATA_READY`.
