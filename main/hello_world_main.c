#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "ds3231.h"
#include "ble_n.h"
// You have to set these CONFIG value using menuconfig.
#define SCL_GPIO		23
#define SDA_GPIO		22
#define	TIMEZONE		9


#define LEFT_PIN_OUT 33
#define RIGHT_PIN_OUT 32

#define LEFT_PIN_ENDPOINT      19
#define RIGHT_PIN_ENDPOINT     18
#define ENCODER_COUNTER        5
#define GPIO_INPUT_PIN_SEL  ((1ULL<<LEFT_PIN_ENDPOINT) | (1ULL<<RIGHT_PIN_ENDPOINT)| (1ULL<<ENCODER_COUNTER))
#define ESP_INTR_FLAG_DEFAULT 0

#define MOVE_LEFT 1
#define MOVE_RIGHT 2
#define NO_MOVE 0


uint8_t counterIn = 0;

static volatile uint32_t main_counter = 0; // 
static uint32_t target_counter = 0;
static uint32_t target_st = 0;
static uint8_t move_direction = 0; // 0 - stop 1- left 2 right 

static const char *TAG = "DS3213";


static volatile uint64_t filtrInt = 0;
static volatile uint64_t timeOut;

static void getGpio(void* arg);

uint8_t move = 0; // for subtask move
uint8_t inMove = 0; // for fix delay for encoder

struct tm rtcinfo;

i2c_dev_t dev;

nvs_handle_t my_handle;

uint32_t inRunProg = 0;

void setTarget(uint32_t data, uint32_t *target_counter, uint8_t *move_direction);



static xQueueHandle counter_queue = NULL;

void getClock(void *pvParameters)
{
    int32_t encoder_counter = 0;
    char timeBuff[20] = {0};

    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } 
    ESP_LOGI(pcTaskGetTaskName(0), "Set initial date time done");


    // Initialise the xLastWakeTime variable with the current time.
    // TickType_t xLastWakeTime = xTaskGetTickCount();

    // Get RTC date and time
    while (1) {
        float temp;
        // struct tm rtcinfo;

        if (ds3231_get_temp_float(&dev, &temp) != ESP_OK) {
            ESP_LOGE(pcTaskGetTaskName(0), "Could not get temperature.");
            while (1) { vTaskDelay(1); }
        }

        if (ds3231_get_time(&dev, &rtcinfo) != ESP_OK) {
            ESP_LOGE(pcTaskGetTaskName(0), "Could not get time.");
            while (1) { vTaskDelay(1); }
        }

        // ESP_LOGI(pcTaskGetTaskName(0), "%04d-%02d-%02d %02d:%02d:%02d, %.2f deg Cel", 
        //     rtcinfo.tm_year, rtcinfo.tm_mon + 1,
        //     rtcinfo.tm_mday, rtcinfo.tm_hour, rtcinfo.tm_min, rtcinfo.tm_sec, temp);
        
        // ESP_LOGI(pcTaskGetTaskName(0), "%" PRIu64 "\n", esp_timer_get_time());

        setCharData(rtcinfo, main_counter, target_counter, move_direction, esp_timer_get_time() - timeOut);

        vTaskDelay(1000 / portTICK_PERIOD_MS);

         // Read
        encoder_counter = 0;
        
        sprintf(timeBuff, "%02d%02d", rtcinfo.tm_hour, rtcinfo.tm_min); 
        err = nvs_get_i32(my_handle, timeBuff, &encoder_counter);
        switch (err) {
            case ESP_OK:
                // printf("RUN for %s = %d\n", timeBuff, encoder_counter);
                if (inRunProg != encoder_counter){
                    setTarget(encoder_counter, &target_counter, &move_direction);
                    inRunProg = encoder_counter;
                }
                
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                // printf("Not programm for %s = %d\n", timeBuff, encoder_counter);
                break;
            default :
                printf("Error (%s) reading!\n", esp_err_to_name(err));
        }

    }
}



// 04 05 1347
void makeData(){
    printf("Committing updates in NVS ... ");
    uint8_t min = 0;
    for (uint8_t i = 6 ; i < 24; i++){
        min = 0;
        printf("%02d%02d\n", i, min);
        min = 30;
        printf("%02d%02d\n", i, min);
    }
}



static xQueueHandle gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}


static void IRAM_ATTR encoder_isr_handler(void* arg)
{
    // esp_timer_get_time in us 1 sec = 1000 000 
    // if (esp_timer_get_time() - filtrInt  > 500000){
        filtrInt = esp_timer_get_time();
        timeOut =  filtrInt;
        
        move_direction = MOVE_RIGHT;
        if (move_direction == MOVE_LEFT && main_counter > 0){
            main_counter--;
        } else if (move_direction == MOVE_RIGHT){
            main_counter++;
        }
    // }
    
}

