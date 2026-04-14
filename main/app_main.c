// Copyright 2016-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "i_system.h"

#include "spi_lcd.h"


extern void jsInit();


void doomEngineTask(void *pvParameters)
{
    printf("Doom engine task started...\n");

    // Test if task is running properly before calling doom_main
    for (int i = 0; i < 5; i++) {
        printf("Doom task test loop %d...\n", i);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    printf("About to start doom_main...\n");

    char const *argv[]={"doom","-cout","ICWEFDA", NULL};
    printf("Calling doom_main with args: %s %s %s\n", argv[0], argv[1], argv[2]);
    doom_main(3, argv);

    printf("Doom engine exited!\n");
}

void app_main()
{
	printf("app_main() started\n");

	// Debug: Check for WAD partitions
	printf("Looking for WAD partitions...\n");

	const esp_partition_t* part;
	part=esp_partition_find_first(66, 6, NULL);
	if (part==0) printf("Couldn't find wad part (type 66, subtype 6)!\n");
	else printf("Found DOOM1.WAD partition\n");

	part=esp_partition_find_first(66, 7, NULL);
	if (part==0) printf("Couldn't find prboom wad part (type 66, subtype 7)!\n");
	else printf("Found prboom.wad partition\n");

	// Try alternative: find by label name
	printf("Trying to find by label name...\n");
	part = esp_partition_find_first(0x40, 0, "wad");
	if (part) printf("Found 'wad' partition by label\n");

	part = esp_partition_find_first(0x40, 0, "prwad");
	if (part) printf("Found 'prwad' partition by label\n");

	printf("About to initialize LCD...\n");
	spi_lcd_init();
	printf("LCD initialized\n");

	// TEMPORARILY SKIP JOYSTICK INITIALIZATION FOR ATOMS3R COMPATIBILITY TESTING
	// The AtomS3R may have different I2C hardware or pin mappings
	printf("SKIPPING joystick initialization for AtomS3R compatibility testing\n");
	// jsInit();
	printf("Joystick initialization skipped\n");

	// Disable task watchdog temporarily for AtomS3R compatibility
	// The Doom engine initialization can take longer than the watchdog timeout
	printf("Disabling task watchdog...\n");
	esp_task_wdt_deinit();
	printf("Task watchdog disabled\n");

	printf("Starting Doom engine with increased stack and watchdog disabled...\n");

	// Increase task stack size for AtomS3R compatibility
	// The PSRAM operations might require more stack space
	printf("Creating Doom engine task...\n");
	TaskHandle_t doom_task_handle = NULL;
	BaseType_t result = xTaskCreatePinnedToCore(&doomEngineTask, "doomEngine", 32768, NULL, 5, &doom_task_handle, 0);

	if (result == pdPASS) {
		printf("Doom engine task created successfully\n");
	} else {
		printf("Failed to create Doom engine task!\n");
	}

	printf("app_main() completed, Doom task should be running\n");

	// Keep app_main alive to monitor the doom task
	while(1) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		printf("Doom task is running...\n");
	}
}
