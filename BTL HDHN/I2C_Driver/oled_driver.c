/*
 * SSD1306 OLED 128x64 I2C Driver cho BeagleBone Black
 * Bus: I2C1 — P9_17 (SCL) / P9_18 (SDA)
 * Giao thức: I2C bare-metal qua ioremap AM335x I2C controller
 *
 * Cách dùng từ userspace:
 *   echo "Hello BBB" > /dev/ssd1306       # Hiển thị text lên dòng 1
 *   echo "L2:Temp: 28C" > /dev/ssd1306    # Prefix "L2:" → dòng 2
 *   echo "L3:Humi: 65%" > /dev/ssd1306    # Prefix "L3:" → dòng 3
 *   echo "CLS" > /dev/ssd1306             # Xóa màn hình
 *
 * Màn hình 128x64 pixel, font 8x8:
 *   → 16 ký tự/dòng, 8 dòng tổng (page 0–7)
 *   Driver hiển thị text ở các page 0–7
 *
 * Tham chiếu:
 *   - SSD1306 Datasheet (Solomon Systech, ver 1.1)
 *   - AM335x TRM chương 21 (I2C)
 *   - casimirsowinski.wordpress.com/2016/08/31/i2c-display-driver/
 *   - conf_spi0_d1=0x958, conf_spi0_cs0=0x95C (TRM Table 9-10)
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/mutex.h>


#define DRIVER_NAME "oled_i2c"
#define CLASS_NAME  "oled_class"

/* ================================================================
 * Clock Manager (CM_PER)
 * ================================================================ */
#define CM_PER_BASE             0x44E00000
#define CM_PER_I2C1_CLKCTRL    0x48    /* TRM Table 8-153 */

/* ================================================================
 * Control Module — Pinmux
 * P9_17 = spi0_cs0  → offset 0x95C → Mode 2 = I2C1_SCL
 * P9_18 = spi0_d1   → offset 0x958 → Mode 2 = I2C1_SDA
 *
 * Giá trị pinmux cho I2C (open-drain, pull-up, input enable):
 *   [2:0] = 010 (mode 2)
 *   [3]   = 1   (pull-up select)
 *   [4]   = 0   (pull enable ON)
 *   [5]   = 1   (input enable — bắt buộc với I2C open-drain)
 *   → 0b10_1010 = 0x2A
 *
 * Lưu ý: I2C PHẢI có pull-up ngoài 4.7kΩ trên cả SCL và SDA
 * ================================================================ */
#define CTRL_MOD_BASE           0x44E10000
#define P9_18_SDA_MUX_OFFSET   0x958   /* conf_spi0_d1  → I2C1_SDA */
#define P9_17_SCL_MUX_OFFSET   0x95C   /* conf_spi0_cs0 → I2C1_SCL */
#define I2C_PINMUX_VAL         0x2A    /* mode2, pull-up, input enable */

/* ================================================================
 * AM335x I2C1 Controller (TRM chương 21)
 * Base: 0x4802A000
 * ================================================================ */
#define I2C1_BASE_PHYS         0x4802A000
#define I2C_MAP_SIZE           0x1000

/* I2C Register offsets (TRM Table 21-3) */
#define I2C_REVNB_LO           0x00
#define I2C_SYSC               0x10
#define I2C_IRQSTATUS_RAW      0x24
#define I2C_IRQSTATUS          0x28
#define I2C_IRQENABLE_SET      0x2C
#define I2C_IRQENABLE_CLR      0x30
#define I2C_WE                 0x34
#define I2C_DMARXENABLE_SET    0x38
#define I2C_DMATXENABLE_SET    0x3C
#define I2C_SYSS               0x90
#define I2C_BUF                0x94
#define I2C_CNT                0x98
#define I2C_DATA               0x9C
#define I2C_CON                0xA4
#define I2C_OA                 0xA8
#define I2C_SA                 0xAC
#define I2C_PSC                0xB0
#define I2C_SCLL               0xB4
#define I2C_SCLH               0xB8
#define I2C_SYSTEST            0xBC
#define I2C_BUFSTAT            0xC0

/* I2C_CON bits */
#define I2C_CON_EN             BIT(15)  /* I2C enable */
#define I2C_CON_MST            BIT(10)  /* Master mode */
#define I2C_CON_TRX            BIT(9)   /* Transmit mode */
#define I2C_CON_STP            BIT(1)   /* Stop condition */
#define I2C_CON_STT            BIT(0)   /* Start condition */