static void getGpio(void* arg){
    // esp_timer_get_time in us 1 sec = 1000 000 
    while(1){
        if (gpio_get_level(ENCODER_COUNTER) == 0){
            vTaskDelay(20 / portTICK_PERIOD_MS);
            if (gpio_get_level(ENCODER_COUNTER) == 0 && counterIn!=1){
                counterIn = 1;
                
                timeOut = esp_timer_get_time();
                    if (move_direction == MOVE_LEFT && main_counter > 0){
                        main_counter--;
                    } else if (move_direction == MOVE_RIGHT){
                        main_counter++;
                    }
                    printf("Encoder main_counter=%02d\n", main_counter);
            }else if (gpio_get_level(ENCODER_COUNTER) == 1){
                counterIn = 0;
            }
        }
        xQueueSend(counter_queue, ( void * ) &main_counter, NULL);
         vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
        }
    }
}

// static void gpio_move_task(void* arg)
// {

//     while(1){
        
//         printf("target_counter=%02d move_direction=%02d main_counter=%02d %" PRIu64 "\n", target_counter, move_direction, main_counter, esp_timer_get_time() - timeOut);
        
//         if (main_counter < target_counter && esp_timer_get_time() - timeOut < 10000000 ){

//             if (move_direction == MOVE_LEFT && gpio_get_level(LEFT_PIN_ENDPOINT) == 1){
                
//                 gpio_set_level(RIGHT_PIN_OUT, 0);
//                 // timeOut = esp_timer_get_time(); // reset
//                 vTaskDelay(100 / portTICK_PERIOD_MS);
//                 gpio_set_level(LEFT_PIN_OUT,  1);
        
//             } else if (move_direction == MOVE_RIGHT && gpio_get_level(RIGHT_PIN_ENDPOINT) == 1){
                  
//                 gpio_set_level(LEFT_PIN_OUT,  0);
//                 // timeOut = esp_timer_get_time(); // reset
//                 vTaskDelay(100 / portTICK_PERIOD_MS);
//                 gpio_set_level(RIGHT_PIN_OUT, 1);

//             } else {
//                 // timeOut = esp_timer_get_time(); // reset
//                 gpio_set_level(LEFT_PIN_OUT,  0);
//                 gpio_set_level(RIGHT_PIN_OUT, 0);
//                 move_direction = 0;
//             }

//         } else {

//             // timeOut = esp_timer_get_time(); // reset 
//             gpio_set_level(LEFT_PIN_OUT,  0);
//             gpio_set_level(RIGHT_PIN_OUT, 0);
//             move_direction = 0;
//         } 

//         vTaskDelay(100 / portTICK_PERIOD_MS);
//     }

// }
// ////////////////////////////

