cmake_minimum_required(VERSION 3.10)

idf_component_register(
SRCS
  "GAMMATBL.c"
  "SINETABL.c"
  "TANGTABL.c"
  "TANTOANG.c"

INCLUDE_DIRS
  "include" "."
)