/* I2C_IRQSTATUS bits */
#define I2C_IRQ_XRDY           BIT(4)   /* Transmit data ready */
#define I2C_IRQ_ARDY           BIT(2)   /* Access ready (transfer done) */
#define I2C_IRQ_NACK           BIT(1)   /* No-ACK */
#define I2C_IRQ_AL             BIT(0)   /* Arbitration lost */

/* I2C_SYSS bits */
#define I2C_SYSS_RDONE         BIT(0)   /* Reset done */

/*
 * I2C clock setup cho 100kHz (Standard mode):
 * I2C functional clock = 48MHz (từ DPLL_PER)
 * PSC  = 3   → internal clock = 48MHz / (3+1) = 12MHz
 * SCLL = 53  → low  period = (53+7)  / 12MHz = 5.0µs
 * SCLH = 55  → high period = (55+5)  / 12MHz = 5.0µs  → tổng = 10µs = 100kHz
 * (Giá trị từ TRM Table 21-5 và TI E2E confirmed)
 */
#define I2C_PSC_VAL    3
#define I2C_SCLL_VAL   53
#define I2C_SCLH_VAL   55

/* SSD1306 I2C address (mặc định, SA0 nối GND) */
#define SSD1306_I2C_ADDR       0x3C

/* ================================================================
 * SSD1306 Control bytes (Datasheet section 8.1.5)
 * Control byte gửi trước mỗi command hoặc data:
 *   0x00 = Co=0, D/C=0 → byte tiếp theo là Command
 *   0x40 = Co=0, D/C=1 → byte tiếp theo là GDDRAM Data
 * ================================================================ */
#define SSD1306_CTRL_CMD       0x00
#define SSD1306_CTRL_DATA      0x40

/* SSD1306 Commands (Datasheet Table 9-1) */
#define SSD1306_DISPLAY_OFF         0xAE
#define SSD1306_DISPLAY_ON          0xAF
#define SSD1306_SET_CONTRAST        0x81
#define SSD1306_ENTIRE_DISPLAY_ON   0xA4   /* 0xA4=output GDDRAM, 0xA5=force all ON */
#define SSD1306_SET_NORMAL          0xA6   /* 0xA6=normal, 0xA7=inverse */
#define SSD1306_SET_MUX_RATIO       0xA8
#define SSD1306_SET_DISPLAY_OFFSET  0xD3
#define SSD1306_SET_START_LINE      0x40   /* bits[5:0] = start line */
#define SSD1306_SEG_REMAP_ON        0xA1   /* Column 127 mapped to SEG0 (flip H) */
#define SSD1306_COM_SCAN_DEC        0xC8   /* Scan từ COM[N-1] → COM0 (flip V)   */
#define SSD1306_SET_COM_PINS        0xDA
#define SSD1306_SET_CLK_DIV         0xD5
#define SSD1306_SET_PRECHARGE       0xD9
#define SSD1306_SET_VCOM_DETECT     0xDB
#define SSD1306_CHARGE_PUMP         0x8D
#define SSD1306_MEM_ADDR_MODE       0x20
#define SSD1306_SET_PAGE_ADDR       0x22
#define SSD1306_SET_COL_ADDR        0x21

/* Memory addressing mode */
#define SSD1306_ADDR_MODE_HORIZ    0x00   /* Horizontal: tự tăng col rồi page */
#define SSD1306_ADDR_MODE_PAGE     0x02   /* Page addressing */

/* ================================================================
 * Font 8x8 ASCII (32–127)
 * Mỗi ký tự = 8 byte, mỗi byte = 1 cột pixel (8 hàng)
 * Nguồn: font 8x8 public domain (deribit/font8x8)
 * ================================================================ */
