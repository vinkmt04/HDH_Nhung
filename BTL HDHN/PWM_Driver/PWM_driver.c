/*
 * PWM Driver cho BeagleBone Black (AM335x eHRPWM)
 * - PWM0A (P9_22): Còi buzzer
 * - PWM1A (P9_14): Quạt DC
 *
 * Tham chiếu:
 *   - AM335x TRM chương 15 (ePWM)
 *   - TI E2E forum: e2e.ti.com/f/670544 (lỗi ghi TBPRD/CMPA)
 *   - theduchy.ualr.edu/?p=299 (TBCTL / AQCTLA setup)
 *   - TI pinmux tool: GPMC_A2 → offset 0x848, SPI0_SCLK → offset 0x950
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>

#define DRIVER_NAME "pwm_driver"
#define CLASS_NAME  "pwm_class"

/* ================================================================
 * Clock Manager (CM_PER)
 * ================================================================ */
#define CM_PER_BASE              0x44E00000
#define CM_PER_EPWMSS0_CLKCTRL  0xD4
#define CM_PER_EPWMSS1_CLKCTRL  0xCC

/* ================================================================
 * Control Module (Pinmux + PWMSS_CTRL)
 * ================================================================ */
#define CTRL_MOD_BASE       0x44E10000

/*
 * Pinmux P9_22 (spi0_sclk, offset 0x950):
 *   Mode 3 → ehrpwm0A
 *   Giá trị: 0x03 | BIT(3) (pull-down enable, pull-down sel, no input)
 *            = 0x03  (output, pull-down on, mode 3)
 *   FIX: Code cũ ghi 0x03 thiếu pull-disable bit → dùng 0x03 với pull-down
 *        Theo TRM conf_module: [2:0]=mode, [3]=pull-up/down sel (0=down),
 *        [4]=pull ena (0=ena), [5]=input ena (0=output)
 *        → 0x03 = output, pull-down enabled, mode3 = ĐúNG cho output PWM
 */
#define P9_22_MUX_OFFSET    0x950   /* spi0_sclk  → ehrpwm0A (mode 3) */
#define P9_14_MUX_OFFSET    0x848   /* gpmc_a2    → ehrpwm1A (mode 6) */
#define P9_22_MUX_VAL       0x03    /* mode 3, pull-down, output */
#define P9_14_MUX_VAL       0x06    /* mode 6, pull-down, output */

/*
 * PWMSS_CTRL (offset 0x664):
 * bit 0 = PWMSS0_TBCLKEN, bit 1 = PWMSS1_TBCLKEN, bit 2 = PWMSS2_TBCLKEN
 * Set bit 0 và 1 để cấp TBCLK cho PWM0 và PWM1
 */
#define PWMSS_CTRL_OFFSET   0x664

/* ================================================================
 * PWMSS Base và ePWM offset
 * AM335x TRM Table 2-3:
 *   PWMSS0: 0x4830_0000, ePWM0 tại +0x200
 *   PWMSS1: 0x4830_2000, ePWM1 tại +0x200
 * ================================================================ */
#define PWMSS0_BASE_PHYS  0x48300000
#define PWMSS1_BASE_PHYS  0x48302000
#define PWMSS_MAP_SIZE    0x1000
#define EHRPWM_OFFSET     0x200

/* ================================================================
 * PWMSS Subsystem Registers (relative to PWMSS base)
 * ================================================================ */
#define PWMSS_CLKCONFIG   0x08   /* bit 8: EPWMCLK_EN */
#define PWMSS_CLKSTATUS   0x0C

/* ================================================================
 * ePWM Register Offsets (relative to ePWM base = PWMSS + 0x200)
 * AM335x TRM Table 15-57
 * QUAN TRỌNG: Tất cả là thanh ghi 16-bit → dùng ioread16/iowrite16
 * ================================================================ */
#define TBCTL   0x00   /* Time-Base Control */
#define TBSTS   0x02
#define TBPHS   0x06
#define TBCNT   0x08   /* Time-Base Counter */
#define TBPRD   0x0A   /* Time-Base Period ← offset lẻ, phải dùng 16-bit! */
#define CMPCTL  0x0E   /* Compare Control */
#define CMPA    0x12   /* Compare A       ← offset lẻ, phải dùng 16-bit! */
#define CMPB    0x14
#define AQCTLA  0x16   /* Action-Qualifier Control A ← offset lẻ! */
#define AQCTLB  0x18

