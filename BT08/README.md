# Bài tập: Sử dụng các công cụ Gỡ lỗi (Debug) và Đánh giá Hiệu năng Cơ bản

Repository này lưu trữ mã nguồn và hướng dẫn các bước thực hành gỡ lỗi chéo (Cross-Debugging), phân tích bộ nhớ, giám sát lỗi và đánh giá hiệu năng trên môi trường Embedded Linux.

---

## 1. Môi trường và Lệnh biên dịch chung

Mọi mã nguồn C trong kho lưu trữ này đều được biên dịch chéo bằng toolchain sinh ra từ Buildroot. Để đính kèm các thông tin gỡ lỗi (debugging symbols), cờ `-g` là **bắt buộc**.

**Cú pháp biên dịch chung trên Host:**
~~~bash
/home/vinh/buildroot_hdh/host/bin/arm-buildroot-linux-gnueabihf-gcc -g <ten_file.c> -o <ten_file_chay>
~~~

---

## 2. Chi tiết các Bài tập Thực hành

### Bài tập 2.1 & 2.2: Cài đặt gdbserver và Điều khiển luồng cơ bản (Cross-Debugging)

**Mục tiêu:** Sử dụng `gdbserver` trên Target và `gdb-multiarch` trên Host để điều khiển luồng chương trình qua mạng LAN/Serial.
<img width="772" height="113" alt="Screenshot from 2026-04-29 00-38-17" src="https://github.com/user-attachments/assets/609eddcb-a4ed-4765-8259-651db8e939ea" />

<img width="1386" height="270" alt="Screenshot from 2026-04-29 01-01-58" src="https://github.com/user-attachments/assets/2ad1db8d-3238-4a97-a9cb-8d99fd1e4d1d" />


**Mã nguồn (`main.c`):**
~~~c
#include <stdio.h>

int main() {
    int counter = 0;
    for(int i = 0; i < 3; i++) {
        counter += 10;
        printf("Lan %d: counter = %d\n", i, counter);
    }
    return 0;
}
~~~

**Các bước thực hiện:**
1. **Trên Target (BBB):** Khởi động gdbserver mở cổng `1234`.
~~~bash
gdbserver :1234 ./main_debug
~~~

2. **Trên Host (Ubuntu):** Khởi động gdb-multiarch, trỏ Sysroot về đúng thư mục Buildroot hiện tại và kết nối với Target.
~~~gdb
gdb-multiarch ./main_debug
(gdb) set sysroot /home/vinh/buildroot_hdh/host/arm-buildroot-linux-gnueabihf/sysroot
(gdb) target remote <192.168.10.105>:1234
~~~
<img width="962" height="407" alt="Screenshot from 2026-04-29 01-02-51" src="https://github.com/user-attachments/assets/5e500561-ebac-4d1e-9d21-16d071ffb5d5" />
<img width="1339" height="316" alt="Screenshot from 2026-04-29 01-06-10" src="https://github.com/user-attachments/assets/f7b5b1f2-44a0-45ec-8502-f08db8fb8ab3" />
**Các lệnh gỡ lỗi đã thực hành:**
* `b main` / `b 6`: Đặt Breakpoint tại hàm hoặc số dòng.
* `info b` / `d <số>`: Xem danh sách và xóa Breakpoint.
* `c` (continue), `n` (next), `s` (step): Thực thi điều khiển chạy chương trình.
* `p counter`: In giá trị biến.
* `set var counter=99`: Ép gán giá trị mới cho biến trong thời gian chạy.
* `info registers`: Xem trạng thái các thanh ghi hệ thống ARM.

<img width="707" height="762" alt="Screenshot from 2026-04-29 01-20-07" src="https://github.com/user-attachments/assets/6c1bc6be-da9c-4bd7-bf68-d78f5c421f33" />

<img width="693" height="896" alt="Screenshot from 2026-04-29 01-23-05" src="https://github.com/user-attachments/assets/729e2f0b-7f55-4e6a-a63e-7fa0ddedbed4" />

