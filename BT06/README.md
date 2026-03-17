# Embedded Linux Project: Blink LED with Buildroot on BeagleBone Black (BBB)

Dự án này hướng dẫn cách điều khiển GPIO trên BeagleBone Black thông qua Sysfs, viết chương trình C để chớp tắt LED, và quan trọng nhất là cách đóng gói ứng dụng thành một **Buildroot Package** để tự động khởi chạy cùng hệ thống.

---

## 1. Xác định phần cứng và Thao tác qua Sysfs

Trước khi viết code, chúng ta cần xác định và kiểm tra chân GPIO trực tiếp trên Terminal của BBB.

### 1.1. Tìm số thứ tự GPIO (GPIO Number)
Trên BBB, chân **P9_12** được kernel ánh xạ thông qua `gpiochip`.
* **Lệnh kiểm tra:** `cat /sys/kernel/debug/gpio`
* **Kết quả:** Chân P9_12 tương ứng với **gpio-540**.

### 1.2. Thao tác điều khiển thủ công
Sử dụng giao tiếp với Device Driver qua Sysfs để test LED:

```bash
# 1. Kích hoạt chân GPIO
echo 540 > /sys/class/gpio/export

# 2. Cấu hình chiều Output
echo out > /sys/class/gpio/gpio540/direction

# 3. Điều khiển ON/OFF
echo 1 > /sys/class/gpio/gpio540/value # Bật LED
echo 0 > /sys/class/gpio/gpio540/value # Tắt LED
