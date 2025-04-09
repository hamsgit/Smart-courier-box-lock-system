#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

// Hardware Configuration
#define ROWS 4
#define COLS 4
#define LOCK_PIN GPIO_NUM_4
#define GUEST_LED_PIN GPIO_NUM_18  // LED pin for guest password usage
#define MASTER_LED_PIN GPIO_NUM_5  // LED pin for master password usage
#define DEBOUNCE_DELAY_MS 20
#define SCAN_INTERVAL_MS 30
#define UNLOCK_DURATION_MS 3000
#define LED_BLINK_DURATION_MS 1000  // Duration for LED to blink when password is used

// Keypad GPIO Pins
const gpio_num_t row_pins[ROWS] = {GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_27};
const gpio_num_t col_pins[COLS] = {GPIO_NUM_26, GPIO_NUM_25, GPIO_NUM_33, GPIO_NUM_32};

// Keypad Layout
static const char keymap[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// LCD Configuration
#define I2C_MASTER_SCL_IO GPIO_NUM_22
#define I2C_MASTER_SDA_IO GPIO_NUM_21
#define I2C_MASTER_NUM I2C_NUM_0
#define LCD_ADDR 0x27
#define LCD_COLUMNS 16
#define LCD_ROWS 2

// LCD Commands
#define LCD_CLEAR_DISPLAY 0x01
#define LCD_RETURN_HOME 0x02
#define LCD_ENTRY_MODE_SET 0x04
#define LCD_DISPLAY_CONTROL 0x08
#define LCD_FUNCTION_SET 0x20
#define LCD_SET_DDRAM_ADDR 0x80
#define LCD_BACKLIGHT 0x08
#define LCD_DATA 0x01
#define LCD_COMMAND 0x00

// Menu States
typedef enum {
    MAIN_MENU,
    UNLOCK_MODE,
    SETTINGS_MENU,
    CHANGE_MASTER_PASSWORD,
    CHANGE_GUEST_PASSWORD,
    VERIFY_MASTER_PASSWORD,
    LOCKED_STATE
} menu_state_t;

// Keypad event structure
typedef struct {
    char key;
    bool is_pressed;
} keypad_event_t;

// System Structure
typedef struct {
    char master_password[6];    // Master password
    char guest_password[6];     // Guest password
    char input_buffer[6];
    int input_pos;
    menu_state_t state;
    menu_state_t previous_state;
    char last_key;
    bool authenticated;
    bool guest_password_used;   // Flag to track if guest password has been used
} lock_system_t;

// Global Variables
static QueueHandle_t keypad_queue;
static lock_system_t lock_system;
static nvs_handle_t nvs_handler;

// Function Declarations
static void lcd_send_cmd(uint8_t cmd);
static void lcd_send_data(uint8_t data);
static void lcd_init(void);
static void lcd_set_cursor(uint8_t col, uint8_t row);
static void lcd_print_str(const char *str);
static void load_passwords(void);
static void save_passwords(void);
static void hardware_init(void);
static void control_lock(bool unlock);
static void display_menu(menu_state_t state);
static void handle_keypress(char key);
void keypad_task(void *pvParameter);
void app_task(void *pvParameter);

// LCD Functions
static void lcd_send_cmd(uint8_t cmd) {
    uint8_t data_u = (cmd & 0xF0) | LCD_BACKLIGHT;
    uint8_t data_l = ((cmd << 4) & 0xF0) | LCD_BACKLIGHT;
    
    uint8_t buf[4];
    buf[0] = data_u | 0x04;
    buf[1] = data_u & ~0x04;
    buf[2] = data_l | 0x04;
    buf[3] = data_l & ~0x04;
    
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (LCD_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd_handle, buf, 4, true);
    i2c_master_stop(cmd_handle);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd_handle);
}

static void lcd_send_data(uint8_t data) {
    uint8_t data_u = (data & 0xF0) | LCD_BACKLIGHT | LCD_DATA;
    uint8_t data_l = ((data << 4) & 0xF0) | LCD_BACKLIGHT | LCD_DATA;
    
    uint8_t buf[4];
    buf[0] = data_u | 0x04;
    buf[1] = data_u & ~0x04;
    buf[2] = data_l | 0x04;
    buf[3] = data_l & ~0x04;
    
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (LCD_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd_handle, buf, 4, true);
    i2c_master_stop(cmd_handle);
    i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd_handle);
}