/* ================================================================
 * TBCTL Register bits (TRM Table 15-22)
 *
 * [1:0]  CTRMODE: 00=up, 01=down, 10=up-down, 11=stop-freeze
 * [3:2]  PHSEN  : 0=disable phase loading
 * [5:4]  SYNCOSEL: 11=disable sync out
 * [6]    SWFSYNC: 0=no force
 * [7]    HSPCLKDIV: [9:7] = high speed prescaler
 * [12:10] CLKDIV : clock divider
 * [13]   PHSDIR : phase direction
 * [14]   FREE_SOFT: [15:14] emulation bits
 *
 * TBCLK = EPWMCLK / (HSPCLKDIV × CLKDIV)
 * EPWMCLK = 100MHz (BBB sysclk)
 *
 * FIX: Code cũ dùng 0x0010 = bit 4 set = SYNCOSEL[0]=1, PHSEN=0, CTRMODE=00
 *      Điều này set SYNCOSEL=01 (sync out khi CTR=0), không phải disable.
 *      Đúng: disable sync out = SYNCOSEL=11 = bits[5:4]=11 → 0x0030
 *
 * Config mong muốn:
 *   CTRMODE = 00 (up-count)
 *   PHSEN   = 0
 *   SYNCOSEL = 11 (disable sync out)
 *   HSPCLKDIV = 001 (÷2, bits[9:7]) → không set trong val cơ bản (default)
 *   CLKDIV  = 000 (÷1, bits[12:10])
 *   FREE_SOFT = 11 (run free, bits[15:14])
 *
 * Tần số PWM = EPWMCLK / (CLKDIV × HSPCLKDIV × (TBPRD+1))
 *
 * Cho quạt: f = 25kHz (phổ biến cho BLDC/PWM fan)
 *   TBPRD = 100MHz / 25kHz - 1 = 3999
 *   CLKDIV=0(÷1), HSPCLKDIV=0(÷1, default after reset=001 so ÷2)
 *   Note: HSPCLKDIV reset value = 001 (÷2), CLKDIV reset = 000 (÷1)
 *   Vậy TBCLK = 100MHz / 2 = 50MHz
 *   TBPRD = 50MHz / 25kHz - 1 = 1999
 *
 * Cho còi buzzer: tần số âm thanh ~2kHz (2000Hz)
 *   TBPRD = 50MHz / 2000Hz - 1 = 24999
 *
 * TBCTL setup: CTRMODE=00, SYNCOSEL=11, HSPCLKDIV=001(default), CLKDIV=000
 *   = bits[5:4]=11, rest=0 → TBCTL_VAL = 0x0030
 *   (FREE_SOFT=00 ổn ở kernel, không cần set)
 */
#define TBCTL_VAL  0x0030   /* up-count, sync out disabled */

/* Chu kỳ PWM
 * TBCLK = 100MHz / (HSPCLKDIV=1 × CLKDIV=1) = 100MHz
 * Vì HSPCLKDIV default sau reset = 001 = ÷2 → TBCLK = 50MHz
 *
 * Quạt 25kHz: TBPRD = 50,000,000 / 25,000 - 1 = 1999
 * Còi 2kHz:   TBPRD = 50,000,000 / 2,000  - 1 = 24999
 */
#define TBPRD_FAN  1999    /* 25 kHz cho quạt */
#define TBPRD_BUZ  24999   /* 2 kHz cho còi   */

/* ================================================================
 * AQCTLA Register (TRM Table 15-42)
 *
 * [1:0]  ZRO : action when CTR=0
 * [3:2]  PRD : action when CTR=TBPRD
 * [5:4]  CAU : action when CTR=CMPA và đang đếm lên (Count-Up)
 * [7:6]  CAD : action when CTR=CMPA và đang đếm xuống
 * Actions: 00=nothing, 01=clear(low), 10=set(high), 11=toggle
 *
 * PWM thông thường (active-high): Set HIGH khi CTR=0, Clear LOW khi CTR=CMPA
 *   ZRO=10(set), CAU=01(clear)
 *   = bits[1:0]=10, bits[5:4]=01 → 0x0012
 *
 * FIX: Code cũ dùng 0x0012 → Thực ra ĐÚNG cho active-high PWM.
 *      Nhưng chỉ hoạt động nếu TBPRD và CMPA ghi được (xem lỗi chính bên dưới)
 */
#define AQCTLA_VAL  0x0012  /* ZRO=set(hi), CAU=clear(lo) → active-high PWM */