static const u8 font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' (32) */
    {0x00,0x00,0x5F,0x00,0x00,0x00,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00,0x00,0x00,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14,0x00,0x00,0x00}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12,0x00,0x00,0x00}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62,0x00,0x00,0x00}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50,0x00,0x00,0x00}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00,0x00,0x00,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00,0x00,0x00,0x00}, /* ')' */
    {0x08,0x2A,0x1C,0x2A,0x08,0x00,0x00,0x00}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08,0x00,0x00,0x00}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00,0x00,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00,0x00,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02,0x00,0x00,0x00}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E,0x00,0x00,0x00}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00,0x00,0x00,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46,0x00,0x00,0x00}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31,0x00,0x00,0x00}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10,0x00,0x00,0x00}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39,0x00,0x00,0x00}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30,0x00,0x00,0x00}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03,0x00,0x00,0x00}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E,0x00,0x00,0x00}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00,0x00,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00,0x00,0x00,0x00}, /* ';' */
    {0x00,0x08,0x14,0x22,0x41,0x00,0x00,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14,0x00,0x00,0x00}, /* '=' */
    {0x41,0x22,0x14,0x08,0x00,0x00,0x00,0x00}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06,0x00,0x00,0x00}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E,0x00,0x00,0x00}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E,0x00,0x00,0x00}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22,0x00,0x00,0x00}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C,0x00,0x00,0x00}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41,0x00,0x00,0x00}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01,0x00,0x00,0x00}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A,0x00,0x00,0x00}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F,0x00,0x00,0x00}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00,0x00,0x00,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01,0x00,0x00,0x00}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41,0x00,0x00,0x00}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40,0x00,0x00,0x00}, /* 'L' */
    {0x7F,0x02,0x04,0x02,0x7F,0x00,0x00,0x00}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F,0x00,0x00,0x00}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E,0x00,0x00,0x00}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06,0x00,0x00,0x00}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E,0x00,0x00,0x00}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46,0x00,0x00,0x00}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31,0x00,0x00,0x00}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01,0x00,0x00,0x00}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F,0x00,0x00,0x00}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F,0x00,0x00,0x00}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F,0x00,0x00,0x00}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63,0x00,0x00,0x00}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07,0x00,0x00,0x00}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43,0x00,0x00,0x00}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00,0x00,0x00,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20,0x00,0x00,0x00}, /* '\\' */
    {0x00,0x41,0x41,0x7F,0x00,0x00,0x00,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04,0x00,0x00,0x00}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40,0x00,0x00,0x00}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00,0x00,0x00,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78,0x00,0x00,0x00}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38,0x00,0x00,0x00}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20,0x00,0x00,0x00}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F,0x00,0x00,0x00}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18,0x00,0x00,0x00}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02,0x00,0x00,0x00}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E,0x00,0x00,0x00}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78,0x00,0x00,0x00}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00,0x00,0x00,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00,0x00,0x00,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00,0x00,0x00,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00,0x00,0x00,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78,0x00,0x00,0x00}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78,0x00,0x00,0x00}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38,0x00,0x00,0x00}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08,0x00,0x00,0x00}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C,0x00,0x00,0x00}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08,0x00,0x00,0x00}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20,0x00,0x00,0x00}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20,0x00,0x00,0x00}, /* 't' */
    {0x3C,0x40,0x40,0x40,0x3C,0x00,0x00,0x00}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C,0x00,0x00,0x00}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C,0x00,0x00,0x00}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44,0x00,0x00,0x00}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C,0x00,0x00,0x00}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44,0x00,0x00,0x00}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00,0x00,0x00,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00,0x00,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00,0x00,0x00,0x00}, /* '}' */
    {0x08,0x04,0x08,0x10,0x08,0x00,0x00,0x00}, /* '~' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* DEL */
};

/* ================================================================
 * Framebuffer nội bộ: 128x64 pixels = 128*8 bytes = 1024 bytes
 * Tổ chức: 8 pages × 128 columns, mỗi byte = 8 pixel dọc
 * ================================================================ */
#define FB_WIDTH   128
#define FB_PAGES   8
#define FB_SIZE    (FB_WIDTH * FB_PAGES)   /* 1024 bytes */

static u8 g_framebuf[FB_SIZE];

/* ================================================================
 * Biến toàn cục
 * ================================================================ */
static dev_t dev_num;
static struct cdev ssd1306_cdev;
static struct class *ssd1306_class;
static void __iomem *i2c1_base;

static DEFINE_MUTEX(oled_mutex);
/* ================================================================
 * I2C Controller helpers
 * Polling-based (không dùng interrupt) — phù hợp cho kernel driver đơn giản
 * ================================================================ */
