# Báo cáo BT06: Điều khiển GPIO qua Sysfs, lập trình C và đóng gói Package tùy chỉnh vào hệ điều hành Linux (Buildroot).

---

## 1. Giao tiếp với Device Driver qua Sysfs

Trước khi lập trình, chúng ta cần xác định và kiểm tra chân GPIO trực tiếp từ không gian người dùng (User Space).

### 1.1. Xác định đúng chỉ số GPIO
Trên BeagleBone Black (BBB), chân **P9_12** hiện tại đang được kernel ánh xạ tới node **gpio-540** (thuộc `gpiochip0` quản lý dải từ 512-543).
* **Lệnh kiểm tra:** `cat /sys/kernel/debug/gpio`



### 1.2. Quy trình điều khiển thủ công
1. **Kích hoạt chân (Export):** `echo 540 > /sys/class/gpio/export`  
   *(Tạo thư mục `/sys/class/gpio/gpio540/`)*
2. **Cấu hình chiều (Direction):** `echo out > /sys/class/gpio/gpio540/direction`  
   *(Thiết lập chế độ Output để điều khiển LED)*
3. **Xuất tín hiệu (Value):** * `echo 1 > /sys/class/gpio/gpio540/value` (Bật LED - 3.3V)
   * `echo 0 > /sys/class/gpio/gpio540/value` (Tắt LED - 0V)
4. **Kiểm tra trạng thái:** `cat /sys/class/gpio/gpio540/value`

---

## 2. Chương trình C điều khiển Blink LED

Chúng ta viết file `blink_led.c` để thực hiện việc nhấp nháy LED tự động với chu kỳ 0.5 giây bằng các hàm hệ thống chuẩn.

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define GPIO_VALUE_PATH "/sys/class/gpio/gpio540/value"

int main() {
    int fd;
    while (1) {
        fd = open(GPIO_VALUE_PATH, O_WRONLY);
        if (fd < 0) {
            perror("Không thể mở file GPIO");
            return 1;
        }
        write(fd, "1", 1); // LED ON
        close(fd);
        usleep(500000); // 0.5s

        fd = open(GPIO_VALUE_PATH, O_WRONLY);
        write(fd, "0", 1); // LED OFF
        close(fd);
        usleep(500000);
    }
    return 0;
}
```

## 3. Đóng gói Package và Tự động khởi chạy (Buildroot)
Quy trình tích hợp ứng dụng vào Rootfs của hệ điều hành để LED tự nháy ngay sau khi cắm nguồn.
### 3.1. Cấu trúc thư mục Package
home/vinh/buildroot_hdh/package/blink_led/

├── src/

│   ├── blink_led.c

│   └── Makefile        # Lệnh biên dịch mã nguồn C

├── S99blink_led        # Init Script (Tự chạy khi boot)

├── Config.in           # Hiển thị trong menuconfig

└── blink_led.mk        # Lệnh đóng gói của Buildroot

### 3.2. Nội dung các file cấu hình quan trọng
Makefile (trong thư mục src/)
Init Script (S99blink_led)
Đặt tại /etc/init.d/ để quản lý dịch vụ
Buildroot Package (blink_led.mk)

## 4. Kích hoạt và Biên dịch hệ thống
### 4.1. Thêm vào Buildroot: Sửa file package/Config.in, thêm dòng:
source "package/blink_led/Config.in"
### 4.2. Chọn Package: Chạy make menuconfig -> Target Packages -> Hardware handling -> Tích chọn blink_led.
### 4.3. Biên dịch: Chạy lệnh make tại thư mục gốc Buildroot.
### 4.4. Kết quả: File thực thi sẽ nằm tại /usr/bin/blink_led và script khởi chạy tại /etc/init.d/S99blink_led. LED sẽ nhấp nháy ngay khi BBB khởi động xong.
