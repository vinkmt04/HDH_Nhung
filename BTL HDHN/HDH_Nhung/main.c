#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>
#include <mosquitto.h>

// ================= CẤU HÌNH THINGSBOARD =================
#define TB_HOST     "mqtt.thingsboard.cloud"
#define TB_PORT     1883
#define TB_TOKEN    "mjgvygdhp1a26xl28hm9"    
#define TB_TOPIC    "v1/devices/me/telemetry"
#define RPC_TOPIC   "v1/devices/me/rpc/request/+"

// ================= CẤU HÌNH DRIVER =====================
#define BUTTON_DEV  "/dev/button_driver"
#define ADC_DEV     "/dev/mq2_adc"
#define PWM_DEV     "/dev/pwm_driver"
#define OLED_DEV    "/dev/oled_i2c"
#define DHT11_DEV   "/dev/dht11"

// ================= MA TRẬN NGƯỠNG CẢNH BÁO ==============
#define GAS_LV1     1000
#define GAS_LV2     1500
#define GAS_LV3     2000

#define TEMP_LV1    33
#define TEMP_LV2    38
#define TEMP_LV3    40

#define HUMI_LV1    75
#define HUMI_LV2    80
#define HUMI_LV3    90

// ================= BIẾN TOÀN CỤC ========================
struct mosquitto *mosq = NULL;

int system_mode    = 0; // 0: Manual, 1: Auto
int fan_speed      = 0; // Tốc độ quạt: 0, 50, 70, 100
int buzzer_speed   = 0; // Tốc độ còi: 0, 50, 70, 100

int global_adc_raw = 0; 
int global_temp    = 0;
int global_humi    = 0;

// ================= IPC: HÀNG ĐỢI SỰ KIỆN NÚT NHẤN =======
#define MAX_EVENTS 16
int btn_events[MAX_EVENTS];
long btn_times[MAX_EVENTS];
int event_head = 0, event_tail = 0;

pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  event_cond  = PTHREAD_COND_INITIALIZER;

pthread_mutex_t buz_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  buz_cond   = PTHREAD_COND_INITIALIZER;
pthread_mutex_t pwm_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t oled_mutex = PTHREAD_MUTEX_INITIALIZER;

// ================= IPC: WATCHDOG ========================
pthread_mutex_t wd_mutex = PTHREAD_MUTEX_INITIALIZER;
long main_heartbeat = 0; // Biến lưu "nhịp tim" của luồng chính

// ================= HÀM TIỆN ÍCH THỜI GIAN ===============
long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

// ================= HÀM HỖ TRỢ HIỂN THỊ OLED =============
void oled_printf(int line, const char *format, ...) {
    char text[32];
    char cmd[64];
    va_list args;
    
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    
    snprintf(cmd, sizeof(cmd), "L%d:%s", line, text);
    
    pthread_mutex_lock(&oled_mutex);
    int fd = open(OLED_DEV, O_WRONLY);
    if (fd >= 0) {
        write(fd, cmd, strlen(cmd));
        close(fd);
    }
    pthread_mutex_unlock(&oled_mutex);
}

void oled_init_interface() {
    pthread_mutex_lock(&oled_mutex);
    int fd = open(OLED_DEV, O_WRONLY);
    if (fd >= 0) {
        write(fd, "CLS", 3);
        usleep(20000); 
        write(fd, "L1:=== SMART SYS ===", 20);
        close(fd);
    }
    pthread_mutex_unlock(&oled_mutex);

    oled_printf(2, "MODE: MANUAL");
    oled_printf(3, "GAS:  0");
    oled_printf(4, "TMP:--C HUM:--%%");
    oled_printf(5, "FAN:  0%%");
    oled_printf(6, "BUZ:  OFF");
}

// ================= ĐIỀU KHIỂN & MQTT ====================
void publish_telemetry() {
    char payload[256];
    snprintf(payload, sizeof(payload), 
             "{\"system_mode\": %d, \"fan_speed\": %d, \"buzzer_speed\": %d, \"gas_raw\": %d, \"temperature\": %d, \"humidity\": %d}", 
             system_mode, fan_speed, buzzer_speed, global_adc_raw, global_temp, global_humi);
    mosquitto_publish(mosq, NULL, TB_TOPIC, strlen(payload), payload, 0, false);
}