/* ================================================================
 * LỖI CHÍNH: ghi TBPRD, CMPA, AQCTLA với writew() KHÔNG HOẠT ĐỘNG
 * trên AM335x vì các offset này không nằm trên biên 32-bit:
 *   TBPRD  = 0x0A (lẻ)
 *   CMPA   = 0x12 (lẻ)
 *   AQCTLA = 0x16 (lẻ)
 *
 * Theo TI E2E forum (e2e.ti.com/f/670544), kỹ sư TI xác nhận:
 * writew() trực tiếp vào odd-offset 16-bit register KHÔNG GHI ĐƯỢC.
 * Giải pháp chuẩn: dùng iowrite16() HOẶC thực hiện RMW 32-bit.
 *
 * Kernel Linux dùng macro ehrpwm_write() = iowrite16() trong ehrpwm.c.
 * Ta làm tương tự: dùng iowrite16() cho các thanh ghi ePWM.
 * ================================================================ */

/* Helper macros an toàn cho ePWM 16-bit registers */
#define epwm_write(base, offset, val)  iowrite16((u16)(val), (base) + (offset))
#define epwm_read(base, offset)        ioread16((base) + (offset))

/* ================================================================
 * Biến toàn cục
 * ================================================================ */
static dev_t dev_num;
static struct cdev pwm_cdev;
static struct class *pwm_class;

static void __iomem *pwmss0_mem;   /* PWMSS0 base */
static void __iomem *pwmss1_mem;   /* PWMSS1 base */
static void __iomem *epwm0_reg;    /* ePWM0 = pwmss0 + 0x200 */
static void __iomem *epwm1_reg;    /* ePWM1 = pwmss1 + 0x200 */

/* ================================================================
 * Hàm setup một kênh ePWM
 * ================================================================ */
static void epwm_setup(void __iomem *base, u16 tbprd)
{
    /* 1. Dừng bộ đếm trước khi cấu hình */
    epwm_write(base, TBCTL, 0x0033); /* CTRMODE=11 (stop), SYNCOSEL=11 */

    /* 2. Ghi period (TBPRD là shadow register theo mặc định) */
    epwm_write(base, TBPRD, tbprd);

    /* 3. CMPA = 0 (tắt, 0% duty) */
    epwm_write(base, CMPA, 0);

    /* 4. Reset bộ đếm */
    epwm_write(base, TBCNT, 0);

    /* 5. Cấu hình Action Qualifier: active-high PWM */
    epwm_write(base, AQCTLA, AQCTLA_VAL);

    /* 6. Bắt đầu đếm lên (up-count, sync disabled) */
    epwm_write(base, TBCTL, TBCTL_VAL);
}

/* ================================================================
 * Hàm đặt duty cycle (0–100%)
 * CMPA = TBPRD * duty / 100
 * (duty=0 → CMPA=0 → luôn LOW → tắt)
 * (duty=100 → CMPA=TBPRD → luôn HIGH → full speed)
 * ================================================================ */
static void epwm_set_duty(void __iomem *base, u16 tbprd, int duty)
{
    u16 cmpa;
    if (duty <= 0)       cmpa = 0;
    else if (duty >= 100) cmpa = tbprd;
    else                  cmpa = (u16)((tbprd * duty) / 100);
    epwm_write(base, CMPA, cmpa);
}

/* ================================================================
 * File operation: write
 * Lệnh: "FAN <0-100>\n", "BUZ <0-100>\n", "SYNC <0-100>\n"
 * ================================================================ */
