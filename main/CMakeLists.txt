idf_component_register(SRCS "main.cpp" "uhd_detect.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES m5unified usb usb_host_uvc esp_driver_jpeg esp_driver_ppa esp-dl
                       EMBED_FILES "models/uhd_w32.espdl")