static void gpio_move_sub_task(void* arg){
uint8_t inRunDirB[3] = {0,0,0};
// uint8_t move = 0;
while(1){

        if (move == MOVE_LEFT){
            if(inRunDirB[MOVE_LEFT] == 0){
                inRunDirB[MOVE_LEFT] = 1;
                inRunDirB[MOVE_RIGHT] = 0;
                inRunDirB[NO_MOVE] = 0;
                gpio_set_level(RIGHT_PIN_OUT, 0);      /// run
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                gpio_set_level(LEFT_PIN_OUT,  0);      /// direction
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                gpio_set_level(RIGHT_PIN_OUT, 1);      /// run
                timeOut = esp_timer_get_time();
                inMove = 1;
            }
        }else if (move == MOVE_RIGHT){
            if(inRunDirB[MOVE_RIGHT] == 0){
                inRunDirB[MOVE_RIGHT] = 1;
                inRunDirB[MOVE_LEFT] = 0;
                inRunDirB[NO_MOVE] = 0;
                gpio_set_level(RIGHT_PIN_OUT, 0);      /// run

                vTaskDelay(3000 / portTICK_PERIOD_MS);
                gpio_set_level(LEFT_PIN_OUT,  1);      /// direction
                
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                gpio_set_level(RIGHT_PIN_OUT, 1);      /// run
                
                timeOut = esp_timer_get_time();
                inMove = 1;
            }
        }else{
                if(inRunDirB[NO_MOVE] == 0){
                inRunDirB[NO_MOVE] = 1;
                inRunDirB[MOVE_RIGHT] = 0;
                inRunDirB[MOVE_LEFT] = 0;
                
                gpio_set_level(RIGHT_PIN_OUT, 0);      /// run
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                gpio_set_level(LEFT_PIN_OUT,  0);      /// direction

                move_direction = NO_MOVE;
                inMove = 0;
            }
        }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}


static void gpio_move_task(void* arg)
{
    uint32_t gpioCounter = 0;
    while(1){
        
        // printf("target_counter=%02d move_direction=%02d main_counter=%02d %" PRIu64 "\n", target_counter, move_direction, main_counter, esp_timer_get_time() - timeOut);
        

        // portMAX_DELAY
        if(xQueueReceive(counter_queue, &gpioCounter, ( TickType_t ) 10 )) {
            printf("Counter[%d]\n", gpioCounter);
        }

        if (esp_timer_get_time() - timeOut < 30000000 || inMove == 0){
            if (move_direction == MOVE_LEFT && gpio_get_level(LEFT_PIN_ENDPOINT) == 1 && gpioCounter > target_counter){
                // upcount encoder to left
                move = MOVE_LEFT;
                // printf("MOVE_LEFT\n");

            
            }else if (move_direction == MOVE_RIGHT && gpio_get_level(RIGHT_PIN_ENDPOINT) == 1 && gpioCounter < target_counter){
                move = MOVE_RIGHT;
                // printf("MOVE_RIGHT\n");
            }else{
                // stop programm
                move = NO_MOVE;
            }


        }else{
            move = NO_MOVE;
        }

        uint8_t tmp;
        
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

}







void makeProgram(){
// Initialize NVS
    esp_err_t err;
    //erase
    // ESP_ERROR_CHECK(nvs_flash_erase());
    // err = nvs_flash_init();

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    // Open
    printf("\n");
    printf("Opening Non-Volatile Storage (NVS) handle... ");
    
    
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        printf("Done\n");

        // Write
        printf("Updating restart counter in NVS ... ");
        err = nvs_set_i32(my_handle, "0730", 2000002);
        printf((err != ESP_OK) ? "Failed!\n" : "Done\n");

        err = nvs_set_i32(my_handle, "0732", 2000004); 
        printf((err != ESP_OK) ? "Failed!\n" : "Done\n");

        err = nvs_set_i32(my_handle, "0734", 2000006);
        printf((err != ESP_OK) ? "Failed!\n" : "Done\n");

        err = nvs_set_i32(my_handle, "0736", 1000001); // return back
        printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
        
        err = nvs_commit(my_handle);
        printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
        // Close
        nvs_close(my_handle);
    }

}


void setTarget(uint32_t data, uint32_t *target_counter, uint8_t *move_direction){
    
    if (data > 1000000 && data < 2000000){
        if (*target_counter != data - 1000000 ){
            *move_direction = MOVE_LEFT;
            *target_counter = data - 1000000;
            timeOut = esp_timer_get_time(); // reset the counter
        }
        
    }else if (data > 2000000 ){
        

        if (*target_counter != data - 2000000 ){
            *move_direction = MOVE_RIGHT;
            *target_counter = data - 2000000;
            timeOut = esp_timer_get_time(); // reset the counter
        }

    } else {
        *move_direction = 0;
        *target_counter = 0;
    }
    
    printf(" Set task--- data=%02d target_counter=%02d move_direction=%02d\n", data, *target_counter, *move_direction);
}



void app_main()
{


    
    main_counter = 0;
    
    // setTarget(2000010, &target_counter, &move_direction);

    // set GPIO
    gpio_config_t io_conf;    
    //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    //create a queue to handle gpio event from isr
    // gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    // xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    xTaskCreate(gpio_move_task, "gpio_move_task", 2048, NULL, 10, NULL);

    xTaskCreate(gpio_move_sub_task, "gpio_move_sub_task", 2048, NULL, 9, NULL);

    // xTaskCreate(getGpio, "getGpio", 2048, NULL, 8, NULL);
    

    
   
   
    // xTaskCreatePinnedToCore(gpio_move_task, "gpio_move_task", 2048, NULL, 10, NULL, 1);

    // xTaskCreatePinnedToCore(gpio_move_sub_task, "gpio_move_sub_task", 2048, NULL, 10, NULL, 1);

    // xTaskCreatePinnedToCore(getGpio, "getGpio", 2048, NULL, 10, NULL, 1);
    

    // // //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    // //hook isr handler for specific gpio pin
    gpio_isr_handler_add(ENCODER_COUNTER, encoder_isr_handler, (void*) ENCODER_COUNTER);


    // // //hook isr handler for specific gpio pin
    // gpio_isr_handler_add(LEFT_PIN_ENDPOINT, gpio_isr_handler, (void*) LEFT_PIN_ENDPOINT);
    // gpio_isr_handler_add(RIGHT_PIN_ENDPOINT, gpio_isr_handler, (void*) RIGHT_PIN_ENDPOINT);


    // init controll gpio
    gpio_reset_pin(LEFT_PIN_OUT);
    gpio_reset_pin(RIGHT_PIN_OUT);

    gpio_set_direction(LEFT_PIN_OUT, GPIO_MODE_OUTPUT);
    gpio_set_direction(RIGHT_PIN_OUT, GPIO_MODE_OUTPUT);


    // set RTC
    if (ds3231_init_desc(&dev, I2C_NUM_0, SDA_GPIO, SCL_GPIO) != ESP_OK) {
        ESP_LOGE(pcTaskGetTaskName(0), "Could not init device descriptor.");
        while (1) { vTaskDelay(1); }
    } 
    // set RTC





bleInit();

// makeData();
makeProgram();

    // Get clock
xTaskCreatePinnedToCore(getClock, "getClock", 1024*4, NULL, 10, NULL,1);

}

