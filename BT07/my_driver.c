#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>       // Cho cap phat Major/Minor
#include <linux/cdev.h>     // Cho cau truc cdev
#include <linux/device.h>   // Cho class va device
#include <linux/uaccess.h>  // Cho copy_to_user, copy_from_user
#include <linux/slab.h>     // Cho kmalloc/kfree neu can

// Thong tin module
MODULE_LICENSE("GPL");
MODULE_AUTHOR("HDH");
MODULE_DESCRIPTION("Driver Character dau tien");

// Khai bao cac bien toan cuc
dev_t dev_num;
struct class *my_class;
struct device *my_device;
struct cdev my_cdev;

int current_count = 0; // Bien luu tru gia tri dem

// -----------------------------------------------------------
// 1. Cac ham xu ly: Open, Release, Read, Write
// -----------------------------------------------------------
static int my_driver_open(struct inode *inode, struct file *file) {
    pr_info("Driver: Thiet bi da duoc mo!\n");
    return 0;
}

static int my_driver_release(struct inode *inode, struct file *file) {
    pr_info("Driver: Thiet bi da duoc dong!\n");
    return 0;
}

static ssize_t my_driver_read(struct file *file, char __user *user_buffer, size_t count, loff_t *offs) {
    char buffer[32]; // Buffer tam de chuyen so thanh chuoi
    int len;

    // Neu da doc roi thi dung lai (tranh loop vo han khi dung lenh cat)
    if (*offs > 0) return 0;

    // Chuyen so nguyen thanh chuoi de gui len User Space
    len = snprintf(buffer, sizeof(buffer), "%d\n", current_count);

    // Gui len User Space
    if (copy_to_user(user_buffer, buffer, len)) {
        pr_err("Driver: Loi gui du lieu len User Space!\n");
        return -EFAULT;
    }

    pr_info("Driver: Da doc gia tri dem hien tai = %d\n", current_count);
    *offs += len; // Cap nhat vi tri con tro
    return len;
}

static ssize_t my_driver_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *offs) {
    char buffer[16];
    int target_number;
    int i;
    int copy_len = (count < sizeof(buffer)) ? count : (sizeof(buffer) - 1);

    // Nhan du lieu tu User Space
    if (copy_from_user(buffer, user_buffer, copy_len)) {
        pr_err("Driver: Loi nhan du lieu tu User Space!\n");
        return -EFAULT;
    }
    buffer[copy_len] = '\0'; // Them ky tu ket thuc chuoi

    // Chuyen chuoi thanh so nguyen (co so 10)
    if (kstrtoint(buffer, 10, &target_number) != 0) {
        pr_err("Driver: Gia tri khong hop le!\n");
        return -EINVAL;
    }

    // Gioi han so dem < 10 theo yeu cau
    if (target_number >= 10) target_number = 9;
    if (target_number < 0) target_number = 0;

    pr_info("Driver: Bat dau dem den %d\n", target_number);
    for (i = 0; i <= target_number; i++) {
        current_count = i;
        pr_info("Driver: Dem... %d\n", current_count);
    }

    return count;
}

// -----------------------------------------------------------
// 2. Dang ky fops
// -----------------------------------------------------------
static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = my_driver_open,
    .release = my_driver_release,
    .read    = my_driver_read,
    .write   = my_driver_write,
};

// -----------------------------------------------------------
// 3. Ham Khoi tao (__init)
// -----------------------------------------------------------
static int __init my_driver_init(void) {
    int ret;

    // 1. Cap phat Major/Minor tu dong
    ret = alloc_chrdev_region(&dev_num, 0, 1, "my_counter_device");
    if (ret < 0) return ret;
    pr_info("Driver: Major = %d, Minor = %d\n", MAJOR(dev_num), MINOR(dev_num));

    // 2. Khoi tao va them cdev (Lien ket fops voi Major/Minor)
    cdev_init(&my_cdev, &fops);
    ret = cdev_add(&my_cdev, dev_num, 1);
    if (ret < 0) goto r_cdev;

    // 3. Tao Class
    my_class = class_create("my_counter_class");
    if (IS_ERR(my_class)) goto r_class;

    // 4. Tao Device (Tao file /dev/my_counter)
    my_device = device_create(my_class, NULL, dev_num, NULL, "my_counter");
    if (IS_ERR(my_device)) goto r_device;

    pr_info("Driver: Da nap thanh cong! File thiet bi tao tai /dev/my_counter\n");
    return 0;

// Xu ly loi (don dep neu co buoc nao o tren that bai)
r_device:
    class_destroy(my_class);
r_class:
    cdev_del(&my_cdev);
r_cdev:
    unregister_chrdev_region(dev_num, 1);
    return -1;
}

// -----------------------------------------------------------
// 4. Ham Don dep (__exit)
// -----------------------------------------------------------
static void __exit my_driver_exit(void) {
    device_destroy(my_class, dev_num);      // Xoa file /dev/my_counter
    class_destroy(my_class);                // Xoa class
    cdev_del(&my_cdev);                     // Go bo cdev
    unregister_chrdev_region(dev_num, 1);   // Giai phong Major/Minor
    
    pr_info("Driver: Da go bo thanh cong!\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);
