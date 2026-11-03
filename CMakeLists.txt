# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_DYNAMIC_MACRO)
  zephyr_include_directories(include)
  target_sources(app PRIVATE src/macro.c src/manager.c)
  if(CONFIG_ZMK_DYNAMIC_SETTINGS_SCHEMA)
    set_property(GLOBAL APPEND PROPERTY DYNAMIC_SETTINGS_SCHEMA_CONTRIBUTORS
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/settings_schema.py")
  endif()
endif()
