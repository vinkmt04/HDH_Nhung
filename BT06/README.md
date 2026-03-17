# Báo cáo BT06-Application: Quá trình điều khiển GPIO qua Sysfs, lập trình C và đóng gói Package tùy chỉnh vào hệ điều hành Linux sử dụng Buildroot.

---

## 1. Giao tiếp với Device Driver qua Sysfs

### 1.1. Xác định số thứ tự GPIO (GPIO Number)
Trên BeagleBone Black (BBB), mỗi chân vật lý sẽ được hệ điều hành quản lý bằng một con số. Để biết chân bạn muốn dùng tương ứng với số mấy, ta sử dụng Debugfs:
* **Mount Debugfs:** `mount -t debugfs debugfs /sys/kernel/debug/`
* **Xem danh sách GPIO:** `cat /sys/kernel/debug/gpio`

<img width="794" height="285" alt="image" src="https://github.com/user-attachments/assets/60fa388d-547b-45c8-849a-d04f4d7fc857" />

**Kết quả:** Dựa vào log debug, chân **P9_12** hiện tại đang được kernel ánh xạ tới node **gpio-540** (thuộc `gpiochip0` quản lý dải từ 512-543).

<img width="375" height="49" alt="image" src="https://github.com/user-attachments/assets/312d3421-ef0e-4807-9d6c-4783cb107a81" />

### 1.2. Quá trình giao tiếp qua Sysfs
1. **Kích hoạt chân (Export):** `echo 540 > /sys/class/gpio/export`
   *Mục đích: Yêu cầu kernel cấp quyền thao tác chân GPIO 540 cho User Space. Hệ thống sẽ tạo ra thư mục /sys/class/gpio/gpio540/.*
2. **Cấu hình chiều hoạt động (Direction):** `echo out > /sys/class/gpio/gpio540/direction`
   *Mục đích: Thiết lập chân P9_12 hoạt động ở chế độ đầu ra (Output).*
3. **Điều khiển LED ON/OFF (Value):**
   * `echo 1 > /sys/class/gpio/gpio540/value` (Xuất mức High - 3.3V để Bật LED)
   * `echo 0 > /sys/class/gpio/gpio540/value` (Xuất mức Low - 0V để Tắt LED)
4. **Kiểm tra trạng thái:** `cat /sys/class/gpio/gpio540/value`

<img width="532" height="161" alt="image" src="https://github.com/user-attachments/assets/877b02c1-7af3-4507-ac5c-96581147ba47" />

<img width="544" height="966" alt="image" src="https://github.com/user-attachments/assets/d229ee91-9122-40f6-8073-59a9213241e0" />

---

## 2. Chương trình C điều khiển nhấp nháy LED

Viết chương trình `blink_led.c` giao tiếp với device driver GPIO thông qua các hàm hệ thống `open()`, `write()`, `close()`.

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define GPIO_PATH "/sys/class/gpio/gpio540/value"

int main() {
    int fd;
    while (1) {
        // Xuất mức cao (High) - Bật LED
        fd = open(GPIO_PATH, O_WRONLY);
        if (fd < 0) {
            perror("Error opening GPIO");
            return 1;
        }
        write(fd, "1", 1);
        close(fd);
        usleep(500000); // Dừng 0.5s

        // Xuất mức thấp (Low) - Tắt LED
        fd = open(GPIO_PATH, O_WRONLY);
        write(fd, "0", 1);
        close(fd);
        usleep(500000); // Dừng 0.5s
    }
    return 0;
}
```

---

## 3. Đóng gói Package và Tự động khởi chạy (Buildroot)

Quá trình đóng gói file thực thi được biên dịch chéo và cấu hình tự khởi động ngay sau khi cắm nguồn.

### 3.1. Cấu trúc thư mục cho Package
```text
home/vinh/buildroot_hdh/
└── package/
    └── blink_led/
        ├── src/
        │   ├── blink_led.c
        │   └── Makefile        # Makefile cho mã nguồn C
        ├── S99blink_led        # Init Script (Khởi chạy cùng hệ thống)
        ├── Config.in           # Định nghĩa menu hiển thị
        └── blink_led.mk        # Lệnh đóng gói và cài đặt
```

### 3.2. Nội dung các file cấu hình chi tiết

#### A. Makefile cho mã nguồn (`package/blink_led/src/Makefile`)
```makefile
CC ?= gcc
CFLAGS ?= -Wall

all: blink_led

blink_led: blink_led.c
	$(CC) $(CFLAGS) -o blink_led blink_led.c

clean:
	rm -f blink_led
```

#### B. Kịch bản tự khởi chạy (`package/blink_led/S99blink_led`)
Theo chuẩn BusyBox init, script này được đặt tại `/etc/init.d/` để chạy lúc khởi động:
```bash
#!/bin/sh
# Tu dong khoi chay Blink LED

case "$1" in
  start)
    printf "Starting blink_led: "
    # Chạy ngầm (&) và ẩn log để không rác màn hình boot
    /usr/bin/blink_led > /dev/null 2>&1 &
    echo "OK"
    ;;
  stop)
    printf "Stopping blink_led: "
    killall blink_led
    echo "OK"
    ;;
  restart|reload)
    $0 stop
    $0 start
    ;;
  *)
    echo "Usage: $0 {start|stop|restart}"
    exit 1
esac

exit 0
```

#### C. File cấu hình nhận diện (`package/blink_led/Config.in`)
```text
config BR2_PACKAGE_BLINK_LED
	bool "blink_led"
	help
	  Chuong trinh dieu khien chop tat LED tren chan GPIO 540 cua BBB.
```

#### D. File đóng gói Buildroot (`package/blink_led/blink_led.mk`)
```make
BLINK_LED_VERSION = 1.0
BLINK_LED_SITE = $(BLINK_LED_PKGDIR)/src
BLINK_LED_SITE_METHOD = local

define BLINK_LED_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) all
endef

# Copy file thực thi vào /usr/bin/ và script khởi động vào /etc/init.d/
define BLINK_LED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/blink_led $(TARGET_DIR)/usr/bin/blink_led
	$(INSTALL) -D -m 0755 $(BLINK_LED_PKGDIR)/S99blink_led $(TARGET_DIR)/etc/init.d/S99blink_led
endef

$(eval $(generic-package))
```

---

## 4. Kích hoạt Package và Biên dịch

1. **Thêm vào hệ thống:** Thêm file `Config.in` vào bên trong `package/Config.in` (tìm đến menu Hardware Handling):
   ```text
   source "package/blink_led/Config.in"
   ```
2. **Kích hoạt trong Menuconfig:** Chạy lệnh `make menuconfig` -> Tìm đến `Target Packages` -> `Hardware handling` -> Tích chọn gói **blink_led**.
3. **Biên dịch:** Chạy lệnh `make` tại thư mục gốc Buildroot.
4. **Kết quả:** Sau khi hệ điều hành được build lại và flash sang BBB, LED tại chân P9_12 sẽ tự động nhấp nháy mỗi 0.5s ngay khi boot thành công mà không cần chạy lệnh thủ công.
