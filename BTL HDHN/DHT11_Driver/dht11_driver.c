/*
 * DHT11 Temperature & Humidity Kernel Driver cho BeagleBone Black
 * Chân: P8_7 → GPIO2_2 (số tuyến tính: 66)
 *
 * Tại sao P8_7?
 *   - P8_1..2   : GND
 *   - P8_3..10  : eMMC (MMC1) → TRÁNH
 *   - P8_11..26 : eMMC còn lại + safe GPIO
 *   - P8_27..46 : HDMI → TRÁNH
 *   - P8_7 (GPIO2_2): HOÀN TOÀN tự do, không đụng eMMC/HDMI/PWM/ADC đã dùng
 *
 * Giao thức DHT11 (1-wire, theo datasheet Aosong):
 *   1. Host kéo LOW ≥ 18ms  → "Start signal"
 *   2. Host thả, pull-up lên HIGH 20–40µs
 *   3. DHT11 kéo LOW 80µs   → "Response"
 *   4. DHT11 kéo HIGH 80µs  → "Chuẩn bị gửi"
 *   5. 40 bit dữ liệu, mỗi bit:
 *        - 50µs LOW (preamble)
 *        - HIGH 26–28µs = bit '0'
 *        - HIGH 70µs    = bit '1'
 *   6. 40 bit = [Humi_int][Humi_dec][Temp_int][Temp_dec][Checksum]
 *
 * Vấn đề timing trong kernel Linux:
 *   DHT11 cần đo thời gian µs chính xác. Kernel không phải RTOS nên
 *   sẽ có jitter từ scheduler. Giải pháp:
 *     - Dùng local_irq_disable() trong suốt quá trình đọc 40 bit
 *       để tránh interrupt làm lệch timing
 *     - Đo bằng ktime_get() (hardware clock, không bị jitter)
 *     - Ngưỡng phân biệt bit: 40µs (giữa 26µs và 70µs)
 *
 * Tham chiếu:
 *   - DHT11 Datasheet (Aosong Electronics)
 *   - AM335x TRM chương 25 (GPIO)
 *   - BBB System Reference Manual, Table 12 (P8 pinout)
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>
#include <linux/irqflags.h>

#define DRIVER_NAME "dht11"
#define CLASS_NAME  "dht11_class"

/* ================================================================
 * Chân: P8_7 → GPIO2_2
 *
 * GPIO AM335x:
 *   GPIO bank = 2  → base address 0x481A_C000
 *   GPIO pin  = 2  → bit 2 của các thanh ghi
 *   GPIO tuyến tính = 2*32 + 2 = 66
 * ================================================================ */

/* Clock Manager: cấp clock cho GPIO2 */
#define CM_PER_BASE              0x44E00000
#define CM_PER_GPIO2_CLKCTRL     0xB0   /* TRM Table 8-153 */

/* Control Module: pinmux */
#define CTRL_MOD_BASE            0x44E10000
/*
 * P8_7 = gpmc_advn_ale → conf offset 0x890 (TRM Table 9-10)
 * Pinmux value: mode7 (GPIO), pull-up enable, input enable
 *   [2:0] = 111 (mode 7 = GPIO)
 *   [3]   = 1   (pull type = pull-up)
 *   [4]   = 0   (pull enable = ON)
 *   [5]   = 1   (input enable = ON, cần cho 1-wire)
 *   → 0b10_1111 = 0x2F
 * Khi cần output: tắt input enable (bit 5 = 0) → 0x0F
 */
#define P8_7_MUX_OFFSET          0x890
#define P8_7_MUX_INPUT           0x2F   /* mode7, pull-up, input  */
#define P8_7_MUX_OUTPUT          0x0F   /* mode7, pull-up, output */

/* GPIO2 base physical address (TRM Table 2-3) */
#define GPIO2_BASE_PHYS          0x481AC000
#define GPIO_MAP_SIZE            0x1000

/* GPIO Register offsets (TRM Table 25-5) */
#define GPIO_REVISION            0x00
#define GPIO_OE                  0x134  /* Output Enable: 1=input, 0=output */
#define GPIO_DATAIN              0x138  /* Đọc mức tín hiệu vào */
#define GPIO_DATAOUT             0x13C  /* Ghi mức tín hiệu ra */
#define GPIO_SETDATAOUT          0x194  /* Set bit = kéo HIGH */
#define GPIO_CLEARDATAOUT        0x190  /* Set bit = kéo LOW  */

/* Bit mask cho GPIO2_2 */
#define DHT11_PIN_MASK           (1U << 2)

