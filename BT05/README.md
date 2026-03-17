# BT05 Biên dịch Ứng dụng và Thư viện với Buildroot

Tài liệu này hướng dẫn chi tiết các bước thực hành với Buildroot trên BeagleBone Black (BBB), từ việc sử dụng thư viện có sẵn (cJSON), tự tạo thư viện cá nhân bằng C, cho đến việc đóng gói (package) thư viện và ứng dụng tích hợp trực tiếp vào hệ điều hành.

---

## Bài tập 1: Biên dịch ứng dụng với thư viện đã có (cJSON)

### Bước 1: Bật `cJSON` trong Buildroot và Build lại OS
Tại thư mục gốc của Buildroot, gõ lệnh:
```bash
make menuconfig
```
1. Di chuyển theo đường dẫn: `Target packages ---> Libraries ---> JSON/XML`
2. Tích chọn `[*] cJSON`
3. Chạy lệnh `make` để Buildroot tải source cJSON, biên dịch và tích hợp vào Image.

> **Lưu ý:** Sau khi build xong:
> - File thư viện `.so` sẽ nằm trong `output/target/usr/lib`
> - File header `.h` sẽ nằm trong `output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/cJSON`

### Bước 2: Viết chương trình `HelloJSON.c`
*(Chuẩn bị file mã nguồn `HelloJSON.c` của bạn tại thư mục làm việc).*

### Bước 3: Biên dịch chéo (Cross-compile)
Sử dụng file biên dịch chéo của chính buildroot:
```bash
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc HelloJSON.c -o HelloJSON \
-I./output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/cjson \
-L./output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/lib \
-lcjson
```

**Giải thích lệnh:**
* `-o HelloJSON`: Tên file đầu ra chạy trên BBB.
* `-lcjson`: Link với thư viện cJSON mà Buildroot đã build ở Bước 1.

### Bước 4 & 5: Đưa xuống BBB và Khởi chạy
Chuyển file xuống board BBB:
```bash
scp HelloJSON root@<địa_chỉ_IP_BBB>:/root/
```

Ngoài ra nếu chỉ copy file `.c` sang BBB thì sẽ cần copy cả file thư viện động `.so` sang:
```bash
scp output/target/usr/lib/libcjson.so* root@192.168.10.109:/usr/lib/
```

Chạy file trên BBB bằng lệnh:
```bash
./HelloJSON
```
<img width="648" height="188" alt="bai1" src="https://github.com/user-attachments/assets/178e0e8a-407c-4fb8-8b7e-fb353dcc83cb" />

---

## Bài tập 2: Tự tạo thư viện cá nhân

### Bước 1: Viết thư viện cá nhân
Bao gồm 2 file `.h` và `.c` (Ví dụ: `libptit.h` và `libptit.c`).

### Bước 2: Biên dịch Thư viện Tĩnh (.a) và Thư viện Động (.so)
Sử dụng Toolchain từ Buildroot đã tạo ở Bài tập 1.

**Thư viện tĩnh (Static Library):**
```bash
# Biên dịch code C sang file object .o
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc -c libptit.c -o libptit.o

# Đóng gói thành file .a
./output/host/bin/arm-buildroot-linux-gnueabihf-ar rcs libptit.a libptit.o
```

**Thư viện động (Dynamic Library):**
```bash
# Biên dịch với cờ -fPIC (Position Independent Code)
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc -fPIC -c libptit.c -o libptit.o

# Tạo file .so
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc -shared -o libptit.so libptit.o
```
> **Lưu ý:** `-fPIC` giúp tạo ra mã nguồn không phụ thuộc vào vị trí địa chỉ tuyệt đối. Điều này cho phép nhiều chương trình cùng chia sẻ một thư viện động tại các vùng nhớ khác nhau trong RAM.

### Bước 3: Đưa thư viện vào Sysroot
Việc này giúp Toolchain tự động tìm thấy thư viện khi biên dịch các ứng dụng khác.
```bash
# Định nghĩa đường dẫn Sysroot cho gọn 
SYSROOT=$(pwd)/output/host/arm-buildroot-linux-gnueabihf/sysroot  

# Copy file header vào include 
cp libptit.h $SYSROOT/usr/include/  

# Copy file thư viện vào lib 
cp libptit.a libptit.so $SYSROOT/usr/lib/
```

