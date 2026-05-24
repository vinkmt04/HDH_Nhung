#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/gpio.h>
#include <linux/version.h>
#include <linux/io.h>

#define DRIVER_NAME "button_driver"
#define CLASS_NAME  "button_class"
#define BUTTON_GPIO 524     // Chân P8_12

#define CTRL_MOD_BASE      0x44E10000 
#define P8_12_MUX_OFFSET   0x830

static dev_t dev_num;
static struct cdev btn_cdev;
static struct class *btn_class;
static int irq_num;

DECLARE_WAIT_QUEUE_HEAD(wq);
static int event_flag = 0;   // Cờ báo hiệu có sự kiện mới
static int button_state = 0; // Trạng thái: 1 = Đang nhấn, 0 = Đã nhả
static unsigned long last_jiffies = 0;

// ====================================================================
// HÀM PHỤC VỤ NGẮT (ISR)
// ====================================================================
static irqreturn_t button_isr(int irq, void *dev_id) {
    // Chống dội phần mềm (Cooldown 50ms để không lỡ các cú click nhanh)
    if (last_jiffies == 0 || time_after(jiffies, last_jiffies + msecs_to_jiffies(50))) {
        int raw_val = gpio_get_value(BUTTON_GPIO);
        
        // Chân cấu hình Pull-up: Nhấn = 0, Nhả = 1
        // Ta đảo lại giá trị cho dễ hiểu ở User-space: 1 = Nhấn, 0 = Nhả
        button_state = (raw_val == 0) ? 1 : 0; 
        event_flag = 1;                   
        
        wake_up_interruptible(&wq);       
        last_jiffies = jiffies;           
    }
    return IRQ_HANDLED;
}

// ====================================================================
// HÀM ĐỌC DỮ LIỆU TỪ USER SPACE
// ====================================================================
static ssize_t button_read(struct file *file, char __user *user_buf, size_t size, loff_t *offset) {
    char kbuf[8];
    int len;

    if (*offset > 0) return 0;

    // Ngủ đông chờ cờ báo hiệu có sự kiện thay đổi trạng thái
    if (wait_event_interruptible(wq, event_flag != 0)) {
        return -ERESTARTSYS;
    }

    event_flag = 0; // Xóa cờ để chờ sự kiện tiếp theo
    
    // Đóng gói dữ liệu ("1\n" hoặc "0\n")
    len = snprintf(kbuf, sizeof(kbuf), "%d\n", button_state);

    if (copy_to_user(user_buf, kbuf, len)) {
        return -EFAULT;
    }

    *offset += len;
    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = button_read,
};

// ====================================================================
// HÀM KHỞI TẠO MODULE (INIT)
// ====================================================================
static int __init button_init(void) {
    int ret;
    void __iomem *ctrl_mod_base;

    printk(KERN_INFO "Button Driver: Dang khoi tao...\n");

    // Ép Pinmux P8_12 (Mode 7, RX Enable, Pull-up)
    ctrl_mod_base = ioremap(CTRL_MOD_BASE, 0x2000);
    if (ctrl_mod_base) {
        writel(0x37, ctrl_mod_base + P8_12_MUX_OFFSET);
        iounmap(ctrl_mod_base);
    }

    if (!gpio_is_valid(BUTTON_GPIO)) {
        printk(KERN_ERR "Button Driver: GPIO %d khong hop le\n", BUTTON_GPIO);
        return -ENODEV;
    }

    ret = gpio_request(BUTTON_GPIO, "smart_button_pin");
    if (ret) {
        printk(KERN_ERR "Button Driver: Khong the request GPIO\n");
        return ret;
    }

    gpio_direction_input(BUTTON_GPIO);
    irq_num = gpio_to_irq(BUTTON_GPIO);

    // QUAN TRỌNG: Đăng ký bắt ngắt ở CẢ HAI CẠNH (Lên và Xuống)
    ret = request_irq(irq_num, button_isr, IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "smart_button_irq", NULL);
    if (ret) {
        printk(KERN_ERR "Button Driver: Khong the dang ky ngat\n");
        gpio_free(BUTTON_GPIO);
        return ret;
    }

    alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME);
    cdev_init(&btn_cdev, &fops);
    cdev_add(&btn_cdev, dev_num, 1);

    btn_class = class_create(CLASS_NAME);
    device_create(btn_class, NULL, dev_num, NULL, DRIVER_NAME);

    printk(KERN_INFO "Button Driver: San sang! Node: /dev/%s\n", DRIVER_NAME);
    return 0;
}

// ====================================================================
// HÀM HỦY MODULE (EXIT)
// ====================================================================
static void __exit button_exit(void) {
    free_irq(irq_num, NULL);
    gpio_free(BUTTON_GPIO);
    device_destroy(btn_class, dev_num);
    class_destroy(btn_class);
    cdev_del(&btn_cdev);
    unregister_chrdev_region(dev_num, 1);
    
    printk(KERN_INFO "Button Driver: Da go bo khoi he thong!\n");
}

module_init(button_init);
module_exit(button_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vinh");
MODULE_DESCRIPTION("Smart Button Driver - Edge Triggered");