#define I2C_TIMEOUT_US  50000   /* 50ms timeout mỗi operation */

static int i2c_wait_for_flag(u32 flag, int timeout_us)
{
    int t = timeout_us;
    while (!(readw(i2c1_base + I2C_IRQSTATUS_RAW) & flag)) {
        udelay(1);
        if (--t <= 0) return -ETIMEDOUT;
    }
    return 0;
}

static int i2c_check_error(void)
{
    u16 status = readw(i2c1_base + I2C_IRQSTATUS_RAW);
    if (status & I2C_IRQ_NACK) {
        writew(I2C_IRQ_NACK, i2c1_base + I2C_IRQSTATUS);
        return -EIO;
    }
    if (status & I2C_IRQ_AL) {
        writew(I2C_IRQ_AL, i2c1_base + I2C_IRQSTATUS);
        return -EAGAIN;
    }
    return 0;
}

/*
 * Gửi buffer qua I2C đến SSD1306
 * @addr : I2C slave address
 * @buf  : dữ liệu cần gửi
 * @len  : số byte
 */
static int i2c_write(u8 addr, const u8 *buf, int len)
{
    int i, ret;

    /* Đặt slave address */
    writew(addr, i2c1_base + I2C_SA);

    /* Đặt số byte cần gửi */
    writew(len, i2c1_base + I2C_CNT);

    /* Xóa pending interrupt */
    writew(0xFFFF, i2c1_base + I2C_IRQSTATUS);

    /* Phát START + Master TX */
    writew(I2C_CON_EN | I2C_CON_MST | I2C_CON_TRX |
           I2C_CON_STT | I2C_CON_STP, i2c1_base + I2C_CON);

    for (i = 0; i < len; i++) {
        /* Chờ XRDY (Transmit data register ready) */
        ret = i2c_wait_for_flag(I2C_IRQ_XRDY, I2C_TIMEOUT_US);
        if (ret < 0) {
            printk(KERN_ERR "SSD1306: I2C XRDY timeout byte %d\n", i);
            goto error;
        }
        ret = i2c_check_error();
        if (ret < 0) {
            printk(KERN_ERR "SSD1306: I2C error byte %d (NACK/AL)\n", i);
            goto error;
        }
        /* Ghi byte vào data register */
        writew(buf[i], i2c1_base + I2C_DATA);
        /* Xóa flag XRDY */
        writew(I2C_IRQ_XRDY, i2c1_base + I2C_IRQSTATUS);
    }

    /* Chờ ARDY (transfer hoàn tất, STOP đã gửi) */
    ret = i2c_wait_for_flag(I2C_IRQ_ARDY, I2C_TIMEOUT_US);
    if (ret < 0)
        printk(KERN_ERR "SSD1306: I2C ARDY timeout\n");

    writew(I2C_IRQ_ARDY, i2c1_base + I2C_IRQSTATUS);
    return ret;

error:
    /* Reset bus khi lỗi */
    writew(0, i2c1_base + I2C_CON);
    udelay(100);
    writew(I2C_CON_EN, i2c1_base + I2C_CON);
    return ret;
}

/* ================================================================
 * SSD1306 helpers
 * ================================================================ */

/* Gửi 1 command */
static int ssd1306_cmd(u8 cmd)
{
    u8 buf[2] = { SSD1306_CTRL_CMD, cmd };
    return i2c_write(SSD1306_I2C_ADDR, buf, 2);
}

/* Gửi 2 byte command (command + argument) */
static int ssd1306_cmd2(u8 cmd, u8 arg)
{
    u8 buf[3] = { SSD1306_CTRL_CMD, cmd, arg };
    return i2c_write(SSD1306_I2C_ADDR, buf, 3);
}

/*
 * Flush framebuffer lên màn hình
 * Dùng Horizontal Addressing Mode: gửi toàn bộ 1024 byte một lần
 * (SSD1306 tự tăng column rồi page)
 */
