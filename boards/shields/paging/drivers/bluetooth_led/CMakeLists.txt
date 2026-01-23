zephyr_library()

zephyr_library_include_directories(
    ${ZMK_DIR}/app/include
)

zephyr_library_sources(
    bluetooth_led.c
)
