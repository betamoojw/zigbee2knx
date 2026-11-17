idf_component_register(
    SRCS "main.c" "zb.c" "knx_link.c" "bvm.c" "kgt.c" "pair.c" "devsrv.c" "vdev.c" "console.c" "zlink.c" "gw.c"
    INCLUDE_DIRS "."
    REQUIRES berry console nvs_flash esp_timer esp_driver_uart esp_driver_gpio esp_app_format mbedtls esp_rom
    # the host API of the templates (berry/) and the two light classes built into the firmware (templates/)
    EMBED_TXTFILES
        "../../berry/prelude.be"
        "../../templates/zigbee-dimmable-light/zigbee-dimmable-light.be"
        "../../templates/zigbee-white-spectrum-light/zigbee-white-spectrum-light.be"
)