static int ssd1306_flush(void)
{
    /* Khai báo buffer tĩnh để tránh stack overflow trong kernel */
    static u8 txbuf[FB_SIZE + 1];
    txbuf[0] = SSD1306_CTRL_DATA;
    memcpy(txbuf + 1, g_framebuf, FB_SIZE);

    /* Set cửa sổ ghi: col 0-127, page 0-7 */
    ssd1306_cmd(SSD1306_SET_COL_ADDR);
    ssd1306_cmd(0);    /* start col */
    ssd1306_cmd(127);  /* end col   */
    ssd1306_cmd(SSD1306_SET_PAGE_ADDR);
    ssd1306_cmd(0);    /* start page */
    ssd1306_cmd(7);    /* end page   */

    return i2c_write(SSD1306_I2C_ADDR, txbuf, FB_SIZE + 1);
}

/* Xóa framebuffer (tất cả pixel = off) */
static void ssd1306_clear_fb(void)
{
    memset(g_framebuf, 0, FB_SIZE);
}

/*
 * Vẽ 1 ký tự lên framebuffer tại (page, col)
 * Font 8x8: mỗi ký tự chiếm 1 page (8 pixel cao) × 8 col
 */
static void fb_draw_char(u8 page, u8 col, char c)
{
    const u8 *glyph;
    int i;

    if (c < 32 || c > 127) c = '?';
    glyph = font8x8[c - 32];

    if (page >= FB_PAGES) return;
    if (col + 8 > FB_WIDTH) return;

    for (i = 0; i < 8; i++)
        g_framebuf[page * FB_WIDTH + col + i] = glyph[i];
}

/* Vẽ chuỗi text lên framebuffer tại page chỉ định */
static void fb_draw_string(u8 page, const char *str, int len)
{
    u8 col = 0;
    int i;
    /* Xóa page này trước */
    memset(g_framebuf + page * FB_WIDTH, 0, FB_WIDTH);
    for (i = 0; i < len && col + 8 <= FB_WIDTH; i++, col += 8)
        fb_draw_char(page, col, str[i]);
}

/* ================================================================
 * SSD1306 Initialization Sequence
 * Theo datasheet Solomon Systech SSD1306 Rev 1.1, section 8.8
 * và Application Note "Software Examples"
 * ================================================================ */
static int ssd1306_init_display(void)
{
    int ret = 0;

    /* 1. Tắt display trước khi cấu hình */
    ret |= ssd1306_cmd(SSD1306_DISPLAY_OFF);

    /* 2. Clock divider / oscillator frequency: 0x80 = div=1, freq=8 */
    ret |= ssd1306_cmd2(SSD1306_SET_CLK_DIV, 0x80);

    /* 3. Multiplex ratio = 63 (64 rows, 0-indexed) */
    ret |= ssd1306_cmd2(SSD1306_SET_MUX_RATIO, 0x3F);

    /* 4. Display offset = 0 */
    ret |= ssd1306_cmd2(SSD1306_SET_DISPLAY_OFFSET, 0x00);

    /* 5. Start line = 0 */
    ret |= ssd1306_cmd(SSD1306_SET_START_LINE | 0x00);

    /* 6. Charge pump: ENABLE (bắt buộc nếu dùng nguồn VCC nội) */
    ret |= ssd1306_cmd2(SSD1306_CHARGE_PUMP, 0x14);  /* 0x14=enable, 0x10=disable */

    /* 7. Memory addressing mode: Horizontal */
    ret |= ssd1306_cmd2(SSD1306_MEM_ADDR_MODE, SSD1306_ADDR_MODE_HORIZ);

    /* 8. Segment remap: col 127 → SEG0 (xem chiều ngang đúng) */
    ret |= ssd1306_cmd(SSD1306_SEG_REMAP_ON);

    /* 9. COM scan direction: scan từ COM[N-1] → COM0 */
    ret |= ssd1306_cmd(SSD1306_COM_SCAN_DEC);

    /* 10. COM pins hardware config: 0x12 = Alternative, Disable remap */
    ret |= ssd1306_cmd2(SSD1306_SET_COM_PINS, 0x12);

    /* 11. Contrast: 0xCF (cao, phù hợp module 3.3V) */
    ret |= ssd1306_cmd2(SSD1306_SET_CONTRAST, 0xCF);

    /* 12. Pre-charge period: 0xF1 */
    ret |= ssd1306_cmd2(SSD1306_SET_PRECHARGE, 0xF1);

    /* 13. VCOMH deselect level: 0x40 = 0.77 × VCC */
    ret |= ssd1306_cmd2(SSD1306_SET_VCOM_DETECT, 0x40);

    /* 14. Entire display ON: output từ GDDRAM (bình thường) */
    ret |= ssd1306_cmd(SSD1306_ENTIRE_DISPLAY_ON);

    /* 15. Normal display (không inverse) */
    ret |= ssd1306_cmd(SSD1306_SET_NORMAL);

    /* 16. Xóa màn hình (flush framebuffer rỗng) */
    ssd1306_clear_fb();
    ret |= ssd1306_flush();

    /* 17. Bật display */
    ret |= ssd1306_cmd(SSD1306_DISPLAY_ON);

    return ret;
}

