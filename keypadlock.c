#include "driver/gpio.h"  
          // 
#include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
           // 
// Define LED and button GPIOs 
Includes the GPIO driver for controlling ESP32 GPIOs 
       // 
Includes the FreeRTOS library for task management 
Includes the task-related functionalities in FreeRTOS 
#define LED_PIN GPIO_NUM_2          // LED is connected to GPIO2 
#define BUTTON1 GPIO_NUM_3          // Button 1 is connected to GPIO3 
#define BUTTON2 GPIO_NUM_21         // Button 2 is connected to GPIO21 
#define BUTTON3 GPIO_NUM_19         // Button 3 is connected to GPIO19 
#define BUTTON4 GPIO_NUM_18         // Button 4 is connected to GPIO18 
// Delay function (ms) 
void delay_ms(int ms) { 
vTaskDelay(ms / portTICK_PERIOD_MS);    // Delays for 'ms' milliseconds. FreeRTOS uses ticks, so we 
convert ms to ticks 
} 
void app_main() { 
// Configure the LED pin as output 
gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);   // Sets GPIO2 (LED) as an output pin 
// Configure the buttons as input with internal pull-up resistors enabled 
gpio_set_direction(BUTTON1, GPIO_MODE_INPUT);       
// Sets GPIO3 (Button 1) as an input pin 
gpio_set_pull_mode(BUTTON1, GPIO_PULLUP_ONLY);      // Enables pull-up resistor for Button 1, 
ensuring the default state is HIGH 
gpio_set_direction(BUTTON2, GPIO_MODE_INPUT);       // Sets GPIO21 (Button 2) as an input pin 
gpio_set_pull_mode(BUTTON2, GPIO_PULLUP_ONLY);      // Enables pull-up resistor for Button 2 
gpio_set_direction(BUTTON3, GPIO_MODE_INPUT);       // Sets GPIO19 (Button 3) as an input pin 
gpio_set_pull_mode(BUTTON3, GPIO_PULLUP_ONLY);      // Enables pull-up resistor for Button 3 
gpio_set_direction(BUTTON4, GPIO_MODE_INPUT);       // Sets GPIO18 (Button 4) as an input pin 
gpio_set_pull_mode(BUTTON4, GPIO_PULLUP_ONLY);      // Enables pull-up resistor for Button 4 
int state = 0;  // Track button press sequence (initial state is 0, waiting for Button 1) 
while (1) {  // Start an infinite loop to continuously check button presses 
switch (state) {   // Use a state machine to handle button press sequence 
case 0:  // Expecting Button 1 
if (gpio_get_level(BUTTON1) == 0) {  // Button1 pressed (active low) 
state = 1;  // Transition to the next state (waiting for Button 2) 
delay_ms(200);  // Debounce delay to avoid multiple presses registering 
} 
break; 
case 1:  // Expecting Button 2 
if (gpio_get_level(BUTTON2) == 0) {  // Button2 pressed 
state = 2;  // Transition to the next state (waiting for Button 1) 
delay_ms(200);  // Debounce delay 
} 
break; 
case 2:  // Expecting Button 1 
if (gpio_get_level(BUTTON1) == 0) {  // Button1 pressed again 
state = 3;  // Transition to the next state (waiting for Button 4) 
delay_ms(200);  // Debounce delay 
} 
break; 
case 3:  // Expecting Button 4 
if (gpio_get_level(BUTTON4) == 0) {  // Button4 pressed 
state = 4;  // Transition to the next state (waiting for Button 2) 
delay_ms(200);  // Debounce delay 
} 
break; 
case 4:  // Expecting Button 2 
if (gpio_get_level(BUTTON2) == 0) {  // Button2 pressed 
state = 5;  // Transition to the next state (waiting for Button 3) 
} 
delay_ms(200);  // Debounce delay 
break; 
case 5:  // Expecting Button 3 
if (gpio_get_level(BUTTON3) == 0) {  // Button3 pressed 
state = 6;  // Transition to the next state (waiting for Button 1) 
delay_ms(200);  // Debounce delay 
} 
break; 
case 6:  // Expecting Button 1 
if (gpio_get_level(BUTTON1) == 0) {  // Button1 pressed again 
state = 7;  // Transition to the next state (waiting for Button 4) 
delay_ms(200);  // Debounce delay 
} 
break; 
case 7:  // Expecting Button 4 
if (gpio_get_level(BUTTON4) == 0) {  // Button4 pressed 
// Blink LED for a longer time (1 second ON, 1 second OFF) 
gpio_set_level(LED_PIN, 1);  // Turn LED ON (GPIO2 high) 
delay_ms(1000);  // Delay for 1 second (LED stays on) 
gpio_set_level(LED_PIN, 0);  // Turn LED OFF (GPIO2 low) 
delay_ms(1000);  // Delay for 1 second (LED stays off) 
state = 0;  // Reset the sequence, go back to the initial state 
} 
break; 
default: 
} 
state = 0;  // In case of unexpected behavior, reset the sequence 
} 
}