static ssize_t pwm_write(struct file *file, const char __user *user_buf,
                          size_t size, loff_t *offset)
{
    char buf[32] = {0};
    int val = 0;

    if (size > sizeof(buf) - 1)
        size = sizeof(buf) - 1;
    if (copy_from_user(buf, user_buf, size))
        return -EFAULT;
    buf[size] = '\0';

    if (strncmp(buf, "FAN", 3) == 0) {
        sscanf(buf + 3, "%d", &val);
        val = clamp(val, 0, 100);
        epwm_set_duty(epwm1_reg, TBPRD_FAN, val);
        //printk(KERN_INFO "PWM: Quat (P9_14) -> %d%%\n", val);
    }
    else if (strncmp(buf, "BUZ", 3) == 0) {
        sscanf(buf + 3, "%d", &val);
        val = clamp(val, 0, 100);
        /*
         * Còi buzzer: duty cycle 50% là âm lớn nhất.
         * Điều chỉnh val 0-100 → CMPA 0-50% thực tế:
         * CMPA = TBPRD * val/200  (÷2 để max = 50%)
         * Nếu muốn tắt hẳn: duty=0 → CMPA=0
         */
        if (val == 0) {
            epwm_write(epwm0_reg, CMPA, 0);
        } else {
            u16 cmpa = (u16)((TBPRD_BUZ * val) / 200);
            if (cmpa == 0) cmpa = 1; /* đảm bảo có xung khi val>0 */
            epwm_write(epwm0_reg, CMPA, cmpa);
        }
        //printk(KERN_INFO "PWM: Coi (P9_22) -> %d%%\n", val);
    }
    else if (strncmp(buf, "SYNC", 4) == 0) {
        sscanf(buf + 4, "%d", &val);
        val = clamp(val, 0, 100);
        epwm_set_duty(epwm1_reg, TBPRD_FAN, val);
        /* Còi: scale như trên */
        if (val == 0)
            epwm_write(epwm0_reg, CMPA, 0);
        else {
            u16 cmpa = (u16)((TBPRD_BUZ * val) / 200);
            if (cmpa == 0) cmpa = 1;
            epwm_write(epwm0_reg, CMPA, cmpa);
        }
        //printk(KERN_INFO "PWM: SYNC Quat+Coi -> %d%%\n", val);
    }
    else {
        //printk(KERN_WARNING "PWM: Lenh khong hop le. Dung: FAN/BUZ/SYNC <0-100>\n");
    }

    return size;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = pwm_write,
};

/* ================================================================
 * Module Init
 * ================================================================ */
static int __init bbb_pwm_init(void)
{
    u32 reg_val;
    void __iomem *cm_per_mem;
    void __iomem *ctrl_mod_mem;

    printk(KERN_INFO "PWM Driver: Dang khoi tao...\n");

    /* --- 1. Cấp clock cho PWMSS0 và PWMSS1 --- */
    cm_per_mem = ioremap(CM_PER_BASE, SZ_4K);
    if (!cm_per_mem) return -ENOMEM;

    writel(0x02, cm_per_mem + CM_PER_EPWMSS0_CLKCTRL);
    writel(0x02, cm_per_mem + CM_PER_EPWMSS1_CLKCTRL);
    /* Chờ clock ổn định: bit[17:16] = 0 là "FULLY_FUNCTIONAL" */
    while (readl(cm_per_mem + CM_PER_EPWMSS0_CLKCTRL) & (0x3 << 16)) cpu_relax();
    while (readl(cm_per_mem + CM_PER_EPWMSS1_CLKCTRL) & (0x3 << 16)) cpu_relax();
    iounmap(cm_per_mem);

    /* --- 2. Pinmux + Mở TBCLK --- */
    ctrl_mod_mem = ioremap(CTRL_MOD_BASE, SZ_8K);
    if (!ctrl_mod_mem) return -ENOMEM;

    /*
     * FIX: Giá trị pinmux cũ thiếu pull-disable.
     * Cho chân output PWM, theo TRM conf register:
     *   [2:0] mode, [3] pull_type (0=down), [4] pull_ena (0=enabled), [5] inputen (0=disable)
     * Output PWM → inputen=0, pull_ena tùy (có thể disable = bit4=1):
     *   0x06 = 0b00000110 = mode6, pull-down, output (P9_14)
     *   0x03 = 0b00000011 = mode3, pull-down, output (P9_22)
     */
    writel(P9_14_MUX_VAL, ctrl_mod_mem + P9_14_MUX_OFFSET);
    writel(P9_22_MUX_VAL, ctrl_mod_mem + P9_22_MUX_OFFSET);

    /* Bật TBCLK cho PWMSS0 (bit0) và PWMSS1 (bit1) */
    reg_val = readl(ctrl_mod_mem + PWMSS_CTRL_OFFSET);
    writel(reg_val | 0x03, ctrl_mod_mem + PWMSS_CTRL_OFFSET);
    iounmap(ctrl_mod_mem);

    /* --- 3. Map vùng nhớ PWMSS --- */
    pwmss0_mem = ioremap(PWMSS0_BASE_PHYS, PWMSS_MAP_SIZE);
    pwmss1_mem = ioremap(PWMSS1_BASE_PHYS, PWMSS_MAP_SIZE);
    if (!pwmss0_mem || !pwmss1_mem) {
        if (pwmss0_mem) iounmap(pwmss0_mem);
        if (pwmss1_mem) iounmap(pwmss1_mem);
        return -ENOMEM;
    }

    epwm0_reg = pwmss0_mem + EHRPWM_OFFSET;
    epwm1_reg = pwmss1_mem + EHRPWM_OFFSET;

    /* --- 4. Bật EPWMCLK nội bộ trong PWMSS ---
     * PWMSS_CLKCONFIG bit 8 = ePWMCLK_EN
     * FIX: Code cũ dùng readl/writel (32-bit) cho CLKCONFIG → ĐúNG
     *      vì CLKCONFIG là register 32-bit (nằm trên 32-bit boundary).
     */
    reg_val = readl(pwmss0_mem + PWMSS_CLKCONFIG);
    writel(reg_val | (1 << 8), pwmss0_mem + PWMSS_CLKCONFIG);
    reg_val = readl(pwmss1_mem + PWMSS_CLKCONFIG);
    writel(reg_val | (1 << 8), pwmss1_mem + PWMSS_CLKCONFIG);
    udelay(100); /* Chờ clock ổn định */

    /* --- 5. Cấu hình ePWM --- */
    epwm_setup(epwm0_reg, TBPRD_BUZ);   /* Còi: 2kHz */
    epwm_setup(epwm1_reg, TBPRD_FAN);   /* Quạt: 25kHz */

    /* --- 6. Đăng ký character device --- */
    if (alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME) < 0) {
        iounmap(pwmss0_mem);
        iounmap(pwmss1_mem);
        return -ENODEV;
    }
    cdev_init(&pwm_cdev, &fops);
    if (cdev_add(&pwm_cdev, dev_num, 1) < 0) {
        unregister_chrdev_region(dev_num, 1);
        iounmap(pwmss0_mem);
        iounmap(pwmss1_mem);
        return -ENODEV;
    }
    pwm_class = class_create(CLASS_NAME);
    if (IS_ERR(pwm_class)) {
        cdev_del(&pwm_cdev);
        unregister_chrdev_region(dev_num, 1);
        iounmap(pwmss0_mem);
        iounmap(pwmss1_mem);
        return PTR_ERR(pwm_class);
    }
    device_create(pwm_class, NULL, dev_num, NULL, DRIVER_NAME);

    printk(KERN_INFO "PWM Driver: San sang! /dev/%s\n", DRIVER_NAME);
    printk(KERN_INFO "  Quat (P9_14, 25kHz): echo 'FAN 80' > /dev/%s\n", DRIVER_NAME);
    printk(KERN_INFO "  Coi  (P9_22, 2kHz) : echo 'BUZ 50' > /dev/%s\n", DRIVER_NAME);
    return 0;
}

