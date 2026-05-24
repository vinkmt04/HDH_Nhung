/*
 * Driver đọc ADC thô (Raw) cho MQ2 trên BeagleBone Black (AM335x TSC_ADC_SS)
 * Chân cấu hình: AIN0
 * Output: Giá trị ADC từ 0 đến 4095
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define DRIVER_NAME "mq2_adc"
#define CLASS_NAME  "mq2_class"

/* ================================================================
 * Địa chỉ vật lý AM335x
 * ================================================================ */
#define CM_WKUP_BASE  0x44E00400
#define ADC_BASE      0x44E0D000
#define CM_WKUP_SIZE  0x100
#define ADC_SIZE      0x2000

/* ================================================================
 * Register offsets
 * ================================================================ */
#define CM_WKUP_ADC_CLKCTRL_OFFSET  0xBC
#define IRQENABLE_CLR_OFFSET        0x30
#define ADC_CTRL_OFFSET             0x40
#define STEPENABLE_OFFSET           0x54
#define STEPCONFIG1_OFFSET          0x64
#define STEPDELAY1_OFFSET           0x68
#define FIFO0COUNT_OFFSET           0xE4
#define FIFO0DATA_OFFSET            0x100

/* ================================================================
 * CTRL bits & Cấu hình Step
 * ================================================================ */
#define CTRL_ENABLE                     BIT(0)
#define CTRL_STEPCONFIG_WRITEPROTECT_N  BIT(2)

/*
 * STEPCONFIG1 = 0x00008010
 * [1:0]   MODE = 00 (SW one-shot)
 * [4:2]   AVG  = 100 (16 mẫu)
 * [15:12] INM  = 1000 (ADCREFM)
 * [19:16] INP  = 0000 (AIN0)
 */
#define STEPCONFIG1_VAL  ((4 << 2) | (8 << 12)) 
#define STEPDELAY1_VAL   0x00000098

#define ADC_TIMEOUT_US    10000

/* ================================================================
 * Biến toàn cục module
 * ================================================================ */
static void __iomem *cm_wkup_base;
static void __iomem *adc_base;
static dev_t dev_num;
static struct cdev mq2_cdev;
static struct class *mq2_class;

/* ================================================================
 * Hàm đọc 1 mẫu ADC raw (12-bit)
 * ================================================================ */
static int adc_read_raw(void)
{
    unsigned int count;
    int timeout = ADC_TIMEOUT_US;

    /* Xả FIFO rác */
    while (readl(adc_base + FIFO0COUNT_OFFSET) > 0)
        readl(adc_base + FIFO0DATA_OFFSET);

    /* Kích hoạt step 1 */
    writel(BIT(1), adc_base + STEPENABLE_OFFSET);

    /* Chờ có dữ liệu với timeout */
    do {
        udelay(1);
        count = readl(adc_base + FIFO0COUNT_OFFSET);
        if (--timeout <= 0) {
            printk(KERN_ERR "MQ2: Timeout cho FIFO ADC!\n");
            return -ETIMEDOUT;
        }
    } while (count == 0);

    return (int)(readl(adc_base + FIFO0DATA_OFFSET) & 0xFFF);
}

/* ================================================================
 * File operation: read (Chỉ trả về giá trị số nguyên)
 * ================================================================ */
static ssize_t mq2_read(struct file *file, char __user *user_buf,
                         size_t size, loff_t *offset)
{
    int raw;
    char kbuf[16];
    int len;

    if (*offset > 0)
        return 0;

    raw = adc_read_raw();
    if (raw < 0)
        return raw; // Trả về mã lỗi nếu timeout

    // Chỉ đóng gói con số ADC thô vào chuỗi (Ví dụ: "2048\n")
    len = snprintf(kbuf, sizeof(kbuf), "%d\n", raw);

    if (copy_to_user(user_buf, kbuf, len))
        return -EFAULT;

    *offset += len;
    return len;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read  = mq2_read,
};

/* ================================================================
 * Module Init
 * ================================================================ */