### Bước 4 & 5: Viết chương trình ứng dụng và Biên dịch `app_test.c`

**Chương trình dùng thư viện tĩnh (app_static):**
```bash
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc app_test.c -o app_static -lptit -static
```

**Chương trình dùng thư viện động (app_dynamic):**
```bash
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc app_test.c -o app_dynamic -lptit
```
> **Lưu ý:** Khi dùng flag `-lptit` (hoặc `-lkmt`), trình biên dịch sẽ tự động tìm file có tiền tố là `lib` và hậu tố là `.a` hoặc `.so`.

### Bước 6 & 7: Thử nghiệm trên BBB và So sánh
Đưa cả 2 chương trình và file `libptit.so` (hoặc `libkmt.so`) xuống BBB.

Chuyển file thư viện sang BBB:
```bash
scp libkmt.so root@192.168.10.109:/usr/lib/
```

**Chạy 2 bản biên dịch tĩnh và động trên BBB:**
```bash
./app_static
./app_dynamic
```
*(app_static chạy được ngay, app_dynamic cần có file .so trong /usr/lib).*

**Kiểm tra & So sánh:**
* Dùng lệnh `ls -lh` để so sánh kích thước 2 file tĩnh và động.
* Kiểm tra sự phụ thuộc (Dependencies) của bản động:
```bash
./output/host/bin/arm-buildroot-linux-gnueabihf-readelf -d app_dynamic | grep NEEDED
```
<img width="999" height="111" alt="bai2" src="https://github.com/user-attachments/assets/109b07a7-9da6-41be-aedd-405d8d4dddc4" />

---

## Bài tập 3: Tích hợp ứng dụng và thư viện vào Buildroot

### Bước 1: Đưa thư viện `libkmt` vào Buildroot làm Package

Tạo cấu trúc thư mục:
```bash
mkdir -p package/libkmt/src
```
*(Lưu ý: Bỏ file `kmt.c`, `kmt.h` vào thư mục `src` vừa tạo).*

Tạo file `package/libkmt/Config.in`:
```kconfig
config BR2_PACKAGE_LIBKMT
    bool "libkmt"
    help
      Thu vien tinh toan co ban cho bai tap HDH Nhung.
```

Tạo file `package/libkmt/libkmt.mk`:
```makefile
LIBKMT_VERSION = 1.0
LIBKMT_SITE = $(TOPDIR)/package/libkmt/src
LIBKMT_SITE_METHOD = local
LIBKMT_INSTALL_STAGING = YES

define LIBKMT_BUILD_CMDS
    $(TARGET_CC) $(TARGET_CFLAGS) -fPIC -c $(@D)/kmt.c -o $(@D)/kmt.o
    $(TARGET_AR) rcs $(@D)/libkmt.a $(@D)/kmt.o
    $(TARGET_CC) -shared $(TARGET_CFLAGS) -o $(@D)/libkmt.so $(@D)/kmt.o
endef

define LIBKMT_INSTALL_STAGING_CMDS
    $(INSTALL) -D -m 0644 $(@D)/kmt.h $(STAGING_DIR)/usr/include/kmt.h
    $(INSTALL) -D -m 0755 $(@D)/libkmt.a $(STAGING_DIR)/usr/lib/libkmt.a
    $(INSTALL) -D -m 0755 $(@D)/libkmt.so $(STAGING_DIR)/usr/lib/libkmt.so
endef

define LIBKMT_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/libkmt.so $(TARGET_DIR)/usr/lib/libkmt.so
endef

$(eval $(generic-package))
```

### Bước 2: Viết chương trình kết hợp `combined_app.c`

Tạo thư mục `src` cho ứng dụng và di chuyển file code vào:
```bash
mkdir -p package/combined_app/src
mv combined_app.c package/combined_app/src/
```