/* ================================================================
 * I2C1 Controller Initialization
 * ================================================================ */
static int i2c1_hw_init(void)
{
    int timeout = 10000;

    /* Soft reset */
    writew(0x02, i2c1_base + I2C_SYSC);  /* bit1 = SRST */
    udelay(100);

    /* Chờ reset xong */
    writew(I2C_CON_EN, i2c1_base + I2C_CON);
    while (!(readw(i2c1_base + I2C_SYSS) & I2C_SYSS_RDONE)) {
        udelay(1);
        if (--timeout <= 0) {
            printk(KERN_ERR "SSD1306: I2C1 reset timeout!\n");
            return -ETIMEDOUT;
        }
    }

    /* Tắt I2C để cấu hình */
    writew(0, i2c1_base + I2C_CON);

    /* Tắt auto-idle để clock luôn hoạt động */
    writew(0x00, i2c1_base + I2C_SYSC);

    /* Cấu hình clock 100kHz:
     * I2C functional clock = 48MHz
     * PSC = 3 → internal = 12MHz
     * SCLL = 53, SCLH = 55 → 100kHz
     */
    writew(I2C_PSC_VAL,  i2c1_base + I2C_PSC);
    writew(I2C_SCLL_VAL, i2c1_base + I2C_SCLL);
    writew(I2C_SCLH_VAL, i2c1_base + I2C_SCLH);

    /* Own address (master không cần, set = 0) */
    writew(0, i2c1_base + I2C_OA);

    /* Bật I2C */
    writew(I2C_CON_EN, i2c1_base + I2C_CON);
    udelay(200);

    return 0;
}

/* ================================================================
 * File operation: write
 *
 * Cú pháp lệnh:
 *   "CLS"         → Xóa toàn bộ màn hình
 *   "L<n>:<text>" → Hiển thị <text> lên dòng n (1–8)
 *                   Ví dụ: "L1:Hello BBB"
 *   "<text>"      → Hiển thị lên dòng 1 (mặc định)
 * ================================================================ */
static ssize_t ssd1306_write(struct file *file, const char __user *user_buf,
                              size_t size, loff_t *offset)
{
    char buf[64] = {0};
    char text[20] = {0};
    int page = 0;  /* 0-indexed, page 0 = dòng 1 */
    int text_len;

    if (size > sizeof(buf) - 1)
        size = sizeof(buf) - 1;
    if (copy_from_user(buf, user_buf, size))
        return -EFAULT;

    /* Loại bỏ newline cuối */
    if (size > 0 && buf[size-1] == '\n')
        buf[--size] = '\0';
    buf[size] = '\0';

    /* --- Lệnh CLS: xóa màn hình --- */
    if (strncmp(buf, "CLS", 3) == 0) {
        ssd1306_clear_fb();
        ssd1306_flush();
        printk(KERN_INFO "SSD1306: Xoa man hinh\n");
        return size + 1;
    }

    /* --- Lệnh L<n>:<text>: hiển thị lên dòng cụ thể --- */
    if (buf[0] == 'L' && buf[1] >= '1' && buf[1] <= '8' && buf[2] == ':') {
        page = buf[1] - '1';   /* L1 → page 0, L8 → page 7 */
        strncpy(text, buf + 3, sizeof(text) - 1);
        text_len = strlen(text);
    } else {
        /* Mặc định: hiển thị lên dòng 1 */
        strncpy(text, buf, sizeof(text) - 1);
        text_len = strlen(text);
    }

    /* Giới hạn 16 ký tự/dòng (128px / 8px per char) */
    if (text_len > 16) text_len = 16;
    
    mutex_lock(&oled_mutex);
    fb_draw_string((u8)page, text, text_len);
    ssd1306_flush();
    mutex_unlock(&oled_mutex);
    printk(KERN_INFO "SSD1306: Dong %d: \"%s\"\n", page + 1, text);
    return size + 1;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .write = ssd1306_write,
};

