idf_component_register(SRCS "main.c" "ethernet_service.c" "ethernet_setup.c" "mqtt_service.c" "app_mqtt.c" "ds18b20_temp.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer nvs_flash
							esp_eth
							esp_netif
							driver
							mqtt	
				PRIV_REQUIRES espressif__onewire_bus			)