void set_fan_speed(int speed) {
    if (fan_speed == speed) return; 
    
    fan_speed = speed;
    pthread_mutex_lock(&pwm_mutex);
    int fd = open(PWM_DEV, O_WRONLY);
    if (fd >= 0) {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "FAN %d\n", speed);
        write(fd, cmd, strlen(cmd));
        close(fd);
    }
    pthread_mutex_unlock(&pwm_mutex);

    oled_printf(5, "FAN:  %d%%", speed);
}

// ====================================================================
// XỬ LÝ LỆNH RPC TỪ DASHBOARD TRUYỀN XUỐNG
// ====================================================================
void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    if (!msg->payload) return;

    char *payload_str = (char *)msg->payload;
    printf("[RPC_RECV] Nhan lenh tu Dashboard: %s\n", payload_str);
    
    int processed = 0;

    if (strstr(payload_str, "setMode") != NULL) {
        system_mode = !system_mode;
        oled_printf(2, "MODE: %s", system_mode ? "AUTO" : "MANUAL");
        
        if (system_mode == 1) {
            set_fan_speed(0);
            pthread_mutex_lock(&buz_mutex);
            buzzer_speed = 0;
            pthread_cond_signal(&buz_cond);
            pthread_mutex_unlock(&buz_mutex);
            oled_printf(6, "BUZ:  OFF");
        }
        publish_telemetry();
        processed = 1;
    }
    else if (strstr(payload_str, "setFan") != NULL) {
        if (system_mode == 0) {
            set_fan_speed(fan_speed > 0 ? 0 : 100); 
            publish_telemetry();
        } 
        processed = 1;
    }
    else if (strstr(payload_str, "setBuzzer") != NULL) {
        if (system_mode == 0) {
            pthread_mutex_lock(&buz_mutex);
            buzzer_speed = (buzzer_speed > 0) ? 0 : 50; 
            pthread_cond_signal(&buz_cond); 
            pthread_mutex_unlock(&buz_mutex);
            
            if (buzzer_speed == 0) oled_printf(6, "BUZ:  OFF");
            else oled_printf(6, "BUZ:  BEEP %d%%", buzzer_speed);
            
            publish_telemetry();
        } 
        processed = 1;
    }

    if (processed && strncmp(msg->topic, "v1/devices/me/rpc/request/", 26) == 0) {
        char response_topic[64];
        const char *req_id = msg->topic + 26; 
        snprintf(response_topic, sizeof(response_topic), "v1/devices/me/rpc/response/%s", req_id);
        mosquitto_publish(mosq, NULL, response_topic, 2, "{}", 0, false); 
    }
}

// ================= QUẢN LÝ SỰ KIỆN NÚT NHẤN VẬT LÝ =============
void push_btn_event(int val, long time) {
    pthread_mutex_lock(&event_mutex);
    btn_events[event_head] = val;
    btn_times[event_head] = time;
    event_head = (event_head + 1) % MAX_EVENTS;
    pthread_cond_signal(&event_cond);
    pthread_mutex_unlock(&event_mutex);
}

int pop_btn_event(int *val, long *time, struct timespec *ts) {
    pthread_mutex_lock(&event_mutex);
    while (event_head == event_tail) {
        if (ts == NULL) {
            pthread_cond_wait(&event_cond, &event_mutex);
        } else {
            int ret = pthread_cond_timedwait(&event_cond, &event_mutex, ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&event_mutex);
                return 0; 
            }
        }
    }
    *val = btn_events[event_tail];
    *time = btn_times[event_tail];
    event_tail = (event_tail + 1) % MAX_EVENTS;
    pthread_mutex_unlock(&event_mutex);
    return 1;
}

// ================= LUỒNG 1: ĐỌC DRIVER NÚT NHẤN =========
void *button_reader_thread(void *arg) {
    char buf[16];
    while (1) {
        int fd = open(BUTTON_DEV, O_RDONLY);
        if (fd < 0) { sleep(1); continue; }

        if (read(fd, buf, sizeof(buf)) > 0) {
            push_btn_event(atoi(buf), get_time_ms()); 
        }
        close(fd); 
    }
    return NULL;
}

