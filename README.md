# Lab 3 – Giám sát mực nước và nhiệt độ bể nuôi bằng ESP32

Bài thực hành **Đề 4**: sử dụng ESP32 và FreeRTOS để đọc khoảng cách đến mặt nước, đo nhiệt độ nước, gửi dữ liệu lên MQTT và điều khiển bơm vào/van xả qua giao diện web.

**[Mở mô phỏng Wokwi](https://wokwi.com/projects/480312949240836097)** · **[Mã nguồn ESP32](firmware/)** · **[Giao diện HTML](Be_nuoi_MQTT.html)** · **[Báo cáo](bao_cao_lap3/)**

## 1. Chức năng

- Đọc khoảng cách từ HC-SR04 đến mặt nước và nhiệt độ từ DS18B20.
- Hiển thị dữ liệu trên Serial Monitor.
- Gửi dữ liệu JSON đến MQTT broker qua WiFi.
- Hiển thị hai giá trị dạng số và hai biểu đồ trên giao diện web.
- Điều khiển bật/tắt máy bơm, mở/đóng van ở chế độ thủ công.
- Điều khiển tự động theo khoảng cách và thời gian quy định.
- Phân chia xử lý thành ba task FreeRTOS trên hai core của ESP32.

> Trong mô phỏng, LED xanh dương biểu diễn đầu ra PWM của bơm; relay và LED xanh lá biểu diễn van xả. Duty PWM 50% là tín hiệu điều khiển mô phỏng, không phải phép đo công suất cơ thực tế của bơm.

## 2. Cấu trúc repository

| Đường dẫn | Nội dung |
|---|---|
| [firmware/wifi-scan.ino](firmware/wifi-scan.ino) | Chương trình ESP32 Arduino và FreeRTOS |
| [firmware/diagram.json](firmware/diagram.json) | Linh kiện và sơ đồ nối dây Wokwi |
| [firmware/libraries.txt](firmware/libraries.txt) | Danh sách thư viện |
| [Be_nuoi_MQTT.html](Be_nuoi_MQTT.html) | Giao diện giám sát và điều khiển qua MQTT |
| [video_demo/](video_demo/) | Video minh họa hoạt động hệ thống |
| [wokwi-link/README.md](wokwi-link/README.md) | Đường dẫn dự án mô phỏng |
| [bao_cao_lap3/](bao_cao_lap3/) | Thư mục dành cho báo cáo Word/PDF sẽ bổ sung |

File `.gitkeep` dùng để giữ thư mục khi chưa có tài liệu. Báo cáo sẽ được bổ sung sau.

## 3. Phần cứng và kết nối

| Thiết bị | Chân/tín hiệu | Kết nối ESP32 |
|---|---|---|
| HC-SR04 | VCC / GND | 5V / GND |
| HC-SR04 | TRIG | GPIO18 |
| HC-SR04 | ECHO | Qua chia áp đến GPIO19 |
| DS18B20 | VCC / GND | 3V3 / GND |
| DS18B20 | DQ | GPIO4; điện trở kéo lên 3V3 là 4,7 kΩ |
| Bơm mô phỏng | PWM qua điện trở 220 Ω đến LED xanh dương | GPIO25 |
| Relay van xả | IN | GPIO26 |
| Relay van xả | VCC / GND | 5V / GND |
| Tải van mô phỏng | COM nhận 5V; NO qua điện trở 330 Ω đến LED xanh lá | Cathode LED nối GND |

ECHO dùng chia áp: **ECHO → R 1 kΩ → nút GPIO19 → R 2 kΩ → GND**. Các thiết bị nối chung GND. Relay `npn` trong sơ đồ hiện tại kích ở mức **HIGH**.

Khi triển khai phần cứng thật, dùng đầu dò DS18B20 chống nước, driver PWM phù hợp cho bơm và nguồn riêng phù hợp với tải. Không cấp bơm hoặc van trực tiếp từ GPIO.

## 4. Tổ chức FreeRTOS

| Task | Core | Ưu tiên | Nhiệm vụ |
|---|---:|---:|---|
| `taskRead` | 1 | 1 | Đọc hai cảm biến, in Serial, ghi dữ liệu vào hai queue |
| `taskAlert` | 1 | 2 | Nhận khoảng cách và lệnh điều khiển; cập nhật bơm/van |
| `taskTransmit` | 0 | 1 | Kết nối WiFi/MQTT, gửi JSON, nhận lệnh MQTT và Serial |

| Queue | Dữ liệu | Số phần tử | Luồng sử dụng |
|---|---|---:|---|
| `dataQueue` | `SensorData`: khoảng cách, nhiệt độ, thời điểm lấy mẫu | 1 | Read → Transmit |
| `dataAlert` | `float`: khoảng cách | 1 | Read → Alert |
| `commandQueue` | `Command`: lệnh điều khiển | 8 | Transmit/callback MQTT → Alert |
| `stateQueue` | `OutputState`: PWM bơm, trạng thái van, chế độ | 1 | Alert → Transmit |

Các queue một phần tử dùng `xQueueOverwrite()` để giữ giá trị mới nhất. Task truyền dùng `xQueuePeek()` đọc bản sao mà không lấy phần tử ra khỏi queue. Trong vận hành, task Alert sở hữu việc cập nhật đầu ra; các task khác gửi yêu cầu qua queue.

Task Alert kiểm tra thời hạn bằng `millis()`, thay vì chặn task trong 10–20 giây. Nhờ đó vẫn tiếp nhận lệnh thủ công trong chu kỳ tự động.

## 5. Thuật toán tự động

Đại lượng xét ngưỡng là **d: khoảng cách từ cảm biến đến mặt nước**, đơn vị mét. Nếu cần chiều cao nước thực tế h, phải biết độ cao lắp cảm biến H và tính **h = H − d**.

| Khoảng cách d khi bắt đầu chu kỳ | Hành động |
|---|---|
| d < 0,5 m | Mở van xả 10 giây |
| 0,5 ≤ d < 1,0 m | Không kích hoạt bơm/van |
| 1,0 ≤ d < 1,5 m | Bơm PWM 50% trong 10 giây |
| 1,5 ≤ d ≤ 2,0 m | Bơm PWM 100% trong 20 giây |
| d > 2,0 m | Không kích hoạt bơm/van |

- Quy ước **d = 1,5 m** thuộc nhánh bơm 100% để không chồng ngưỡng.
- Mỗi vùng kích hoạt **một chu kỳ**. Sau khi hết thời gian, đầu ra tắt; giữ nguyên vùng sẽ không tự kích lại.
- Nếu đổi vùng khi đang chạy, chương trình hoàn thành chu kỳ hiện tại rồi xét vùng mới.
- Chuyển sang vùng khác sau khi chu kỳ kết thúc để tái kích; lệnh `AUTO` cũng đặt lại việc xét vùng.
- Đọc lỗi khoảng cách hoặc quá 3 giây không có dữ liệu mới sẽ dừng đầu ra trong chế độ tự động.
- Bật bơm bằng lệnh manual sẽ đóng van; mở van bằng lệnh manual sẽ dừng bơm.

## 6. Kết nối MQTT

| Tham số | Giá trị |
|---|---|
| WiFi Wokwi | `Wokwi-GUEST`, mật khẩu rỗng |
| Broker | `broker.hivemq.com` |
| ESP32 | MQTT TCP, cổng `1883` |
| Trình duyệt | `wss://broker.hivemq.com:8884/mqtt` |
| Topic dữ liệu | `nhungblt/de4/ea9c235d/data` |
| Topic lệnh | `nhungblt/de4/ea9c235d/cmd` |

ESP32 và HTML dùng cùng broker và topic, nhưng kết nối bằng giao thức phù hợp với từng thiết bị.

Ví dụ cấu trúc bản tin:

```json
{
  "distance_m": 1.2,
  "temperature_c": 28.0,
  "pump_percent": 50,
  "valve_open": false,
  "mode": "auto",
  "sample_ms": 1000
}
```

`sample_ms` là thời gian lấy mẫu tính từ lúc ESP32 khởi động, không phải thời gian lịch. Giá trị cảm biến lỗi được gửi là `null`.

| Payload gửi đến topic lệnh | Tác dụng |
|---|---|
| `PUMP_ON` | Chuyển manual, bật bơm PWM 100%, đóng van |
| `PUMP_OFF` | Chuyển manual, tắt bơm |
| `VALVE_ON` | Chuyển manual, mở van, tắt bơm |
| `VALVE_OFF` | Chuyển manual, đóng van |
| `AUTO` | Trở lại chế độ tự động |

Broker công cộng được dùng cho dữ liệu mô phỏng của bài thực hành. Khi làm nhiều bộ độc lập, đổi tiền tố topic riêng và cập nhật đồng thời trong sketch và HTML.

## 7. Cách chạy

### Chạy mô phỏng Wokwi

1. Mở [dự án Wokwi](https://wokwi.com/projects/480312949240836097).
2. Kiểm tra đủ ba file: `wifi-scan.ino`, `diagram.json`, `libraries.txt`.
3. Các thư viện cần có: **OneWire**, **DallasTemperature**, **PubSubClient**.
4. Bấm **Run** và theo dõi Serial Monitor ở **115200 baud**.
5. Chờ thông báo `[MQTT core 0] Connected`.
6. Bấm HC-SR04 để đổi khoảng cách; bấm DS18B20 để đổi nhiệt độ.

Nếu dựng lại dự án mới, chép nội dung các file trong [firmware/](firmware/) vào các tab tương ứng và kiểm tra thư viện trong Library Manager.

### Mở giao diện web

1. Tải [Be_nuoi_MQTT.html](Be_nuoi_MQTT.html) về máy và mở bằng Chrome/Edge.
2. Giữ mô phỏng Wokwi đang chạy.
3. Bấm **Kết nối MQTT** trên giao diện.
4. Khi nhận dữ liệu ESP32, hai ô số và hai biểu đồ cập nhật; các nút điều khiển được mở.
5. Bấm nút bơm/van để điều khiển manual; bấm **Chuyển tự động** để gửi `AUTO`.

Trang cần Internet để tải MQTT.js và kết nối broker. Kết nối được broker chưa có nghĩa ESP32 đã online: cần nhận bản tin dữ liệu. Biểu đồ giữ tối đa 60 mẫu trong phiên mở trang. Trạng thái đầu ra được xác nhận bằng phản hồi ESP32.

Nếu không có phản hồi mới trong 10 giây, giao diện tạm khóa các nút điều khiển. Khi mất kết nối, web không tự gửi lệnh tắt đến ESP32.

### Thử nhanh qua Serial

| Ký tự | Tác dụng |
|---|---|
| `p` | Bật bơm, chuyển manual |
| `s` | Tắt bơm, chuyển manual |
| `v` | Mở van, chuyển manual |
| `x` | Đóng van, chuyển manual |
| `a` | Trở lại tự động |

## 8. Các trường hợp kiểm tra

Bảng dưới là **kết quả mong đợi**, không thay thế ảnh/video kiểm chứng.

| Thao tác | Kết quả mong đợi |
|---|---|
| Bắt đầu chu kỳ ở khoảng 1,2 m | LED bơm PWM 50% trong 10 giây; van đóng |
| Bắt đầu chu kỳ ở khoảng 1,88 m | LED bơm PWM 100% trong 20 giây; van đóng |
| Bắt đầu chu kỳ ở khoảng 0,3 m | LED van sáng trong 10 giây; bơm tắt |
| Giữ nguyên vùng sau khi chu kỳ kết thúc | Bơm/van giữ tắt |
| Đổi nhiệt độ DS18B20 | Serial, ô nhiệt độ và biểu đồ nhận giá trị mới |
| Bật bơm trên web | ESP32 chuyển manual, bơm 100%, van đóng |
| Mở van trên web | ESP32 chuyển manual, van mở, bơm tắt |
| Bấm Chuyển tự động | ESP32 nhận `AUTO` và xét vùng khoảng cách hiện tại |

Wokwi không tự thay đổi khoảng cách khi bơm/van hoạt động; dùng thanh trượt để mô phỏng mặt nước thay đổi. Thời gian 10/20 giây tính theo đồng hồ mô phỏng.

## 9. Báo cáo và minh chứng

Tải báo cáo Word/PDF vào [bao_cao_lap3/](bao_cao_lap3/) và video minh họa vào [video_demo/](video_demo/).

Nội dung báo cáo cần trình bày:

- Ưu điểm của RTOS, ưu/nhược điểm của FreeRTOS.
- Cơ chế semaphore và queue: nguyên lý, ưu điểm và ứng dụng.
- Sơ đồ nối dây, bảng chân và linh kiện.
- Bảng task, core, ưu tiên và dữ liệu trao đổi.
- Giải thích thuật toán tự động, manual và kết nối MQTT.
- Giao diện web, hình ảnh/video kết quả và nhận xét.

Tài liệu này mô tả chức năng theo mã nguồn hiện tại; kết quả vận hành cần được bổ sung bằng minh chứng chạy thực tế.
