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
    .on_destroy   = NULL,                    // tùy chọn

    /* bảng định tuyến: message nào thì ai xử lý */
    .routes       = k_routes,
    .route_count  = sizeof(k_routes) / sizeof(k_routes[0]),

    /* đọc/ghi tham số */
    .get = sensor_get,
    .set = sensor_set,
};

app_service_t *sensorService(void) { return &s_svc; }
```

Thêm service mới:

1. `services/<tên>/<tên>Service.{h,c}` theo khuôn này — mỗi service một thư mục
2. thêm một dòng `#include` vào [common/services.h](../services/common/services.h)
3. thêm một dòng vào bảng `k_services[]` trong [app.c](../app/app.c)

Không sửa gì khác. Header của service chỉ lộ ra đúng một hàm:

```c
app_service_t *sensorService(void);
```

Mọi thứ khác — nhận message, đọc/ghi tham số, khởi tạo lại — đi qua khuôn chung,
nên một service không có cách nào lộ API riêng lẻ ra ngoài.

## Các móc đời

| Móc | Khi nào chạy | Dùng để |
|-----|--------------|---------|
| `on_create` | trong task, mỗi lần khởi động **và mỗi lần hồi sinh** | dựng lại state, cắm driver, đặt nhịp timer |
| `on_subscribe` | ngay sau `on_create` | `svc_listen_topic(self, ...)` |
| `routes[]` | mỗi message tới | định tuyến theo `what` → hàm xử lý riêng |
| `on_receive` | chỉ khi **không** route nào khớp | dự phòng; để `NULL` nếu không cần |
| `on_destroy` | khi bị dừng hẳn | dọn tài nguyên ngoài (tùy chọn) |

## Bảng định tuyến

Thay cho một `switch` dài, mỗi loại message có một hàm riêng có tên nói rõ nó
làm gì:

```c
static bool on_data_ready(app_service_t *self, ipc_message_t *msg) {
    queue_push(msg->arg1);
    if (du_lo()) flush();
    return true;
}

static const svc_route_t k_routes[] = {
    { MSG_EV_DATA_READY, on_data_ready, "data_ready" },
    { MSG_EV_NET_STATE,  on_net_state,  "net_state"  },
    { MSG_UPLOAD_RETRY,  on_flush,      "retry"      },
    { MSG_UPLOAD_FLUSH,  on_flush,      "flush"      },
};
```

Lợi ích không chỉ là thẩm mỹ:

- Nhìn bảng là biết service này nhận những gì — không phải đọc hết thân hàm.
- Thêm một loại message = thêm một dòng, không sửa hàm đang có.
- **Message không ai nhận được đếm vào `svc->unhandled`** thay vì bị nuốt im
  lặng. Message lạ gần như luôn là dấu hiệu đăng ký bus sai mã `what` — trước
  đây nhánh `default:` của `switch` nuốt nó và lỗi này rất khó thấy. Xem
  `app_dump()` để đọc `handled`/`unhandled` từng service.

`on_receive` vẫn còn, nhưng chỉ chạy khi không route nào khớp. Để `NULL` nếu
service chỉ nhận đúng những gì đã khai báo.

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
- **Hàm xử lý không được block lâu.** Vượt `heartbeat_timeout_ms` thì
  supervisor coi như treo, giết task, và message đang xử lý bị hủy. Việc dài
  thì chia nhỏ rồi tự gửi lại cho mình (`ipc_handler_send_empty(&self->handler,
  MSG_JOB_STEP)`) — vừa giữ nhịp tim, vừa để request khác xen vào.

## Một cửa duy nhất ra thế giới bên ngoài

Service **không gọi thẳng API lõi**. Mọi thứ đi qua các hàm `svc_*` trong
[common/service_api.h](../services/common/service_api.h), hàm nào cũng nhận
`self` làm tham số đầu:

| Việc cần làm | Hàm | Thay cho |
|---|---|---|
| công bố sự kiện | `svc_publish_topic(self, topic, a1, a2)` | `ipc_bus_publish()` |
| công bố **trạng thái** (giữ lại giá trị cuối) | `svc_publish_state(self, topic, a1, a2)` | `ipc_bus_publish_retained()` |
| đăng ký nghe | `svc_listen_topic(self, topic, as_what)` | `ipc_bus_subscribe_service()` |
| báo sự việc hỏng | `svc_report_warn(self, code, detail)` | `ipc_health_report()` |
| hỏng nặng | `svc_report_error(self, code, detail)` | `ipc_health_report()` |
| hẹn một lần | `svc_timer_after(self, what, arg1, ms)` | `ipc_timer_send_delayed()` |
| nhịp lặp lại | `svc_timer_every(self, what, ms)` | `ipc_timer_send_periodic_to()` |
| hủy hẹn | `svc_timer_stop(self, id)` / `svc_timer_stop_all(self)` | `ipc_timer_destroy()` |
| cờ trạng thái | `svc_flag_set/clear(self, bits)` | `ipc_event_group_set/clear()` |

Vì sao thêm một lớp mỏng như vậy:

- **Danh tính không viết tay nữa.** Trước đây `sensorService` viết
  `ipc_health_report(SVC_SENSOR, ...)`. Sao chép một hàm sang service khác mà
  quên đổi hằng số tên → báo cáo hỏng **dưới tên người khác**, trình biên dịch
  không báo gì. Bây giờ nguồn lấy từ `self->name`, không sai được.
- **Đọc một file service là thấy đúng một tầng.** Trước đây mỗi service phải
  `#include` bốn header lõi (`ipc_event.h`, `ipc_health.h`, `ipc_timer.h`,
  `ipc_service.h`). Bây giờ chỉ `common/services.h`.
- **Muốn thêm gì cho mọi lần công bố** — đếm, in trace, chặn vòng lặp sự kiện —
  sửa **đúng một chỗ**: `service_api.c`.

Ngoại lệ duy nhất là `healthService`: nó *chính là* nơi đặt chính sách sức khỏe
và là cầu nối sang supervisor, nên nó được phép dùng `ipc_health.h` và
`ipc_supervisor.h`. Các service khác chỉ báo sự việc.

### Ranh giới `svc_report_*`

```
Service báo SỰ VIỆC.        "lần đọc thứ 3 liên tiếp thất bại"
healthService quyết ĐỊNH.   "3 lần trong 10 giây → khởi động lại"
```

Service **không** tự leo thang mức độ theo số lần hỏng, **không** tự gọi
restart. Làm cả hai nơi thì ngưỡng bị chia đôi: service đợi 3 lần mới báo
`ERROR`, health đợi 3 `ERROR` mới xử lý → thực tế phải hỏng **9 lần**, mà đọc
code ở cả hai file cũng không đoán ra con số đó.

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
  [common/app_events.h](../services/common/app_events.h), mỗi service một không gian
  khóa riêng bắt đầu từ 1.

### Vì sao `set` đôi khi không sửa state trực tiếp

`uploaderService.set(UPK_ONLINE, 1)` không gán thẳng biến trong uploader — nó
publish `TOPIC_NET_STATE`. Lý do: "có mạng lại" là sự kiện của cả hệ thống, ai
quan tâm cũng cần biết, không riêng uploader. Sửa lút state bên trong thì
những người nghe khác không hay biết gì.

Tương tự, `sensorService.set()` luôn trả `false` cho chu kỳ lấy mẫu: đó là cấu
hình của hệ thống, phải đi qua `configService` để mọi người cùng biết và để giá
trị được lưu xuống file.

## Bản hợp đồng chung

Tất cả những gì các service dùng để nói chuyện với nhau nằm trong
[common/app_events.h](../services/common/app_events.h): tên service, chủ đề sự kiện,
khóa `get`/`set`, cờ đồng bộ, mã exception.

Các file service **không include lẫn nhau**. Đó là thứ giữ cho hệ thống tháo
lắp được: `uploaderService` không biết `sensorService` tồn tại, nó chỉ biết chủ đề
`TOPIC_DATA_READY`.