/* ================================================================
 * Timing DHT11 (µs), theo datasheet Aosong
 * ================================================================ */
#define DHT11_START_LOW_US       20000  /* Host kéo LOW ≥ 18ms, dùng 20ms */
#define DHT11_START_HIGH_US      40    /* Host thả, chờ 20–40µs           */
#define DHT11_RESPONSE_TIMEOUT   200   /* Timeout chờ response DHT11       */
#define DHT11_BIT_THRESHOLD_US   40    /* <40µs = '0', >40µs = '1'         */
#define DHT11_BIT_TIMEOUT_US     100   /* Timeout đọc 1 bit                */

/* Khoảng cách tối thiểu giữa 2 lần đọc: 1 giây */
#define DHT11_MIN_INTERVAL_MS    1000

/* ================================================================
 * Biến toàn cục
 * ================================================================ */
static dev_t dev_num;
static struct cdev dht11_cdev;
static struct class *dht11_class;
static void __iomem *gpio2_base;

/* Cache kết quả đọc gần nhất (tránh đọc liên tục) */
static int g_temp = -1;
static int g_humi = -1;
static ktime_t g_last_read;

/* ================================================================
 * GPIO helpers inline
 * ================================================================ */

/* Đặt chân thành OUTPUT */
static inline void gpio_set_output(void)
{
    u32 val = readl(gpio2_base + GPIO_OE);
    val &= ~DHT11_PIN_MASK;   /* bit=0 → output */
    writel(val, gpio2_base + GPIO_OE);
}

/* Đặt chân thành INPUT */
static inline void gpio_set_input(void)
{
    u32 val = readl(gpio2_base + GPIO_OE);
    val |= DHT11_PIN_MASK;    /* bit=1 → input */
    writel(val, gpio2_base + GPIO_OE);
}

/* Kéo chân HIGH */
static inline void gpio_set_high(void)
{
    writel(DHT11_PIN_MASK, gpio2_base + GPIO_SETDATAOUT);
}

/* Kéo chân LOW */
static inline void gpio_set_low(void)
{
    writel(DHT11_PIN_MASK, gpio2_base + GPIO_CLEARDATAOUT);
}

/* Đọc mức chân */
static inline int gpio_read(void)
{
    return (readl(gpio2_base + GPIO_DATAIN) & DHT11_PIN_MASK) ? 1 : 0;
}

/* ================================================================
 * Chờ chân đạt mức mong muốn, trả về thời gian chờ (µs)
 * hoặc -1 nếu timeout
 * PHẢI gọi trong vùng irq disabled
 * ================================================================ */
static int wait_for_level(int level, int timeout_us)
{
    ktime_t start = ktime_get();
    while (gpio_read() != level) {
        s64 elapsed = ktime_to_us(ktime_sub(ktime_get(), start));
        if (elapsed >= timeout_us)
            return -1;
    }
    return (int)ktime_to_us(ktime_sub(ktime_get(), start));
}

/* ================================================================
 * Đọc 40 bit dữ liệu từ DHT11
 * Trả về 0 nếu thành công, âm nếu lỗi
 *
 * Protocol chi tiết (datasheet Aosong, trang 4–5):
 *   Bước 1: Host → DHT11: START
 *     - Output LOW 20ms
 *     - Output HIGH rồi chuyển Input (pull-up giữ HIGH)
 *   Bước 2: DHT11 → Host: RESPONSE
 *     - LOW 80µs, HIGH 80µs
 *   Bước 3: DHT11 gửi 40 bit
 *     - Mỗi bit: 50µs LOW (preamble) + HIGH (26µs='0', 70µs='1')
 *   Bước 4: DHT11 kéo LOW 50µs rồi thả (end of transmission)
 * ================================================================ */
