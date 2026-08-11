# Quá trình observe trong hệ thống

Hệ này có **năm** cơ chế quan sát khác nhau, mỗi cơ chế trả lời một loại câu
hỏi. Dùng nhầm cơ chế là nguồn lỗi phổ biến nhất, nên tài liệu này bắt đầu
bằng bảng chọn.

| Quan sát cái gì | Cơ chế | Người quan sát biết bằng cách nào |
|---|---|---|
| Sự kiện / dòng dữ liệu | **event bus** | nhận message vào hàng đợi của mình |
| Dịch vụ chết hay sống lại | **linkToDeath** | callback trên task supervisor |
| Một tập điều kiện đã đủ chưa | **event group** | thức dậy khi cờ đủ |
| Sức khỏe & tài nguyên | **health probe + luật** | quét định kỳ, so ngưỡng |
| Task còn sống không | **heartbeat** | supervisor/watchdog đọc mốc thời gian |

Ba cái đầu là *observer chủ động* — người bị quan sát tự báo. Hai cái sau là
*observer thụ động* — người quan sát tự đi đọc, vì đối tượng bị quan sát có
thể đã chết và không còn báo được gì.

---

## 1. Event bus — quan sát sự kiện

### Vì sao không dùng gửi thẳng

`ipc_service_send("uploader", ...)` là giao tiếp **điểm-đến-điểm**: bên gửi
phải biết tên bên nhận. Thêm một người nghe mới thì phải sửa code bên gửi.

Bus đảo ngược phụ thuộc: bên công bố chỉ nói *"có dữ liệu mới"*, ai quan tâm
thì tự đăng ký. `processorService` không có một dòng nào nhắc đến `uploaderService`
hay `configService` — nhưng cả hai đều nhận được `TOPIC_DATA_READY`.

### Ba vai

```
   người công bố            bus                 người quan sát
   (publisher)         (điều phối)              (observer/listener)
        │                   │                          │
        │ publish(topic,…)  │                          │
        ├──────────────────>│                          │
        │                   │ tra bảng đăng ký         │
        │                   │ obtain message           │
        │                   │ đẩy vào hàng đợi ────────>│  (không gọi callback!)
        │<──────────────────┤                          │
        │  trả về ngay      │                          │ [task riêng]
        │                   │                          │ on_receive(msg)
```

Tên hàm trong code cố ý phản ánh điều này: hàm giao việc tên là
`enqueue_to_subscriber()` chứ không phải `deliver()` hay `notify()` — nó đẩy
message vào hàng đợi rồi trả về, không chạy code của ai cả.

**Điểm cốt lõi: bus không chạy callback của người quan sát.** Nó chuyển sự
kiện thành `ipc_message_t` rồi đẩy vào **hàng đợi của looper đích**, xong trả
về. Người quan sát xử lý sau, trên task của chính nó.

Hệ quả: `sensorService` không bao giờ bị `uploaderService` làm nghẽn, kể cả khi
uploader đang chờ mạng hàng giây. Nếu bus gọi callback trực tiếp thì cả dây
chuyền sẽ chạy trên task sensor và một người nghe chậm sẽ khóa nhịp lấy mẫu.

### Đăng ký: hai kiểu, chọn đúng

```c
// Kiểu 1 — theo con trỏ handler
ipc_bus_subscribe(TOPIC_DATA_READY, &my_handler, MSG_EV_DATA_READY);

// Kiểu 2 — theo TÊN dịch vụ
ipc_bus_subscribe_service(TOPIC_DATA_READY, SVC_UPLOADER, MSG_EV_DATA_READY);
```

| | Theo con trỏ | Theo tên |
|---|---|---|
| Phân giải đích | lúc **đăng ký** | lúc **công bố** |
| Dịch vụ hồi sinh | con trỏ cũ → **mất sự kiện** | vẫn nhận bình thường |
| Chi phí mỗi lần giao | không | một lần tra bảng tên |
| Dùng cho | listener không bao giờ restart (console, UI cục bộ) | **mọi dịch vụ** |