static void lcd_init(void) {
    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_send_cmd(0x03);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_send_cmd(0x03);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_cmd(0x03);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_cmd(0x02);
    
    lcd_send_cmd(LCD_FUNCTION_SET | 0x08 | 0x00);
    lcd_send_cmd(LCD_DISPLAY_CONTROL | 0x04);
    lcd_send_cmd(LCD_CLEAR_DISPLAY);
    vTaskDelay(pdMS_TO_TICKS(2));
    lcd_send_cmd(LCD_ENTRY_MODE_SET | 0x02);
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t row_offsets[] = {0x00, 0x40};
    if (row >= LCD_ROWS) {
        row = LCD_ROWS - 1;
    }
    lcd_send_cmd(LCD_SET_DDRAM_ADDR | (col + row_offsets[row]));
}

static void lcd_print_str(const char *str) {
    while (*str) {
        lcd_send_data(*str++);
    }
}

// Password Management
static void load_passwords(void) {
    size_t required_size = sizeof(lock_system.master_password);
    esp_err_t err = nvs_get_blob(nvs_handler, "master_password", lock_system.master_password, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND || required_size == 0) {
        strcpy(lock_system.master_password, "1234");
        save_passwords();
    }
    
    required_size = sizeof(lock_system.guest_password);
    err = nvs_get_blob(nvs_handler, "guest_password", lock_system.guest_password, &required_size);
    
    if (err == ESP_ERR_NVS_NOT_FOUND || required_size == 0) {
        strcpy(lock_system.guest_password, "5678");
        save_passwords();
    }

    // Load guest password usage status
    err = nvs_get_u8(nvs_handler, "guest_used", (uint8_t*)&lock_system.guest_password_used);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        lock_system.guest_password_used = false;
        nvs_set_u8(nvs_handler, "guest_used", lock_system.guest_password_used);
        nvs_commit(nvs_handler);
    }
}

static void save_passwords(void) {
    nvs_set_blob(nvs_handler, "master_password", lock_system.master_password, sizeof(lock_system.master_password));
    nvs_set_blob(nvs_handler, "guest_password", lock_system.guest_password, sizeof(lock_system.guest_password));
    nvs_set_u8(nvs_handler, "guest_used", lock_system.guest_password_used);
    nvs_commit(nvs_handler);
}

