#
# Copyright (c) 2020 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

# Local secrets (access token) live in a git-ignored file at the repository root, shared by the
# apps; see secrets.conf.example.
set(PIGGYSAT_SECRETS ${CMAKE_CURRENT_LIST_DIR}/../secrets.conf)
if(EXISTS ${PIGGYSAT_SECRETS})
  list(APPEND EXTRA_CONF_FILE ${PIGGYSAT_SECRETS})
else()
  message(WARNING "secrets.conf not found: CONFIG_APP_THINGSBOARD_ACCESS_TOKEN is empty "
                  "(copy secrets.conf.example to secrets.conf)")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(piggysat-test)

# NORDIC SDK APP START
target_sources(app PRIVATE
	src/main.c
	src/antenna.c
	src/leds.c
	src/magnet.c
	src/gnss.c
	src/rftest.c
	src/simtest.c
	src/shell.c
	src/modem.c
	src/transport.c
)
# NORDIC SDK APP END

zephyr_include_directories(src)

# Test tool: keep the debug port permanently open so the debugger and RTT always work, even
# after a full chip erase. The "unprotected" value is merged into UICR.APPROTECT and
# UICR.SECUREAPPROTECT of the flashed image (tfm_merged.hex); with the default
# NRF_APPROTECT_USE_UICR, the firmware then opens both access ports at boot.
# Remove this for production firmware.
if(CONFIG_TFM_FLASH_MERGED_BINARY AND NOT CONFIG_PARTITION_MANAGER_ENABLED)
  set(TFM_MERGED_HEX ${CMAKE_BINARY_DIR}/zephyr/tfm_merged.hex)
  add_custom_target(uicr_debug_open ALL
    COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/mergehex.py --overlap=replace
            -o ${TFM_MERGED_HEX} ${TFM_MERGED_HEX}
            ${BOARD_DIR}/uicr_debug_open.hex
    COMMENT "Adding UICR debug-open values to tfm_merged.hex"
  )
  add_dependencies(uicr_debug_open zephyr_final)
endif()
