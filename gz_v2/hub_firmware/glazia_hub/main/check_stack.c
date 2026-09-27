#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void app_main() {
    printf("StackType_t size: %zu\n", sizeof(StackType_t));
}