Nội dung file `combined_app.c`:
```c
#include <stdio.h>
#include <cjson/cJSON.h>
#include <kmt.h>

int main() {
    const char *data = "{\"val1\": 15, \"val2\": 25}";
    cJSON *json = cJSON_Parse(data);
    
    int v1 = cJSON_GetObjectItem(json, "val1")->valueint;
    int v2 = cJSON_GetObjectItem(json, "val2")->valueint;
    
    printf("JSON Data: val1=%d, val2=%d\n", v1, v2);
    printf("Ket qua tinh tong tu libkmt: %d\n", tinh_tong(v1, v2));
    
    cJSON_Delete(json);
    return 0;
}
```

### Bước 3: Tạo Package cho `combined_app` với ràng buộc phụ thuộc

Tạo thư mục:
```bash
mkdir -p package/combined_app
```

Tạo file `package/combined_app/Config.in`:
```kconfig
config BR2_PACKAGE_COMBINED_APP
    bool "combined_app"
    select BR2_PACKAGE_CJSON      # Tu dong bat cJSON
    select BR2_PACKAGE_LIBKMT     # Tu dong bat libkmt
    help
      Ung dung ket hop cJSON va libkmt.
```

Tạo file `package/combined_app/combined_app.mk`:
```makefile
COMBINED_APP_VERSION = 1.0
COMBINED_APP_SITE = $(TOPDIR)/package/combined_app/src
COMBINED_APP_SITE_METHOD = local
COMBINED_APP_DEPENDENCIES = cjson libkmt

define COMBINED_APP_BUILD_CMDS
    $(TARGET_CC) $(TARGET_CFLAGS) $(@D)/combined_app.c -o $(@D)/combined_app \
    $(TARGET_LDFLAGS) -lcjson -lkmt
endef

define COMBINED_APP_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/combined_app $(TARGET_DIR)/usr/bin/combined_app
endef

$(eval $(generic-package))
```
> **Mục đích của TARGET_LDFLAGS:** Giúp trình liên kết (Linker) tự động nhận diện và tìm đúng đường dẫn thư viện chuẩn bên trong sysroot của Buildroot, giải quyết triệt để lỗi báo thiếu `-lcjson` hoặc `-lkmt`.

### Bước 4: Kích hoạt và Build lại hệ thống

**Khai báo Package mới:** Mở file `package/Config.in` (file tổng của Buildroot) và thêm vào cuối:
```kconfig
menu "Custom Apps"
    source "package/libkmt/Config.in"
    source "package/combined_app/Config.in"
endmenu
```

**Cấu hình:**
1. Chạy `make menuconfig`.
2. Tìm đến mục `Custom Apps`.
3. Chỉ cần tích chọn `[*] combined_app`. Sẽ thấy cJSON và libkmt tự động được chọn theo (do lệnh select).
4. Lưu và thoát.

**Biên dịch:**
```bash
make
```

### Bước 5: Kiểm tra trên BBB

Nạp Image vào thẻ nhớ:
```bash
sudo dd if=output/images/sdcard.img of=/dev/sdX bs=4M status=progress conv=fsync
```

Sau khi nạp Image mới vào thẻ nhớ và khởi động BBB, gõ lệnh:
```bash
combined_app
```
Hệ thống sẽ tự chạy mà không cần ông phải copy file `.so` hay export đường dẫn thủ công nữa, vì Buildroot đã cài đặt mọi thứ vào đúng vị trí hệ thống (`/usr/bin` và `/usr/lib`).

<img width="624" height="115" alt="bai3" src="https://github.com/user-attachments/assets/7bd6df4d-3a19-4648-bb88-61a3712901f8" />

---

### CHÚ Ý: Thao tác Copy file thủ công vào thẻ nhớ

Nếu bạn mount phân vùng `rootfs` của thẻ nhớ trên Ubuntu (ví dụ: `/media/vinh/rootfs1`), bạn có thể copy trực tiếp:

1. Copy chương trình cJSON (Bài tập Buildroot):
```bash
sudo cp HelloJSON /media/vinh/rootfs1/root/
```

2. Copy 2 chương trình thư viện (Bài tập Library):
```bash
sudo cp kmt_static kmt_dynamic /media/vinh/rootfs1/root/
```

3. Đảm bảo file thư viện động `libkmt.so` đã có trong hệ thống (để chắc chắn):
```bash
sudo cp libkmt.so /media/vinh/rootfs1/usr/lib/
```
