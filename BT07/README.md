# BT07: Viết Character Device Driver cho BeagleBone Black (Buildroot)

---

## 1. Mã nguồn Driver (my_driver.c)

Driver thực hiện chức năng: Nhận một số từ User Space (< 10), thực hiện đếm trong Kernel và cho phép User Space đọc lại giá trị đếm hiện tại.

```c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>       
#include <linux/cdev.h>     
#include <linux/device.h>   
#include <linux/uaccess.h>  

// Thong tin Module
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vinh - PTIT");
MODULE_DESCRIPTION("Driver Character Counter cho BBB");

// Bien toan cuc
dev_t dev_num;
struct class *my_class;
struct device *my_device;
struct cdev my_cdev;
int current_count = 0;

// 1. Ham mo thiet bi
static int my_driver_open(struct inode *inode, struct file *file) {
    pr_info("Driver: Thiet bi da mo thanh cong!\n");
    return 0;
}

// 2. Ham dong thiet bi
static int my_driver_release(struct inode *inode, struct file *file) {
    pr_info("Driver: Thiet bi da dong!\n");
    return 0;
}

// 3. Ham doc: Tra ve gia tri current_count cho User Space
static ssize_t my_driver_read(struct file *file, char __user *user_buffer, size_t count, loff_t *offs) {
    char buffer[16];
    int len;
    if (*offs > 0) return 0;
    len = snprintf(buffer, sizeof(buffer), "%d\n", current_count);
    if (copy_to_user(user_buffer, buffer, len)) return -EFAULT;
    *offs += len;
    return len;
}

// 4. Ham ghi: Nhan so tu User Space de Kernel thuc hien dem
static ssize_t my_driver_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *offs) {
    char buffer[16];
    int target, i;
    int bytes_to_copy = (count < 16) ? count : 15;
    
    if (copy_from_user(buffer, user_buffer, bytes_to_copy)) return -EFAULT;
    buffer[bytes_to_copy] = '\0';

    if (kstrtoint(buffer, 10, &target) == 0) {
        if (target >= 10) target = 9; // Gioi han < 10
        pr_info("Driver: Bat dau dem den %d\n", target);
        for (i = 0; i <= target; i++) {
            current_count = i;
            pr_info("Driver: Dang dem... %d\n", current_count);
        }
    }
    return count;
}

// Lien ket cac ham vao struct file_operations
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = my_driver_open,
    .release = my_driver_release,
    .read = my_driver_read,
    .write = my_driver_write,
};

// 5. Ham khoi tao Driver
static int __init my_driver_init(void) {
    // 1. Cap phat Major/Minor tu dong
    if (alloc_chrdev_region(&dev_num, 0, 1, "my_counter_device") < 0) return -1;
    
    // 2. Dang ky cdev (Lien ket fops voi Major/Minor)
    cdev_init(&my_cdev, &fops);
    if (cdev_add(&my_cdev, dev_num, 1) < 0) goto r_class;

    // 3. Tao Class (Kernel 6.4+ chi dung 1 tham so)
    my_class = class_create("my_counter_class");
    if (IS_ERR(my_class)) goto r_cdev;

    // 4. Tao Device File (/dev/my_counter)
    my_device = device_create(my_class, NULL, dev_num, NULL, "my_counter");
    if (IS_ERR(my_device)) goto r_device;

    pr_info("Driver: Da nap! Major = %d, Minor = %d\n", MAJOR(dev_num), MINOR(dev_num));
    return 0;

r_device:
    class_destroy(my_class);
r_cdev:
    cdev_del(&my_cdev);
r_class:
    unregister_chrdev_region(dev_num, 1);
    return -1;
}

// 6. Ham go bo Driver
static void __exit my_driver_exit(void) {
    device_destroy(my_class, dev_num);
    class_destroy(my_class);
    cdev_del(&my_cdev);
    unregister_chrdev_region(dev_num, 1);
    pr_info("Driver: Da go bo an toan!\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);
```

---

## 2. Quy trình Biên dịch (Makefile)

Thực hiện trên máy tính Ubuntu (Host) sử dụng Toolchain của Buildroot.

```makefile
obj-m += my_driver.o

# Duong dan KDIR va Toolchain (Sua lai cho dung voi may cua ban)
KDIR = /home/vinh/buildroot_hdh/output/build/linux-6.6.121
TOOLCHAIN = /home/vinh/buildroot_hdh/output/host/bin/arm-buildroot-linux-gnueabihf-

all:
	make ARCH=arm CROSS_COMPILE=$(TOOLCHAIN) -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
```

**Lệnh thực hiện:** Gõ `make` trong thư mục chứa file. Kết quả thu được file `my_driver.ko`.

---

## 3. Triển khai trên BeagleBone Black

### Bước 1: Nạp Module vào Kernel
```bash
insmod my_driver.ko
dmesg | tail -n 5  # Kiem tra so Major thuc te (Vi du: 247)
```

### Bước 2: Tạo Device Node thủ công
Do môi trường Buildroot tối giản, cần tự tạo file trong `/dev` tương ứng với số Major nhận được:
```bash
mknod /dev/my_counter c <SO_MAJOR> 0
# Vi du: mknod /dev/my_counter c 247 0
```

### Bước 3: Thử nghiệm chức năng
1. **Ghi dữ liệu (Gửi lệnh đếm):**
   `echo "5" > /dev/my_counter`
   -> Dùng lệnh `dmesg | tail -n 10` để xem Kernel đếm từ 0 đến 5.
2. **Đọc dữ liệu (Xem giá trị hiện tại):**
   `cat /dev/my_counter`
   -> Màn hình sẽ hiển thị số `5`.
<img width="818" height="467" alt="image" src="https://github.com/user-attachments/assets/9fc3bab4-45b6-41bc-9111-a1fbd2fb474b" />

---

## ⚠️ Lưu ý (Notes)
* **Kernel 6.6:** Hàm `class_create` đã thay đổi, chỉ nhận 1 tham số là tên class.
* **Buildroot Nodes:** File trong `/dev/` không tự sinh ra nếu thiếu `udev/mdev`, phải dùng `mknod`.
* **Cấp phát động:** Số Major có thể thay đổi sau mỗi lần reboot hoặc nạp lại driver, cần kiểm tra bằng `cat /proc/devices`.
* **An toàn bộ nhớ:** Luôn dùng `copy_from_user` để tránh làm hỏng vùng nhớ Kernel khi nhận dữ liệu từ User Space.
