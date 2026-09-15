# Local OLED interface; settings_reset and other boards do not use it.
if(CONFIG_ZMK_DISPLAY AND CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM AND
   (CONFIG_BOARD_EYELASH_SOFLE_LEFT OR CONFIG_BOARD_EYELASH_SOFLE_RIGHT))
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/sofle_display.c)
endif()
