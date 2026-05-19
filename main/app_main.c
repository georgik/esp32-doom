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
#include "esp_log.h"
#include "i_system.h"

#include "spi_lcd.h"
#include "doom_espnow.h"
#include "doom_espnow_server.h"

static const char *TAG = "app_main";

extern void jsInit();

// Global to store multiplayer role
static int g_player_num = 0;
static bool g_multiplayer = false;


// Network tick task - handles ESP-NOW packets during game
void networkTickTask(void *pvParameters) {
    ESP_LOGI("doom_net", "Network tick task started");

    while (1) {
        doom_server_tick();
        vTaskDelay(pdMS_TO_TICKS(10));  // 100Hz network tick
    }
}


void doomEngineTask(void *pvParameters)
{
    printf("Doom engine task started...\n");
    printf("Player num: %d, Multiplayer: %s, is_host: %d\n",
           g_player_num, g_multiplayer ? "yes" : "no", doom_espnow_is_host());

    // If client, send init packet to server
    if (!doom_espnow_is_host() && g_multiplayer) {
        printf("*** CLIENT MODE: Sending PKT_INIT to server ***\n");
        doom_client_send_init();
        // Don't block - networkTickTask will receive SETUP+GO packets
    } else {
        printf("*** %s MODE: Not sending PKT_INIT ***\n",
               doom_espnow_is_host() ? "SERVER" : "SINGLE PLAYER");
    }

    // Build command line args
    // Use -solo-net for multiplayer mode (enables netgame without separate server)
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

	// Initialize Doom server state before starting tasks
	if (g_multiplayer) {
		doom_server_config_t config = {
			.skill = 3,
			.episode = 1,
			.level = 1,
			.deathmatch = 0,
			.num_players = 2
		};
		printf("Initializing Doom server state (is_host=%d)...\n", doom_espnow_is_host());
		doom_server_init(&config);
		printf("Doom server init complete\n");
	}

	// Disable task watchdog temporarily for AtomS3R compatibility
	printf("Disabling task watchdog...\n");
	esp_task_wdt_deinit();
	printf("Task watchdog disabled\n");

	// Start network tick task for multiplayer
	if (g_multiplayer) {
		ESP_LOGI(TAG, "Starting network tick task...");
		TaskHandle_t net_task_handle = NULL;
		BaseType_t ret = xTaskCreatePinnedToCore(&networkTickTask, "netTick", 2048, NULL, 6, &net_task_handle, 0);
		ESP_LOGI(TAG, "Network tick task: %s", ret == pdPASS ? "OK" : "FAIL");
	}

	ESP_LOGI(TAG, "Starting Doom engine...");

	// Increase task stack size for AtomS3R compatibility
	ESP_LOGI(TAG, "Creating Doom engine task...");
	TaskHandle_t doom_task_handle = NULL;
	BaseType_t result = xTaskCreatePinnedToCore(&doomEngineTask, "doomEngine", 28672, NULL, 2, &doom_task_handle, 1);  // Reduced from 32K

	if (result == pdPASS) {
		ESP_LOGI(TAG, "Doom engine task created successfully");
	} else {
		ESP_LOGE(TAG, "Failed to create Doom engine task!");
	}

	ESP_LOGI(TAG, "app_main() completed, Doom task should be running");

	// Keep app_main alive to monitor the doom task
	while(1) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