---


### Bài tập 2.3: Phân tích Bộ nhớ (Memory Leak) với Valgrind

**Mục tiêu:** Tạo chương trình cố ý rò rỉ bộ nhớ (cấp phát nhưng không thu hồi) và sử dụng Valgrind (Memcheck) để phát hiện.

**Mã nguồn (`bt3.c`):**
~~~c
#include <stdlib.h>
#include <stdio.h>

int main() {
    int *leak_ptr = (int *)malloc(100 * sizeof(int));
    if (leak_ptr != NULL) {
        leak_ptr[0] = 10;
        printf("Da cap phat bo nho nhung khong thu hoi.\n");
    }
    return 0;
}
~~~

**Kiểm tra với Valgrind trên Target:**
~~~bash
valgrind --leak-check=full ./bt23
~~~
*Kết quả ghi nhận:* Valgrind phát hiện lỗi `definitely lost: 400 bytes in 1 blocks`.  
*Vá lỗi:* Thêm lệnh `free(leak_ptr);` trước dòng `return 0;` và build lại.

<img width="1003" height="929" alt="Screenshot from 2026-04-29 01-32-20" src="https://github.com/user-attachments/assets/36640278-5f03-479f-a51d-effb48c7183c" />

---


### Bài tập 2.4: Phân tích Core Dump

**Mục tiêu:** Giám sát và sinh file báo cáo (core) khi chương trình gặp lỗi nghiêm trọng như Segmentation Fault.

**Mã nguồn (`bt4.c`):**
~~~c
#include <stddef.h>

void cause_crash() {
    int *ptr = NULL;
    *ptr = 42; // Cố tình gây lỗi Segmentation fault
}

int main() {
    cause_crash();
    return 0;
}
~~~

**Các bước thực hiện trên Target:**
1. Mở khóa giới hạn sinh file Core Dump của kernel (mặc định bị tắt):
~~~bash
ulimit -c unlimited
~~~
2. Thực thi chương trình để tạo lỗi:
~~~bash
./bt4
~~~
*Hệ thống báo cáo: `Segmentation fault (core dumped)` và sinh ra một file tên là `core` tại thư mục hiện hành.*

<img width="522" height="97" alt="Screenshot from 2026-04-29 01-35-34" src="https://github.com/user-attachments/assets/6f8ce56e-f88b-492d-b77c-cefb43b1556c" />

<img width="661" height="101" alt="Screenshot from 2026-04-29 01-37-59" src="https://github.com/user-attachments/assets/1d899397-10c9-4b79-ab52-5a3d19777923" />

---

### Bài tập 2.5: Phân tích Hiệu năng bằng Perf

**Mục tiêu:** Mô phỏng chương trình tải nặng và dùng `perf` để đo hiệu năng thực thi, sau đó xuất dữ liệu thô phục vụ phân tích.

**Mã nguồn (`bt5.c`):**
~~~c
#include <math.h>

int main() {
    double result = 0;
    for(long i = 0; i < 50000000; i++) {
        result += sin(i) * cos(i);
    }
    return 0;
}
~~~
*(Cần thêm cờ `-lm` khi biên dịch để link thư viện toán học).*

**Các bước thu thập dữ liệu trên Target:**
1. Dùng `perf record` để theo dõi và ghi lại hiệu năng:
~~~bash
perf record -g ./bt5
~~~
*(Sinh ra file `perf.data`)*
2. Dùng `perf script` để xuất dữ liệu đã thu thập ra định dạng text:
~~~bash
perf script > out.perf
~~~

<img width="876" height="480" alt="Screenshot from 2026-04-29 01-57-23" src="https://github.com/user-attachments/assets/32b330c2-84b3-409d-b812-c0188a6c5026" />

> **Tiến độ hiện tại:** Đã hoàn thành việc thu thập dữ liệu và sinh thành công file `out.perf`.  