// ================= LUỒNG 2: XỬ LÝ LOGIC NÚT NHẤN VẬT LÝ ========
void *button_logic_thread(void *arg) {
    int val;
    long time;

    while (1) {
        pop_btn_event(&val, &time, NULL);
        if (val != 1) continue; 
        long t_press = time;

        pop_btn_event(&val, &time, NULL);
        if (val != 0) continue; 
        long t_release = time;

        long duration = t_release - t_press;

        if (duration >= 2000) { 
            system_mode = !system_mode;
            oled_printf(2, "MODE: %s", system_mode ? "AUTO" : "MANUAL");
            
            if (system_mode == 1) {
                set_fan_speed(0);
                pthread_mutex_lock(&buz_mutex);
                if (buzzer_speed != 0) {
                    buzzer_speed = 0;
                    pthread_cond_signal(&buz_cond);
                }
                pthread_mutex_unlock(&buz_mutex);
                oled_printf(6, "BUZ:  OFF");
            }
            publish_telemetry();
        } 
        else if (system_mode == 0) { 
            struct timeval now;
            struct timespec ts;
            gettimeofday(&now, NULL);
            ts.tv_sec = now.tv_sec;
            ts.tv_nsec = (now.tv_usec * 1000) + (400 * 1000000); 
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }

            if (pop_btn_event(&val, &time, &ts) == 0) {
                set_fan_speed(fan_speed > 0 ? 0 : 100);
                publish_telemetry();
            } else if (val == 1) {
                pop_btn_event(&val, &time, NULL); 
                
                pthread_mutex_lock(&buz_mutex);
                buzzer_speed = (buzzer_speed > 0) ? 0 : 50; 
                pthread_cond_signal(&buz_cond); 
                pthread_mutex_unlock(&buz_mutex);
                
                if (buzzer_speed == 0) oled_printf(6, "BUZ:  OFF");
                else oled_printf(6, "BUZ:  BEEP %d%%", buzzer_speed);
                publish_telemetry();
            }
        }
    }
    return NULL;
}

// ================= LUỒNG 3: ĐIỀU KHIỂN CÒI NHẤP NHẢ ======
void *buzzer_thread_func(void *arg) {
    char cmd[16];
    while (1) {
        pthread_mutex_lock(&buz_mutex);
        while (buzzer_speed == 0) {
            pthread_cond_wait(&buz_cond, &buz_mutex);
        }
        int current_speed = buzzer_speed; 
        pthread_mutex_unlock(&buz_mutex);

        pthread_mutex_lock(&pwm_mutex);
        int fd = open(PWM_DEV, O_WRONLY);
        if (fd >= 0) { 
            snprintf(cmd, sizeof(cmd), "BUZ %d\n", current_speed);
            write(fd, cmd, strlen(cmd)); 
            close(fd); 
        }
        pthread_mutex_unlock(&pwm_mutex);
        
        usleep(300000); 

        pthread_mutex_lock(&pwm_mutex);
        fd = open(PWM_DEV, O_WRONLY);
        if (fd >= 0) { write(fd, "BUZ 0\n", 6); close(fd); }
        pthread_mutex_unlock(&pwm_mutex);
        
        usleep(300000); 
    }
    return NULL;
}

// ================= LUỒNG 4: WATCHDOG BẢO VỆ HỆ THỐNG ======
void *watchdog_thread_func(void *arg) {
    printf("[WATCHDOG] Luong giam sat he thong da kich hoat!\n");
    while (1) {
        sleep(5); // Kiểm tra nhịp tim mỗi 5 giây

        pthread_mutex_lock(&wd_mutex);
        long current_hb = main_heartbeat;
        pthread_mutex_unlock(&wd_mutex);

        // Nếu đã từng có nhịp tim (khởi động xong) và kẹt quá 15 giây
        if (current_hb > 0 && (get_time_ms() - current_hb) > 15000) {
            printf("\n[WATCHDOG] FATAL ERROR: Main thread hoac Ngoai vi bi treo > 15s!\n");
            printf("[WATCHDOG] Ep buoc khoi dong lai BeagleBone...\n");
            
            sync(); // Đồng bộ dữ liệu xuống thẻ nhớ/eMMC để tránh lỗi hệ điều hành
            system("reboot -f"); // Lệnh force reboot của Linux
        }
    }
    return NULL;
}