static int dht11_read_raw(u8 data[5])
{
    int i, bit;
    unsigned long flags;

    /* --- BƯỚC 1: Gửi START signal ---
     * Không cần disable IRQ ở bước này vì msleep không care IRQ
     * và 18ms >> jitter của scheduler
     */
    gpio_set_output();
    gpio_set_low();
    msleep(20);                /* Kéo LOW 20ms (≥18ms) */
    gpio_set_high();
    udelay(30);                /* Chờ 20–40µs trước khi thả */
    gpio_set_input();          /* Thả bus, pull-up kéo HIGH */

    /* --- TỪ ĐÂY: DISABLE IRQ để timing chính xác --- */
    local_irq_save(flags);

    /* --- BƯỚC 2a: Chờ DHT11 kéo LOW (response bắt đầu) --- */
    if (wait_for_level(0, DHT11_RESPONSE_TIMEOUT) < 0) {
        local_irq_restore(flags);
        printk(KERN_WARNING "DHT11: Khong nhan duoc response LOW\n");
        return -ETIMEDOUT;
    }

    /* --- BƯỚC 2b: Chờ DHT11 kéo HIGH (response kết thúc, 80µs LOW xong) --- */
    if (wait_for_level(1, DHT11_RESPONSE_TIMEOUT) < 0) {
        local_irq_restore(flags);
        printk(KERN_WARNING "DHT11: Khong nhan duoc response HIGH\n");
        return -ETIMEDOUT;
    }

    /* --- BƯỚC 2c: Chờ DHT11 kéo LOW lần nữa (80µs HIGH xong, bắt đầu data) --- */
    if (wait_for_level(0, DHT11_RESPONSE_TIMEOUT) < 0) {
        local_irq_restore(flags);
        printk(KERN_WARNING "DHT11: Khong vao duoc trang thai data\n");
        return -ETIMEDOUT;
    }

    /* --- BƯỚC 3: Đọc 40 bit --- */
    memset(data, 0, 5);

    for (i = 0; i < 40; i++) {
        int high_duration;

        /* Chờ preamble LOW 50µs kết thúc (lên HIGH) */
        if (wait_for_level(1, DHT11_BIT_TIMEOUT_US) < 0) {
            local_irq_restore(flags);
            printk(KERN_WARNING "DHT11: Timeout cho bit %d preamble\n", i);
            return -ETIMEDOUT;
        }

        /* Đo thời gian HIGH để xác định bit 0 hay 1 */
        high_duration = wait_for_level(0, DHT11_BIT_TIMEOUT_US);
        if (high_duration < 0) {
            local_irq_restore(flags);
            printk(KERN_WARNING "DHT11: Timeout do bit %d\n", i);
            return -ETIMEDOUT;
        }

        /*
         * Phân biệt bit:
         *   HIGH < 40µs → bit '0'  (datasheet: 26–28µs)
         *   HIGH ≥ 40µs → bit '1'  (datasheet: 70µs)
         * Ngưỡng 40µs nằm giữa 28µs và 70µs → an toàn
         */
        bit = (high_duration >= DHT11_BIT_THRESHOLD_US) ? 1 : 0;

        /* Ghi bit vào mảng data, MSB first */
        data[i / 8] <<= 1;
        data[i / 8] |= bit;
    }

    local_irq_restore(flags);

    /* --- BƯỚC 4: Kiểm tra checksum ---
     * Byte thứ 5 = tổng 4 byte đầu (8 bit thấp)
     */
    if (data[4] != (u8)(data[0] + data[1] + data[2] + data[3])) {
        printk(KERN_WARNING "DHT11: Checksum sai! Got=0x%02X, Expect=0x%02X\n",
               data[4], (u8)(data[0] + data[1] + data[2] + data[3]));
        printk(KERN_WARNING "DHT11: Raw bytes: %02X %02X %02X %02X %02X\n",
               data[0], data[1], data[2], data[3], data[4]);
        return -EIO;
    }

    return 0;
}

/* ================================================================
 * File operation: read
 * Trả về: "Nhiet do: XX C | Do am: YY %\n"
 * Cache 1 giây để tránh đọc liên tục làm lỗi giao thức DHT11
 * ================================================================ */
static ssize_t dht11_read(struct file *file, char __user *user_buf,
                           size_t size, loff_t *offset)
{
    u8 data[5];
    char kbuf[80];
    int len, ret;
    ktime_t now;

    if (*offset > 0)
        return 0;

    /* Kiểm tra cache: nếu đọc trong vòng 1 giây → trả về cache */
    now = ktime_get();
    if (g_temp >= 0 &&
        ktime_to_ms(ktime_sub(now, g_last_read)) < DHT11_MIN_INTERVAL_MS) {
        len = snprintf(kbuf, sizeof(kbuf),
                       "Nhiet do: %d C | Do am: %d %%\n",
                       g_temp, g_humi);
        goto out;
    }

    /* Đọc thật từ sensor */
    ret = dht11_read_raw(data);
    if (ret < 0) {
        if (g_temp >= 0) {
            /* Trả về giá trị cũ nếu có, kèm cảnh báo */
            len = snprintf(kbuf, sizeof(kbuf),
                           "Nhiet do: %d C | Do am: %d %% (du lieu cu, loi doc: %d)\n",
                           g_temp, g_humi, ret);
        } else {
            len = snprintf(kbuf, sizeof(kbuf),
                           "LOI: Khong doc duoc DHT11 (ma loi: %d)\n", ret);
        }
        goto out;
    }

    /*
     * Giải mã dữ liệu DHT11:
     *   data[0] = Humidity integer
     *   data[1] = Humidity decimal (thường = 0 với DHT11)
     *   data[2] = Temperature integer
     *   data[3] = Temperature decimal (thường = 0 với DHT11)
     *   data[4] = Checksum
     */
    g_humi = data[0];
    g_temp = data[2];
    g_last_read = ktime_get();

    len = snprintf(kbuf, sizeof(kbuf),
                   "Nhiet do: %d C | Do am: %d %%\n",
                   g_temp, g_humi);

out:
    if (copy_to_user(user_buf, kbuf, len))
        return -EFAULT;
    *offset += len;
    return len;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .read  = dht11_read,
};

