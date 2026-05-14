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
#include "doom_espnow.h"
#include "doom_espnow_server.h"


extern void jsInit();

// Global to store multiplayer role
static int g_player_num = 0;
static bool g_multiplayer = false;


void doomEngineTask(void *pvParameters)
{
    printf("Doom engine task started...\n");
    printf("Player num: %d, Multiplayer: %s\n", g_player_num, g_multiplayer ? "yes" : "no");

    // Initialize Doom server
    doom_server_config_t config = {
        .skill = 3,
        .episode = 1,
        .level = 1,
        .deathmatch = 0,
        .num_players = g_multiplayer ? 2 : 1
    };
    doom_server_init(&config);

    // Build command line args
    // For true multiplayer, we'd use -net <address>, but for ESP-NOW we use -solo-net
    // which enables netgame features without requiring a separate server
    char const *argv[]={"doom","-cout","ICWEFDA","-skill","3","-warp","1","-solo-net", NULL};
    int argc = 8;

    printf("Calling doom_main with args: ");
    for (int i = 0; i < argc-1; i++) printf("%s ", argv[i]);
    printf("\n");

    doom_main(argc, argv);

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

	// Initialize AtomS3R joystick system
	printf("Initializing AtomS3R joystick system...\n");
	jsInit();
	printf("Joystick initialization completed\n");

	// ESP-NOW Auto-Discovery for Multiplayer
	printf("\n=== ESP-NOW Multiplayer Setup ===\n");
	printf("Turn on BOTH devices within 30 seconds\n");
	printf("First device = HOST (Player 1)\n");
	printf("Second device = CLIENT (Player 2)\n");

	int role = doom_espnow_setup();

	if (role < 0) {
		printf("ESP-NOW setup failed! Starting single player...\n");
		g_player_num = 0;
		g_multiplayer = false;
	} else {
		printf("\n=== Setup Complete! ===\n");
		printf("Role: %s\n", role == 0 ? "HOST (Player 1)" : "CLIENT (Player 2)");
		g_player_num = role;
		g_multiplayer = doom_espnow_is_multiplayer();

		if (g_multiplayer) {
			printf("\n*** MULTIPLAYER MODE: 2 PLAYERS ***\n");
		} else {
			printf("\n*** SINGLE PLAYER MODE ***\n");
		}
	}

	// Disable task watchdog temporarily for AtomS3R compatibility
	printf("Disabling task watchdog...\n");
	esp_task_wdt_deinit();
	printf("Task watchdog disabled\n");

	printf("Starting Doom engine...\n");

	// Increase task stack size for AtomS3R compatibility
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
	}
}
