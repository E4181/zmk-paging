target_sources(app PRIVATE
    #src/charging_monitor.c
    #src/charging_backlight_controller.c
    #drivers/charging_status/charging_status.c
    #drivers/ble_check/ble_check.c
)

#zephyr_include_directories(dts/bindings)

zephyr_library()

zephyr_library_include_directories(
    ${ZMK_DIR}/app/include
)

zephyr_library_sources(
    #drivers/charging_status/charging_status.c
    drivers/bluetooth_led/bluetooth_led.c
)