/* ================================================================
 * Module Init
 * ================================================================ */
static int __init dht11_init(void)
{
    void __iomem *cm_per_mem;
    void __iomem *ctrl_mod_mem;

    printk(KERN_INFO "DHT11: Dang khoi tao driver (P8_7 = GPIO2_2)...\n");

    /* --- 1. Cấp clock cho GPIO2 --- */
    cm_per_mem = ioremap(CM_PER_BASE, SZ_4K);
    if (!cm_per_mem) return -ENOMEM;

    writel(0x02, cm_per_mem + CM_PER_GPIO2_CLKCTRL);
    /* Chờ IDLEST = 0 (FULLY_FUNCTIONAL) */
    while (readl(cm_per_mem + CM_PER_GPIO2_CLKCTRL) & (0x3 << 16))
        cpu_relax();
    iounmap(cm_per_mem);

    /* --- 2. Pinmux P8_7 → GPIO mode, pull-up, input --- */
    ctrl_mod_mem = ioremap(CTRL_MOD_BASE, SZ_8K);
    if (!ctrl_mod_mem) return -ENOMEM;
    writel(P8_7_MUX_INPUT, ctrl_mod_mem + P8_7_MUX_OFFSET);
    iounmap(ctrl_mod_mem);

    /* --- 3. Map GPIO2 registers --- */
    gpio2_base = ioremap(GPIO2_BASE_PHYS, GPIO_MAP_SIZE);
    if (!gpio2_base) return -ENOMEM;

    /* --- 4. Cấu hình ban đầu: input (idle state của DHT11 là HIGH) --- */
    gpio_set_input();
    gpio_set_high(); /* Đảm bảo DATAOUT=1 trước khi chuyển output */

    /* --- 5. Đăng ký character device --- */
    if (alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME) < 0) {
        iounmap(gpio2_base);
        return -ENODEV;
    }

    cdev_init(&dht11_cdev, &fops);
    if (cdev_add(&dht11_cdev, dev_num, 1) < 0) {
        unregister_chrdev_region(dev_num, 1);
        iounmap(gpio2_base);
        return -ENODEV;
    }

    dht11_class = class_create(CLASS_NAME);
    if (IS_ERR(dht11_class)) {
        cdev_del(&dht11_cdev);
        unregister_chrdev_region(dev_num, 1);
        iounmap(gpio2_base);
        return PTR_ERR(dht11_class);
    }

    device_create(dht11_class, NULL, dev_num, NULL, DRIVER_NAME);

    g_last_read = ktime_set(0, 0); /* Reset cache */

    printk(KERN_INFO "DHT11: San sang! Doc: cat /dev/%s\n", DRIVER_NAME);
    printk(KERN_INFO "DHT11: Dau day: VCC->3.3V(P8_3), GND->P8_1, DATA->P8_7\n");
    printk(KERN_INFO "DHT11: Nho cam dien tro pull-up 4.7k-10k giua DATA va VCC!\n");
    return 0;
}

/* ================================================================
 * Module Exit
 * ================================================================ */
static void __exit dht11_exit(void)
{
    if (gpio2_base) {
        gpio_set_input(); /* Trả chân về input khi unload */
        iounmap(gpio2_base);
    }

    device_destroy(dht11_class, dev_num);
    class_destroy(dht11_class);
    cdev_del(&dht11_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "DHT11: Da go bo driver.\n");
}

module_init(dht11_init);
module_exit(dht11_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DHT11 Driver for BeagleBone Black - P8_7 (GPIO2_2)");