/* ================================================================
 * Module Init
 * ================================================================ */
static int __init ssd1306_init(void)
{
    void __iomem *cm_per_mem;
    void __iomem *ctrl_mod_mem;
    int ret;

    printk(KERN_INFO "SSD1306: Khoi tao driver OLED I2C1 (P9_17/P9_18)...\n");

    /* --- 1. Cấp clock I2C1 --- */
    cm_per_mem = ioremap(CM_PER_BASE, SZ_4K);
    if (!cm_per_mem) return -ENOMEM;
    writel(0x02, cm_per_mem + CM_PER_I2C1_CLKCTRL);
    while (readl(cm_per_mem + CM_PER_I2C1_CLKCTRL) & (0x3 << 16))
        cpu_relax();
    iounmap(cm_per_mem);

    /* --- 2. Pinmux P9_17 (SCL) và P9_18 (SDA) → Mode 2 I2C1 --- */
    ctrl_mod_mem = ioremap(CTRL_MOD_BASE, SZ_8K);
    if (!ctrl_mod_mem) return -ENOMEM;
    writel(I2C_PINMUX_VAL, ctrl_mod_mem + P9_17_SCL_MUX_OFFSET);
    writel(I2C_PINMUX_VAL, ctrl_mod_mem + P9_18_SDA_MUX_OFFSET);
    iounmap(ctrl_mod_mem);

    /* --- 3. Map I2C1 registers --- */
    i2c1_base = ioremap(I2C1_BASE_PHYS, I2C_MAP_SIZE);
    if (!i2c1_base) return -ENOMEM;

    /* --- 4. Khởi tạo I2C1 controller --- */
    ret = i2c1_hw_init();
    if (ret < 0) {
        iounmap(i2c1_base);
        return ret;
    }

    /* --- 5. Khởi tạo SSD1306 --- */
    ret = ssd1306_init_display();
    if (ret < 0) {
        printk(KERN_ERR "SSD1306: Khong the init display! Kiem tra ket noi I2C.\n");
        iounmap(i2c1_base);
        return ret;
    }

    /* --- 6. Đăng ký character device --- */
    if (alloc_chrdev_region(&dev_num, 0, 1, DRIVER_NAME) < 0) {
        iounmap(i2c1_base);
        return -ENODEV;
    }
    cdev_init(&ssd1306_cdev, &fops);
    if (cdev_add(&ssd1306_cdev, dev_num, 1) < 0) {
        unregister_chrdev_region(dev_num, 1);
        iounmap(i2c1_base);
        return -ENODEV;
    }
    ssd1306_class = class_create(CLASS_NAME);
    if (IS_ERR(ssd1306_class)) {
        cdev_del(&ssd1306_cdev);
        unregister_chrdev_region(dev_num, 1);
        iounmap(i2c1_base);
        return PTR_ERR(ssd1306_class);
    }
    device_create(ssd1306_class, NULL, dev_num, NULL, DRIVER_NAME);

    printk(KERN_INFO "SSD1306: San sang! /dev/%s\n", DRIVER_NAME);
    printk(KERN_INFO "  echo 'L1:Hello BBB' > /dev/%s\n", DRIVER_NAME);
    printk(KERN_INFO "  echo 'L2:Temp: 28C' > /dev/%s\n", DRIVER_NAME);
    printk(KERN_INFO "  echo 'CLS'          > /dev/%s\n", DRIVER_NAME);
    return 0;
}

/* ================================================================
 * Module Exit
 * ================================================================ */
static void __exit ssd1306_exit(void)
{
    if (i2c1_base) {
        ssd1306_cmd(SSD1306_DISPLAY_OFF);
        writew(0, i2c1_base + I2C_CON);
        iounmap(i2c1_base);
    }
    device_destroy(ssd1306_class, dev_num);
    class_destroy(ssd1306_class);
    cdev_del(&ssd1306_cdev);
    unregister_chrdev_region(dev_num, 1);
    printk(KERN_INFO "SSD1306: Driver da go bo.\n");
}

module_init(ssd1306_init);
module_exit(ssd1306_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSD1306 OLED I2C Driver for BeagleBone Black");