static int __init mq2_adc_init(void)
{
    unsigned int ctrl_val;

    printk(KERN_INFO "MQ2 ADC: Dang khoi tao driver (Raw Mode)...\n");

    adc_base     = ioremap(ADC_BASE, ADC_SIZE);
    cm_wkup_base = ioremap(CM_WKUP_BASE, CM_WKUP_SIZE);

    if (!adc_base || !cm_wkup_base) {
        printk(KERN_ERR "MQ2: ioremap that bai!\n");
        if (adc_base)     iounmap(adc_base);
        if (cm_wkup_base) iounmap(cm_wkup_base);
        return -ENOMEM;
    }

    /* 1. Cấp clock ADC */
    writel(0x02, cm_wkup_base + CM_WKUP_ADC_CLKCTRL_OFFSET);
    while (readl(cm_wkup_base + CM_WKUP_ADC_CLKCTRL_OFFSET) & (0x3 << 16))
        cpu_relax();

    /* 2. Tắt toàn bộ interrupt kernel */
    writel(0x7FF, adc_base + IRQENABLE_CLR_OFFSET);

    /* 3. Tắt ADC trước khi cấu hình */
    ctrl_val = readl(adc_base + ADC_CTRL_OFFSET);
    ctrl_val &= ~CTRL_ENABLE;
    writel(ctrl_val, adc_base + ADC_CTRL_OFFSET);

    /* 4. Mở khóa và cấu hình Step 1 */
    ctrl_val |= CTRL_STEPCONFIG_WRITEPROTECT_N;
    writel(ctrl_val, adc_base + ADC_CTRL_OFFSET);

    writel(STEPCONFIG1_VAL, adc_base + STEPCONFIG1_OFFSET);
    writel(STEPDELAY1_VAL,  adc_base + STEPDELAY1_OFFSET);

    /* 5. Khóa lại và Bật ADC */
    ctrl_val &= ~CTRL_STEPCONFIG_WRITEPROTECT_N;
    writel(ctrl_val, adc_base + ADC_CTRL_OFFSET);

    ctrl_val |= CTRL_ENABLE;
    writel(ctrl_val, adc_base + ADC_CTRL_OFFSET);
    udelay(100);

    /* 6. Đăng ký character device */
    if (alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME) < 0) {
        iounmap(adc_base);
        iounmap(cm_wkup_base);
        return -ENODEV;
    }

    cdev_init(&mq2_cdev, &fops);
    if (cdev_add(&mq2_cdev, dev_num, 1) < 0) {
        unregister_chrdev_region(dev_num, 1);
        iounmap(adc_base);
        iounmap(cm_wkup_base);
        return -ENODEV;
    }

    mq2_class = class_create(CLASS_NAME);
    if (IS_ERR(mq2_class)) {
        cdev_del(&mq2_cdev);
        unregister_chrdev_region(dev_num, 1);
        iounmap(adc_base);
        iounmap(cm_wkup_base);
        return PTR_ERR(mq2_class);
    }

    device_create(mq2_class, NULL, dev_num, NULL, DRIVER_NAME);

    printk(KERN_INFO "MQ2 ADC: San sang! Node: /dev/%s\n", DRIVER_NAME);
    return 0;
}

/* ================================================================
 * Module Exit
 * ================================================================ */
static void __exit mq2_adc_exit(void)
{
    device_destroy(mq2_class, dev_num);
    class_destroy(mq2_class);
    cdev_del(&mq2_cdev);
    unregister_chrdev_region(dev_num, 1);

    if (adc_base) {
        unsigned int ctrl_val = readl(adc_base + ADC_CTRL_OFFSET);
        ctrl_val &= ~CTRL_ENABLE;
        writel(ctrl_val, adc_base + ADC_CTRL_OFFSET);
        iounmap(adc_base);
    }
    if (cm_wkup_base)
        iounmap(cm_wkup_base);

    printk(KERN_INFO "MQ2 ADC: Da go bo khoi he thong.\n");
}

module_init(mq2_adc_init);
module_exit(mq2_adc_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MQ2 Raw ADC Driver for BeagleBone Black");
