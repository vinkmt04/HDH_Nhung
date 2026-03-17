# BT04 Hệ Điều Hành Nhúng - System Build Buildroot

Tài liệu này ghi chú lại quá trình thực hành biên dịch hệ điều hành và sử dụng Toolchain với Buildroot cho board BeagleBone Black (BBB).

---

## Bài tập 01: Biên dịch Buildroot cho BBB

**Mục tiêu:** Sử dụng mã nguồn Buildroot để biên dịch 1 Image cơ bản cho BBB (đầu ra gồm Image và Toolchain). Sau đó cài đặt và khởi động thành công OS trên BBB.

### 1. Cấu hình Buildroot
Tải mã nguồn Buildroot và thiết lập cấu hình mặc định cho BBB:
```bash
make beaglebone_defconfig
```

### 2. Tùy chỉnh Package
Thêm hoặc bỏ các phần mềm như `vi`, `nano`, `top`, `htop`... để tạo custom OS. Mở giao diện cấu hình:
```bash
make menuconfig
```
*(Điều hướng vào mục `Target packages` để tùy chỉnh các công cụ).*

### 3. Biên dịch hệ thống
Bắt đầu quá trình biên dịch (sẽ mất một khoảng thời gian):
```bash
make
```
Sau khi build xong, Image và Toolchain sẽ nằm trong thư mục `output/images` và `output/host`.

### 4. Cài đặt và Khởi động
* Nạp file `sdcard.img` vào thẻ nhớ SD.
* Cắm thẻ nhớ vào BBB, cấp nguồn để khởi động hệ điều hành.

---

## Bài tập 02: Sử dụng Toolchain từ Buildroot

**Mục tiêu:** Dùng Toolchain vừa tạo để biên dịch chương trình C/C++ trên PC và chạy thử trên BBB.

### 1. Viết chương trình Hello World
Tạo file `helloworld.c` trên máy tính:
```c
#include <stdio.h>

int main() {
    printf("Hello World! Chuong trinh bien dich bang Buildroot Toolchain.\n");
    return 0;
}
```

### 2. Biên dịch chéo trên PC
Dùng Toolchain của Buildroot để biên dịch file C thành file thực thi cho kiến trúc ARM:
```bash
./output/host/bin/arm-buildroot-linux-gnueabihf-gcc helloworld.c -o helloworld
```

### 3. Đưa chương trình xuống BBB
Copy file thực thi vào thẻ nhớ hoặc gửi qua SSH/SCP (nếu BBB đã kết nối mạng):
```bash
scp helloworld root@<địa_chỉ_IP_của_BBB>:/root/
```

### 4. Khởi chạy trên BBB
Truy cập terminal của BBB và chạy thử chương trình:
```bash
./helloworld
```
![BT04](https://github.com/user-attachments/assets/d1ba42c9-999e-42ca-b78e-b16a09da04df)