// Hardware Initialization
static void hardware_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handler));

    // Initialize lock pin
    gpio_reset_pin(LOCK_PIN);
    gpio_set_direction(LOCK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LOCK_PIN, 1);

    // Initialize guest LED pin
    gpio_reset_pin(GUEST_LED_PIN);
    gpio_set_direction(GUEST_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(GUEST_LED_PIN, 0);

    // Initialize master LED pin
    gpio_reset_pin(MASTER_LED_PIN);
    gpio_set_direction(MASTER_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MASTER_LED_PIN, 0);

    for (int i = 0; i < ROWS; i++) {
        gpio_reset_pin(row_pins[i]);
        gpio_set_direction(row_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(row_pins[i], GPIO_PULLUP_ONLY);
    }

    for (int i = 0; i < COLS; i++) {
        gpio_reset_pin(col_pins[i]);
        gpio_set_direction(col_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(col_pins[i], 1);
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0));
    
    lcd_init();
}

// Lock Control
static void control_lock(bool unlock) {
    gpio_set_level(LOCK_PIN, unlock ? 0 : 1);
    if (unlock) {
        ESP_LOGI("LOCK", "Door unlocked");
        vTaskDelay(pdMS_TO_TICKS(UNLOCK_DURATION_MS));
        gpio_set_level(LOCK_PIN, 1);
        ESP_LOGI("LOCK", "Door relocked");
    }
}

// Menu Display
static void display_menu(menu_state_t state) {
    lcd_send_cmd(LCD_CLEAR_DISPLAY);
    vTaskDelay(pdMS_TO_TICKS(2));
    
    switch(state) {
        case MAIN_MENU:
            lcd_print_str("  1: Unlock");
            lcd_set_cursor(0, 1);
            lcd_print_str("2: Settings 3:Exit");
            break;
            
        case UNLOCK_MODE:
            lcd_print_str("  Enter Password:");
            lcd_set_cursor(0, 1);
            for (int i = 0; i < lock_system.input_pos; i++) {
                lcd_print_str("*");
            }
            break;
            
        case SETTINGS_MENU:
            lcd_print_str("  Settings Menu");
            lcd_set_cursor(0, 1);
            lcd_print_str("A:Master B:Guest C:Back");
            break;
            
        case VERIFY_MASTER_PASSWORD:
            lcd_print_str("  Master Password:");
            lcd_set_cursor(0, 1);
            for (int i = 0; i < lock_system.input_pos; i++) {
                lcd_print_str("*");
            }
            break;
            
        case CHANGE_MASTER_PASSWORD:
            lcd_print_str("  New Master Pass:");
            lcd_set_cursor(0, 1);
            for (int i = 0; i < lock_system.input_pos; i++) {
                lcd_print_str("*");
            }
            break;
            
        case CHANGE_GUEST_PASSWORD:
            lcd_print_str("  New Guest Pass:");
            lcd_set_cursor(0, 1);
            for (int i = 0; i < lock_system.input_pos; i++) {
                lcd_print_str("*");
            }
            break;
            
        case LOCKED_STATE:
            lcd_print_str("  System Locked");
            break;
    }
}

// Keypad Handling
static void handle_keypress(char key) {
    // Handle cancel button
    if (key == 'C') {
        if (lock_system.state == SETTINGS_MENU) {
            lock_system.state = MAIN_MENU;
        } else if (lock_system.state == VERIFY_MASTER_PASSWORD || 
                   lock_system.state == CHANGE_MASTER_PASSWORD ||
                   lock_system.state == CHANGE_GUEST_PASSWORD) {
            lock_system.state = SETTINGS_MENU;
        } else {
            lock_system.state = MAIN_MENU;
        }
        memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
        lock_system.input_pos = 0;
        display_menu(lock_system.state);
        return;
    }

    switch(lock_system.state) {
        case MAIN_MENU:
            if (key == '1') {
                lock_system.state = UNLOCK_MODE;
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == '2') {
                lock_system.state = SETTINGS_MENU;
            } else if (key == '3') {
                lock_system.state = LOCKED_STATE;
            }
            break;
            
        case UNLOCK_MODE:
            if (key == '#') {
                if (strcmp(lock_system.input_buffer, lock_system.master_password) == 0) {
                    control_lock(true);
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("  Access Granted!");
                    
                    // Blink master LED when master password is used
                    for (int i = 0; i < 3; i++) {
                        gpio_set_level(MASTER_LED_PIN, 1);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        gpio_set_level(MASTER_LED_PIN, 0);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(2000));
                } else if (strcmp(lock_system.input_buffer, lock_system.guest_password) == 0) {
                    if (!lock_system.guest_password_used) {
                        control_lock(true);
                        lcd_send_cmd(LCD_CLEAR_DISPLAY);
                        lcd_print_str("  Access Granted!");
                        
                        // Blink guest LED when guest password is used
                        for (int i = 0; i < 3; i++) {
                            gpio_set_level(GUEST_LED_PIN, 1);
                            vTaskDelay(pdMS_TO_TICKS(200));
                            gpio_set_level(GUEST_LED_PIN, 0);
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                        
                        lock_system.guest_password_used = true;
                        save_passwords();
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        lcd_send_cmd(LCD_CLEAR_DISPLAY);
                        lcd_print_str("Guest Pass Used!");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                } else {
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("  Wrong Password!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                lock_system.state = MAIN_MENU;
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == '*' && lock_system.input_pos > 0) {
                lock_system.input_buffer[--lock_system.input_pos] = '\0';
            } else if (key >= '0' && key <= '9' && lock_system.input_pos < 5) {
                lock_system.input_buffer[lock_system.input_pos++] = key;
            }
            break;
            
        case SETTINGS_MENU:
            if (key == 'A') {
                lock_system.state = VERIFY_MASTER_PASSWORD;
                lock_system.previous_state = SETTINGS_MENU;
                lock_system.last_key = 'A';
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == 'B') {
                lock_system.state = VERIFY_MASTER_PASSWORD;
                lock_system.previous_state = SETTINGS_MENU;
                lock_system.last_key = 'B';
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == 'C') {
                lock_system.state = MAIN_MENU;
            }
            break;
            
        case VERIFY_MASTER_PASSWORD:
            if (key == '#') {
                if (strcmp(lock_system.input_buffer, lock_system.master_password) == 0) {
                    if (lock_system.last_key == 'A') {
                        lock_system.state = CHANGE_MASTER_PASSWORD;
                    } else if (lock_system.last_key == 'B') {
                        lock_system.state = CHANGE_GUEST_PASSWORD;
                    }
                    memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                    lock_system.input_pos = 0;
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Enter New Pass");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                } else {
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Wrong Password!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    lock_system.state = SETTINGS_MENU;
                }
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == '*' && lock_system.input_pos > 0) {
                lock_system.input_buffer[--lock_system.input_pos] = '\0';
            } else if (key >= '0' && key <= '9' && lock_system.input_pos < 5) {
                lock_system.input_buffer[lock_system.input_pos++] = key;
            }
            break;
            
        case CHANGE_MASTER_PASSWORD:
            if (key == '#') {
                if (lock_system.input_pos >= 4) {
                    strcpy(lock_system.master_password, lock_system.input_buffer);
                    save_passwords();
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Master Pass Changed!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                } else {
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Min 4 digits!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                lock_system.state = SETTINGS_MENU;
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == '*' && lock_system.input_pos > 0) {
                lock_system.input_buffer[--lock_system.input_pos] = '\0';
            } else if (key >= '0' && key <= '9' && lock_system.input_pos < 5) {
                lock_system.input_buffer[lock_system.input_pos++] = key;
            }
            break;
            
        case CHANGE_GUEST_PASSWORD:
            if (key == '#') {
                if (lock_system.input_pos >= 4) {
                    strcpy(lock_system.guest_password, lock_system.input_buffer);
                    lock_system.guest_password_used = false;  // Reset the usage flag
                    save_passwords();
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Guest Pass Changed!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                } else {
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Min 4 digits!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                lock_system.state = SETTINGS_MENU;
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == '*' && lock_system.input_pos > 0) {
                lock_system.input_buffer[--lock_system.input_pos] = '\0';
            } else if (key >= '0' && key <= '9' && lock_system.input_pos < 5) {
                lock_system.input_buffer[lock_system.input_pos++] = key;
            }
            break;
            
        case LOCKED_STATE:
            if (key == '#') {
                if (strcmp(lock_system.input_buffer, lock_system.master_password) == 0) {
                    lock_system.state = MAIN_MENU;
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("System Unlocked");
                    
                    // Blink master LED when master password is used
                    for (int i = 0; i < 3; i++) {
                        gpio_set_level(MASTER_LED_PIN, 1);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        gpio_set_level(MASTER_LED_PIN, 0);
                        vTaskDelay(pdMS_TO_TICKS(200));
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(2000));
                } else if (strcmp(lock_system.input_buffer, lock_system.guest_password) == 0) {
                    if (!lock_system.guest_password_used) {
                        lock_system.state = MAIN_MENU;
                        lcd_send_cmd(LCD_CLEAR_DISPLAY);
                        lcd_print_str("System Unlocked");
                        
                        // Blink guest LED when guest password is used
                        for (int i = 0; i < 3; i++) {
                            gpio_set_level(GUEST_LED_PIN, 1);
                            vTaskDelay(pdMS_TO_TICKS(200));
                            gpio_set_level(GUEST_LED_PIN, 0);
                            vTaskDelay(pdMS_TO_TICKS(200));
                        }
                        
                        lock_system.guest_password_used = true;
                        save_passwords();
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        lcd_send_cmd(LCD_CLEAR_DISPLAY);
                        lcd_print_str("Guest Pass Used!");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                } else {
                    lcd_send_cmd(LCD_CLEAR_DISPLAY);
                    lcd_print_str("Wrong Password!");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                memset(lock_system.input_buffer, 0, sizeof(lock_system.input_buffer));
                lock_system.input_pos = 0;
            } else if (key == '*' && lock_system.input_pos > 0) {
                lock_system.input_buffer[--lock_system.input_pos] = '\0';
            } else if (key >= '0' && key <= '9' && lock_system.input_pos < 5) {
                lock_system.input_buffer[lock_system.input_pos++] = key;
            }
            break;
            
        default:
            break;
    }
    
    display_menu(lock_system.state);
}

// Keypad Task
void keypad_task(void *pvParameter) {
    while (1) {
        for (int c = 0; c < COLS; c++) {
            gpio_set_level(col_pins[c], 0);
            
            for (int r = 0; r < ROWS; r++) {
                if (gpio_get_level(row_pins[r]) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_DELAY_MS));
                    if (gpio_get_level(row_pins[r]) == 0) {
                        while (gpio_get_level(row_pins[r]) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                        
                        keypad_event_t event = {
                            .key = keymap[r][c],
                            .is_pressed = true
                        };
                        xQueueSend(keypad_queue, &event, portMAX_DELAY);
                    }
                }
            }
            
            gpio_set_level(col_pins[c], 1);
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

// Main Application Task
void app_task(void *pvParameter) {
    memset(&lock_system, 0, sizeof(lock_system));
    lock_system.state = MAIN_MENU;
    hardware_init();
    load_passwords();
    display_menu(MAIN_MENU);

    keypad_event_t event;
    while (1) {
        if (xQueueReceive(keypad_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event.is_pressed) {
                ESP_LOGI("KEYPAD", "Key pressed: %c", event.key);
                handle_keypress(event.key);
            }
        }
    }
}

void app_main() {
    keypad_queue = xQueueCreate(10, sizeof(keypad_event_t));
    
    xTaskCreate(keypad_task, "keypad_scan", 4096, NULL, 5, NULL);
    xTaskCreate(app_task, "app_task", 8192, NULL, 4, NULL);
    
    ESP_LOGI("MAIN", "Digital Lock System Started");
}
