cmake_minimum_required(VERSION 3.10)

idf_component_register(
SRCS
  "atom_input.c"
  "doom_espnow.c"
  "doom_espnow_server.c"
  "espnow_network.c"
  "gamepad.c"
  "i_main.c"
  "i_network.c"
  "i_sound.c"
  "i_system.c"
  "i_video.c"
  "spi_lcd.c"
  "sndhw.c"
  "i2c_joystick.c"

INCLUDE_DIRS
  "include" "."

PRIV_REQUIRES
  prboom
  heap
  spi_flash
  xtensa
  esp_partition
  driver
  esp_lcd
  m5_atom_s3
  esp_driver_i2c
  esp_driver_ledc
  esp_wifi
  esp_netif
  esp_event
  nvs_flash
)

#Need this otherwise linking fails because of some sort of link optimization for sndhw...
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u sndhw_init")

#Need this otherwise linking fails because of some sort of link optimization for sndhw...
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u sndhw_init")
