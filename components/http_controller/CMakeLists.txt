idf_component_register(SRCS "http_controller.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server
                                json
                       PRIV_REQUIRES freertos
                                     util
                                     settings
                                     schedule)