/* ================================================================
 * Module Exit
 * ================================================================ */
static void __exit bbb_pwm_exit(void)
{
    u32 reg_val;
    void __iomem *ctrl_mod_mem;

    /* Tắt output (duty = 0) và dừng bộ đếm */
    if (epwm0_reg) {
        epwm_write(epwm0_reg, CMPA, 0);
        epwm_write(epwm0_reg, TBCTL, 0x0033); /* stop */
        reg_val = readl(pwmss0_mem + PWMSS_CLKCONFIG);
        writel(reg_val & ~(1 << 8), pwmss0_mem + PWMSS_CLKCONFIG);
    }
    if (epwm1_reg) {
        epwm_write(epwm1_reg, CMPA, 0);
        epwm_write(epwm1_reg, TBCTL, 0x0033); /* stop */
        reg_val = readl(pwmss1_mem + PWMSS_CLKCONFIG);
        writel(reg_val & ~(1 << 8), pwmss1_mem + PWMSS_CLKCONFIG);
    }

    /* Khóa lại TBCLK */
    ctrl_mod_mem = ioremap(CTRL_MOD_BASE, SZ_8K);
    if (ctrl_mod_mem) {
        reg_val = readl(ctrl_mod_mem + PWMSS_CTRL_OFFSET);
        writel(reg_val & ~0x03, ctrl_mod_mem + PWMSS_CTRL_OFFSET);
        iounmap(ctrl_mod_mem);
    }

    if (pwmss0_mem) iounmap(pwmss0_mem);
    if (pwmss1_mem) iounmap(pwmss1_mem);

    device_destroy(pwm_class, dev_num);
    class_destroy(pwm_class);
    cdev_del(&pwm_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "PWM Driver: Da go bo an toan.\n");
}

module_init(bbb_pwm_init);
module_exit(bbb_pwm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fixed");
MODULE_DESCRIPTION("AM335x eHRPWM Driver - Fan (P9_14) & Buzzer (P9_22)");
