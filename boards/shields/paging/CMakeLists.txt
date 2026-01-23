target_sources(app PRIVATE
    #src/charging_monitor.c
    #src/charging_backlight_controller.c
    drivers/charging_status/charging_status.c
    drivers/ble_check/ble_check.c
)