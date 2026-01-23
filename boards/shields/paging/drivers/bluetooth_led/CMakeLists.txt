zephyr_library()

# 关键：把这个 driver 挂到 ZMK app 上
zephyr_library_link_libraries(zmk)

zephyr_library_sources(
    bluetooth_led.c
)
