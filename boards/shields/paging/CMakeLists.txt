target_sources(app PRIVATE
    #src/charging_monitor.c
    #src/charging_backlight_controller.c
    #drivers/charging_status/charging_status.c
    drivers/bluetooth_status/bluetooth_status.c
    drivers/layer_indicator/layer_indicator.c
)

#zephyr_include_directories(dts/bindings)

#zephyr_library()
#add_subdirectory(drivers/bluetooth_led)
