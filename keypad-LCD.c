#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"

// Keypad Configuration
#define ROWS 4
#define COLS 4
#define DEBOUNCE_DELAY_MS 20
#define SCAN_INTERVAL_MS 50

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
#define I2C_MASTER_SCL_IO           GPIO_NUM_22
#define I2C_MASTER_SDA_IO           GPIO_NUM_21
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000
#define LCD_ADDR                    0x27
#define LCD_COLUMNS                 16
#define LCD_ROWS                    2

// LCD Commands
#define LCD_CLEAR_DISPLAY           0x01
#define LCD_RETURN_HOME             0x02
#define LCD_ENTRY_MODE_SET          0x04
#define LCD_DISPLAY_CONTROL         0x08
#define LCD_FUNCTION_SET            0x20
#define LCD_SET_DDRAM_ADDR          0x80
#define LCD_BACKLIGHT               0x08
#define LCD_DATA                    0x01
#define LCD_COMMAND                 0x00

// Global variables
static QueueHandle_t keypad_queue;
static char lcd_buffer[LCD_ROWS][LCD_COLUMNS + 1];
static int cursor_pos = 0;
static int current_row = 0;

// Keypad event structure
typedef struct {
    char key;
    bool is_pressed;
} keypad_event_t;

// Function declarations
static esp_err_t i2c_master_init(void);
static void lcd_send_cmd(uint8_t cmd);
static void lcd_send_data(uint8_t data);
static void lcd_init(void);
static void lcd_set_cursor(uint8_t col, uint8_t row);
static void lcd_print_str(const char *str);
static void update_lcd(void);
void keypad_init(void);
bool is_key_pressed(int row, int col);
void keypad_task(void *pvParameter);
void handle_keypress(char key);
void keypad_handler_task(void *pvParameter);

// Initialize I2C
static esp_err_t i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        return err;
    }
    
    return i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

// Send command to LCD
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

// Send data to LCD
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

// Initialize LCD
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

// Set cursor position
static void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t row_offsets[] = {0x00, 0x40};
    if (row >= LCD_ROWS) {
        row = LCD_ROWS - 1;
    }
    lcd_send_cmd(LCD_SET_DDRAM_ADDR | (col + row_offsets[row]));
}

// Print string to LCD
static void lcd_print_str(const char *str) {
    while (*str) {
        lcd_send_data(*str++);
    }
}

// Update LCD display
static void update_lcd(void) {
    lcd_send_cmd(LCD_CLEAR_DISPLAY);
    vTaskDelay(pdMS_TO_TICKS(2));
    
    for (int row = 0; row < LCD_ROWS; row++) {
        lcd_set_cursor(0, row);
        lcd_print_str(lcd_buffer[row]);
    }
    
    lcd_set_cursor(cursor_pos, current_row);
}

// Initialize keypad
void keypad_init(void) {
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
}

// Check if key is pressed
bool is_key_pressed(int row, int col) {
    gpio_set_level(col_pins[col], 0);
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_DELAY_MS));
    bool pressed = (gpio_get_level(row_pins[row]) == 0);
    gpio_set_level(col_pins[col], 1);
    return pressed;
}

// Keypad scanning task
void keypad_task(void *pvParameter) {
    while (1) {
        for (int c = 0; c < COLS; c++) {
            gpio_set_level(col_pins[c], 0);
            
            for (int r = 0; r < ROWS; r++) {
                if (is_key_pressed(r, c)) {
                    while (is_key_pressed(r, c)) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    
                    keypad_event_t event = {
                        .key = keymap[r][c],
                        .is_pressed = true
                    };
                    xQueueSend(keypad_queue, &event, portMAX_DELAY);
                }
            }
            
            gpio_set_level(col_pins[c], 1);
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

// Handle keypad input
void handle_keypress(char key) {
    if (cursor_pos < LCD_COLUMNS) {
        lcd_buffer[current_row][cursor_pos] = key;
        cursor_pos++;
    } else if (current_row < LCD_ROWS - 1) {
        current_row++;
        cursor_pos = 0;
        lcd_buffer[current_row][cursor_pos] = key;
        cursor_pos++;
    } else {
        memcpy(lcd_buffer[0], lcd_buffer[1], LCD_COLUMNS);
        memset(lcd_buffer[1], ' ', LCD_COLUMNS);
        lcd_buffer[1][0] = key;
        cursor_pos = 1;
    }
    
    update_lcd();
}

// Keypad event handler task
void keypad_handler_task(void *pvParameter) {
    keypad_event_t event;
    
    memset(lcd_buffer, ' ', sizeof(lcd_buffer));
    for (int i = 0; i < LCD_ROWS; i++) {
        lcd_buffer[i][LCD_COLUMNS] = '\0';
    }
    strncpy(lcd_buffer[0], "Press keys:", LCD_COLUMNS);
    update_lcd();
    
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
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI("LCD", "I2C initialized");
    
    lcd_init();
    ESP_LOGI("LCD", "LCD initialized");
    
    keypad_init();
    ESP_LOGI("KEYPAD", "Keypad initialized");
    
    keypad_queue = xQueueCreate(10, sizeof(keypad_event_t));
    
    xTaskCreate(keypad_task, "keypad_scan", 4096, NULL, 5, NULL);
    xTaskCreate(keypad_handler_task, "keypad_handler", 4096, NULL, 4, NULL);
    
    ESP_LOGI("MAIN", "Application started");
}