// ================= LUỒNG CHÍNH ==========================
int main() {
    char adc_buf[32], dht_buf[64];
    pthread_t thread_reader, thread_logic, thread_buzzer, thread_watchdog;

    printf("=== SMART SYSTEM: FULL INTEGRATION W/ WATCHDOG ===\n");

    oled_init_interface();

    mosquitto_lib_init();
    mosq = mosquitto_new("bbb_gateway", true, NULL);
    mosquitto_message_callback_set(mosq, on_message);
    mosquitto_username_pw_set(mosq, TB_TOKEN, NULL);

    if (mosquitto_connect(mosq, TB_HOST, TB_PORT, 60) == MOSQ_ERR_SUCCESS) {
        mosquitto_subscribe(mosq, NULL, RPC_TOPIC, 0); 
        mosquitto_loop_start(mosq);
    }

    set_fan_speed(0);
    int fd = open(PWM_DEV, O_WRONLY);
    if (fd >= 0) { write(fd, "BUZ 0\n", 6); close(fd); }

    pthread_create(&thread_reader, NULL, button_reader_thread, NULL);
    pthread_create(&thread_logic, NULL, button_logic_thread, NULL);
    pthread_create(&thread_buzzer, NULL, buzzer_thread_func, NULL);
    pthread_create(&thread_watchdog, NULL, watchdog_thread_func, NULL); // Khởi chạy Watchdog

    while (1) {
        // --- 0. BÁO CÁO NHỊP TIM CHO WATCHDOG ---
        pthread_mutex_lock(&wd_mutex);
        main_heartbeat = get_time_ms();
        pthread_mutex_unlock(&wd_mutex);

        // --- 1. Đọc ADC Khí Gas thô ---
        int adc_fd = open(ADC_DEV, O_RDONLY);
        if (adc_fd >= 0) {
            memset(adc_buf, 0, sizeof(adc_buf));
            if (read(adc_fd, adc_buf, sizeof(adc_buf) - 1) > 0) {
                global_adc_raw = atoi(adc_buf); 
                oled_printf(3, "GAS:  %d", global_adc_raw);
            }
            close(adc_fd);
        }
        
        // --- 2. Đọc cảm biến DHT11 ---
        int dht_fd = open(DHT11_DEV, O_RDONLY);
        if (dht_fd >= 0) {
            memset(dht_buf, 0, sizeof(dht_buf));
            if (read(dht_fd, dht_buf, sizeof(dht_buf) - 1) > 0) {
                sscanf(dht_buf, "Nhiet do: %d C | Do am: %d", &global_temp, &global_humi);
                oled_printf(4, "TMP:%dC HUM:%d%%", global_temp, global_humi);
            }
            close(dht_fd);
        }

        // --- 3. Xử lý Ma trận Logic tự động (AUTO MODE) ---
        if (system_mode == 1) {
            int target_speed = 0;

            if (global_adc_raw >= GAS_LV3 || global_temp >= TEMP_LV3 || global_humi >= HUMI_LV3) {
                target_speed = 100;
            } 
            else if (global_adc_raw >= GAS_LV2 || global_temp >= TEMP_LV2 || global_humi >= HUMI_LV2) {
                target_speed = 70;
            } 
            else if (global_adc_raw >= GAS_LV1 || global_temp >= TEMP_LV1 || global_humi >= HUMI_LV1) {
                target_speed = 50;
            }

            set_fan_speed(target_speed);

            pthread_mutex_lock(&buz_mutex);
            if (buzzer_speed != target_speed) {
                buzzer_speed = target_speed;
                pthread_cond_signal(&buz_cond);
            }
            pthread_mutex_unlock(&buz_mutex);
            
            if (buzzer_speed == 0) oled_printf(6, "BUZ:  OFF");
            else oled_printf(6, "BUZ:  BEEP %d%%", buzzer_speed);
        }

        // --- 4. Định kỳ đồng bộ dữ liệu Telemetry ---
        publish_telemetry();
        
        sleep(2); 
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    
    return 0;
}