Trong hệ IoT, tất cả service đều đăng ký **theo tên**. Xem
[enqueue_to_subscriber() — ipc_event.c:208](../core/src/ipc_event.c#L208): handler được phân
giải ngay trước khi giao, nên một dịch vụ vừa được hồi sinh với handler mới
vẫn nhận đúng sự kiện đó.

`what` là mã message do **người quan sát tự chọn** — nó quyết định nhánh nào
trong `on_receive` của mình sẽ chạy. Cùng một topic có thể tới hai listener
với hai `what` khác nhau. Bus cũng điền `msg->topic` để listener đăng ký nhiều
topic vẫn phân biệt được nguồn.

### Cơ chế giao: chụp rồi mới giao

```c
// ipc_bus_publish_ev()
lock(bus);
   chụp danh sách người nghe của topic vào mảng cục bộ
unlock(bus);                       // ← thả khóa TRƯỚC khi giao
for (mỗi người nghe trong bản chụp)
   enqueue_to_subscriber() → ipc_service_get() → ipc_message_obtain()
                           → ipc_handler_send()
```

Vì sao phải thả khóa trước: `enqueue_to_subscriber()` chạm vào **mutex của
looper khác** và
vào ServiceManager. Giữ khóa bus trong lúc đó tạo ra thứ tự khóa
`bus → looper`, mà một luồng khác đang giữ khóa looper rồi gọi
`subscribe()` sẽ tạo thứ tự ngược `looper → bus` — kinh điển của deadlock.
Chụp-rồi-thả cắt đứt khả năng đó.

Đánh đổi phải biết: một người quan sát hủy đăng ký **ngay sau** khi bản chụp
được lấy vẫn nhận thêm đúng một sự kiện cuối. Với hệ này là chấp nhận được —
`on_receive` chỉ đọc message, không chạm vào thứ đã bị hủy.

### Đảm bảo giao hàng

| Đảm bảo | Có? | Chi tiết |
|---|---|---|
| Thứ tự FIFO **cho từng người quan sát** | ✅ | hàng đợi looper sorted theo `(when_ms, seq)` |
| Thứ tự giống nhau **giữa các người quan sát** | ❌ | mỗi người có hàng đợi riêng, task riêng, priority riêng |
| Giao đúng một lần | ❌ | **at-most-once** — xem dưới |
| Người công bố biết ai đã nhận | một phần | `publish()` trả về **số** người nhận, không phải danh sách |

Sự kiện bị **rớt** trong ba trường hợp, cả ba đều được đếm vào
`ipc_bus_stats_t.dropped`, không bao giờ im lặng:

1. Đăng ký theo tên nhưng dịch vụ không còn trong ServiceManager (đang chết,
   hoặc đã bị health giết hẳn).
2. Pool message cạn (`ipc_message_obtain()` trả NULL).
3. Looper đích đang `STOPPED` hoặc `quit_requested`.

Vì vậy `publish()` trả về `0` **không phải lỗi** — nó có thể chỉ là "chưa ai
quan tâm chủ đề này".

### Giá trị giữ lại — cho người đến muộn

```c
ipc_bus_publish_retained(TOPIC_HEALTH_STATE, 1, 0);
```

Bus nhớ giá trị cuối của topic đó. Ai đăng ký **sau** sẽ nhận nó ngay lập tức,
không phải chờ tới nhịp cập nhật kế.

Đây là mảnh ghép làm cho hồi sinh hoạt động trọn vẹn: một dịch vụ vừa sống
lại đăng ký nghe `TOPIC_NET_STATE` và `TOPIC_HEALTH_STATE` thì biết ngay hiện
đang online hay offline, hệ đang khỏe hay đang ốm — thay vì chạy mù cho đến
khi trạng thái đó tình cờ đổi lần sau.

Quy tắc: **trạng thái thì retained, sự kiện thì không.** `NET_STATE`,
`HEALTH_STATE` là trạng thái. `SENSOR_SAMPLE`, `DATA_READY` là sự kiện — giao
lại một mẫu cũ cho người mới vào là bịa dữ liệu.

### Quyền sở hữu payload

**Bus không bao giờ giải phóng payload.** Một sự kiện tới nhiều người quan
sát, nên "ai free" sẽ không bao giờ rõ ràng — bus từ chối trách nhiệm đó thay
vì đoán.

- Dữ liệu nhỏ → nhét vào `arg1`/`arg2` (hệ này dùng milli-độ C, vừa `int32`).
- Dữ liệu lớn → trỏ tới bộ nhớ **sống lâu** do bên công bố giữ (static, hoặc
  vùng đệm retained), và bên công bố phải bảo đảm nó không bị ghi đè trước khi
  người quan sát chậm nhất xử lý xong.

### Từ ISR

```c
ipc_bus_publish_from_isr(TOPIC_SENSOR_SAMPLE, mc, seq, &woken);
```

Đường riêng, hẹp hơn: không lấy mutex (chỉ critical section ngắn) và **không
phân giải theo tên** — vì ServiceManager dùng mutex. Nghĩa là đăng ký theo tên
sẽ không nhận được sự kiện phát từ ISR. Nếu cần nhận từ ISR thì phải đăng ký
theo con trỏ handler.

### Vòng đời một đăng ký

```
subscribe()  → cấp slot, id = (generation << 16) | (slot + 1)
             → nếu topic có retained: giao ngay
   ...
publish()    → chụp → giao vào hàng đợi
   ...
unsubscribe(id)              → nhả slot
unsubscribe_service(name)    → nhả mọi slot của tên đó   ← khung dùng cái này
```

`id` mã hóa `generation` nên hủy nhầm một slot đã được cấp phát lại cho người
khác là **không thể** (lỗi ABA kinh điển). Có test riêng cho việc này.

**Ai hủy đăng ký khi dịch vụ hồi sinh**: khung, không phải dịch vụ. Xem
[service_iface.c](../services/common/service_iface.c) — trước mỗi lần gọi
`on_subscribe()`, khung gọi `ipc_bus_unsubscribe_service(name)`. Nếu để dịch
vụ tự làm và nó quên, sau mỗi lần hồi sinh nó sẽ nhận **mọi sự kiện hai lần**.
Có test bắt đúng lỗi này (`test_processor_chet_roi_hoi_sinh_khong_xu_ly_trung`).

### Ai quan sát ai trong hệ IoT

| Chủ đề | Người công bố | Người quan sát | Retained |
|---|---|---|---|
| `TOPIC_SENSOR_SAMPLE` | sensor | processor | ✗ |
| `TOPIC_DATA_READY` | processor | uploader, config, *(console trong demo)* | ✗ |
| `TOPIC_ALERT` | processor | health, *(console)* | ✗ |
| `TOPIC_CONFIG_CHANGED` | config | sensor *(đổi nhịp lấy mẫu)* | ✗ |
| `TOPIC_NET_STATE` | bất kỳ ai phát hiện | uploader | ✓ |
| `TOPIC_UPLOAD_RESULT` | uploader | *(console)* | ✗ |
| `TOPIC_HEALTH_STATE` | health | ai quan tâm | ✓ |

Đọc cột "người quan sát" theo chiều ngược lại sẽ thấy điều quan trọng: **không
ô nào bắt buộc phải có**. Bỏ hết người quan sát của một chủ đề thì
`publish()` trả `0` và hệ vẫn chạy.

---

## 2. linkToDeath — quan sát cái chết của dịch vụ

Bus báo *"có dữ liệu"*. Nó không báo được *"tôi vừa chết"* — vì lúc đó không
còn ai chạy để publish.

```c
ipc_service_link_to_death("worker", on_worker_state, NULL);

static void on_worker_state(const char *svc, bool alive, uint32_t gen, void *u) {
    // alive=false: vừa phát hiện chết
    // alive=true : đã hồi sinh xong, gen là số thế hệ mới
}
```

Người báo là **supervisor**, không phải dịch vụ đã chết:

```
[supervisor] check_alive() = false
   ├─ ipc_service_notify_state(lp, false)   → gọi mọi recipient, alive=false
   ├─ ipc_looper_revive()
   └─ ipc_service_notify_state(lp, true)    → alive=true, CHỈ khi hồi sinh thành công
```

Hai quy tắc khi viết recipient:

- **Chạy trên task supervisor** → chỉ đẩy message đi, không làm việc nặng.
  Xem `on_worker_state` trong [examples/basic_ipc/main.c](../examples/basic_ipc/main.c):
  nó chỉ `ipc_handler_send()` sang looper UI.
- `alive=true` **không** được phát nếu revive thất bại (hết quota
  `max_restarts`). Im lặng ở đây nghĩa là dịch vụ chết hẳn.

---

## 3. Event group — quan sát một tập điều kiện

Bus và semaphore đều không trả lời được câu *"đã đủ **cả** A **và** B chưa"*.

```c
// Chờ tất cả dịch vụ báo sẵn sàng, tối đa 5 giây
ipc_event_group_wait(bits, BIT_ALL_SERVICES, /*wait_all=*/true, false, 5000);
```

Mỗi dịch vụ tự bật cờ của mình ở cuối chuỗi khởi động (khung làm, bước 5).
`app_start()` quan sát tập cờ đó. Quá 5 giây chưa đủ → có dịch vụ không lên
được → báo thất bại, **không** chạy nửa vời.

`timeout_ms = 0` biến `wait()` thành phép hỏi thăm không chặn — đó là cách bộ
test đơn luồng dùng nó.

---

## 4. Health — quan sát sức khỏe và tài nguyên

Đây là observer **thụ động**: nó tự đi đọc, vì thứ cần quan sát (heap sắp cạn,
hàng đợi dày lên) không có ai để tự báo.

```
[task health] mỗi 500ms:
   1. đọc probe   → heap free, min heap
   2. quét looper → độ sâu hàng đợi từng cái
   3. đọc pool    → số message còn trống
   4. rút vòng đệm exception do các service báo lên
   5. duyệt bảng luật → hành động
```

Phần **chủ động** ghép vào đây là `ipc_health_report()`: service báo sự việc,
hàm chỉ ghi vào vòng đệm rồi trả về ngay (nên gọi được từ ISR). Vòng đệm đầy
thì bỏ báo cáo **mới** và giữ báo cáo **cũ** — báo cáo đầu tiên của một sự cố
thường có giá trị chẩn đoán cao nhất; số bị mất được đếm vào `dropped_reports`.

Ranh giới trách nhiệm: **service báo sự việc, health quyết định.** `sensorService`
cố ý không tự leo thang mức độ theo số lần hỏng liên tiếp — ngưỡng "3 lần
trong 10 giây" nằm trong bảng luật ở `healthService.c`.

---

## 5. Heartbeat — quan sát sự sống

Observer thụ động thuần túy. Looper cập nhật `heartbeat_ms` mỗi vòng lặp —
**cả khi bận lẫn khi rảnh**, vì nó chỉ chặn tối đa `heartbeat_timeout_ms / 2`
rồi thức dậy làm mới nhịp. Nhờ vậy "im lặng" luôn đồng nghĩa với "chết", không
cần phân biệt idle với hang.

Hai người quan sát cùng đọc mốc đó, ngưỡng nới rộng dần nên không giẫm chân:

```
supervisor  (heartbeat_timeout)      → tạo lại task
watchdog    (2× ngưỡng trên)         → restart, thất bại nhiều lần thì reset chip
HW watchdog (chỉ được cho ăn khi mọi client khỏe)
```

---

## Chống áp lực ngược (backpressure)

Bus **không có** cơ chế chặn người công bố. Người công bố nhanh hơn người
quan sát thì hàng đợi của người quan sát dày lên, chứ không ai bị chặn lại.
Đó là chủ ý — chặn lại sẽ lan cái chậm ngược lên toàn hệ.

Hệ thống chịu áp lực đó ở ba tầng:

1. **Hàng đợi ứng dụng có trần.** Uploader giữ tối đa 16 bản ghi; tràn thì bỏ
   bản ghi **cũ nhất** và báo `EXC_QUEUE_OVERFLOW` — trong đo lường, dữ liệu
   mới có giá trị hơn dữ liệu cũ.
2. **Health quan sát độ sâu hàng đợi looper** (`queue_depth_warn = 48`) và
   **số message còn trong pool** (`pool_free_warn = 6`), sinh
   `IPC_EXC_QUEUE_FULL` / `IPC_EXC_OOM` trước khi hệ nghẹt hẳn.
3. **Pool cạn thì `publish()` rớt sự kiện và đếm vào `dropped`.** Không có
   đường nào mất dữ liệu mà im lặng.

## Sai lầm thường gặp

| Sai | Hệ quả | Đúng |
|---|---|---|
| Đăng ký theo con trỏ cho dịch vụ có thể restart | mất sự kiện sau khi hồi sinh | `ipc_bus_subscribe_service()` |
| Dịch vụ tự đăng ký nghe mà quên hủy cái cũ | nhận **hai lần** mỗi sự kiện | để khung làm, chỉ điền `on_subscribe` |
| Làm việc nặng trong death recipient | chặn task supervisor | đẩy message đi |
| Retained cho dữ liệu đo | người mới vào nhận mẫu cũ như mẫu mới | retained chỉ cho **trạng thái** |
| Trỏ payload vào stack | người quan sát đọc phải rác | static / retained buffer |
| Trông chờ thứ tự giống nhau giữa các listener | sai giả định | mỗi listener chỉ được đảm bảo FIFO của riêng nó |
| Coi `publish() == 0` là lỗi | báo động giả | `0` = chưa ai quan tâm |

## Event manager: module hay service có task riêng?

Mô hình pub/sub qua một "event manager" chung, mỗi service một hàng đợi độc
lập, event manager tự bỏ message vào hàng đợi của listener — chính là thứ đang
chạy. Ánh xạ:

| Khái niệm | Ở đây |
|---|---|
| event manager | `ipc_event.c` (bảng đăng ký + hàm `publish`) |
| hàng đợi độc lập của mỗi service | hàng đợi của `ipc_looper_t` mỗi service |
| EM bỏ message vào hàng đợi listener | `enqueue_to_subscriber()` → `ipc_handler_send()` |
| service register listener | `ipc_bus_subscribe_service()` trong `on_subscribe` |

Khác biệt duy nhất còn lại: **event manager ở đây là một module, không phải một
service có task riêng.** `ipc_bus_publish()` chạy ngay trên task của người công
bố — nó tra bảng rồi bỏ thẳng message vào hàng đợi của từng listener.

Nếu biến EM thành service có task và hàng đợi riêng, luồng sẽ là:
`publisher → hàng đợi EM → task EM thức dậy → fan-out → hàng đợi listener`.

Cái giá trên MCU, đo được chứ không phải ước lượng:

| | EM là module (hiện tại) | EM là service có task |
|---|---|---|
| Slot pool mỗi sự kiện | 1 (hoặc N nếu N listener) | **2** (hoặc 1+N) |
| Sức chứa burst (pool 64) | 64 sự kiện | **32** |
| Task / stack | 0 thêm | +1 |
| Độ trễ | 1 chặng | **2 chặng** |
| Điểm nghẽn chung | không | **có** — mọi sự kiện đi qua một task |

Publisher **không** bị chặn ở cả hai phương án — ở phương án hiện tại nó chỉ
lấy mutex ngắn, chụp danh sách rồi enqueue, không bao giờ chạy code của
listener. Nên cái lợi duy nhất của EM-có-task (giảm việc trên task publisher)
chỉ là vòng lặp fan-out qua N listener, mà N ở đây là 1–3.

Đổi lại là chi phí thật: gấp đôi áp lực lên pool — đúng cái nút thắt đã đo
(bắn 100 request không tiêu thụ kịp thì rớt 36 vì hết pool, không phải vì hàng
đợi đầy). Vì vậy EM giữ nguyên là module.

Khi nào nên đổi ý: nếu cần **ưu tiên sự kiện** (fan-out chạy ở priority riêng),
cần **ghi log/trace tập trung** mọi sự kiện, hoặc listener nhiều tới mức vòng
fan-out trên task publisher trở nên đáng kể (hàng chục listener một topic).

## Đọc thêm

- [FLOW.md](FLOW.md) — luồng chạy cụ thể theo từng bước, từng task
- [SERVICE_API.md](SERVICE_API.md) — khuôn `on_create` / `on_subscribe` / `on_receive`
- [ARCHITECTURE.md](ARCHITECTURE.md) — bốn tầng và ranh giới phụ thuộc
