idf_component_register(
  REQUIRES esp_driver_spi
           esp_driver_gpio
           nvs_flash
  SRCS "ssd1619.c"
       "ssd1619_lut.c"
  INCLUDE_DIRS include
)
