#
# Copyright (c) 2020 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

# Local secrets (access token) live in a git-ignored file, see secrets.conf.example.
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/secrets.conf)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/secrets.conf)
else()
  message(WARNING "secrets.conf not found: CONFIG_APP_THINGSBOARD_ACCESS_TOKEN is empty "
                  "(copy secrets.conf.example to secrets.conf)")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(piggy-sat)

# NORDIC SDK APP START
target_sources(app PRIVATE
	src/main.c
	src/app_antenna.c
	src/app_leds.c
	src/app_gnss.c
	src/app_rftest.c
	src/app_modem.c
	src/app_transport.c
)
# NORDIC SDK APP END

zephyr_include_directories(src)
