find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(LIBGPIOD QUIET libgpiod)
endif()

# A flight build must not silently lose GPIO: without libgpiod the heater and
# motor HALs compile to stubs and the binary "runs" with no outputs at all.
# deploy_onboard.sh configures with -DCOATHEAL_REQUIRE_LIBGPIOD=ON; desktop
# builds and the unit tests keep the optional behaviour.
option(COATHEAL_REQUIRE_LIBGPIOD "Fail to configure when libgpiod is not found" OFF)
if(COATHEAL_REQUIRE_LIBGPIOD AND NOT LIBGPIOD_FOUND)
  message(FATAL_ERROR
    "libgpiod not found (pkg-config libgpiod). Install libgpiod-dev and pkg-config, "
    "or configure with -DCOATHEAL_REQUIRE_LIBGPIOD=OFF for a no-hardware build.")
endif()
if(LIBGPIOD_FOUND)
  message(STATUS "libgpiod ${LIBGPIOD_VERSION}: GPIO outputs enabled")
else()
  message(WARNING "libgpiod not found: building WITHOUT GPIO (heaters/motors are stubs)")
endif()

add_library(coatheal_onboard_core
  src/command_parser.cpp
  src/command_server.cpp
  src/config.cpp
  src/fallback_planner.cpp
  src/heater_scheduler.cpp
  src/motion_lock.cpp
  src/pid_autotuner.cpp
  src/pid_controller.cpp
  src/sd_notify.cpp
  src/sensor_manager.cpp
  src/state_manager.cpp
  src/status_flags.cpp
  src/stepper_controller.cpp
  src/stepper_channel.cpp
  src/storage_manager.cpp
  src/system_controller.cpp
  src/telemetry.cpp
  src/telemetry_queue.cpp
  src/telemetry_client.cpp
  src/thermal_controller.cpp
  src/tmc5160_driver.cpp
  src/hal/i2c_adapter.cpp
  src/hal/i2c_bus.cpp
  src/hal/max31865_adapter.cpp
  src/hal/sequent_rtd_adapter.cpp
  src/hal/ina3221_adapter.cpp
  src/hal/pwm_controller.cpp
  src/hal/rtc_adapter.cpp
  src/hal/spi_adapter.cpp
  src/hal/spi_bus.cpp
  src/hal/spi_bus_lock.cpp
  src/hal/gpio_output.cpp
  src/hal/gpio_status_led.cpp
  src/hal/simulated_status_led.cpp
  src/hal/simulated_stepper_driver.cpp
)

target_include_directories(coatheal_onboard_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(COATHEAL_STRICT)
  target_compile_options(coatheal_onboard_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(coatheal_onboard_core PUBLIC Threads::Threads)

# The _WIN32 branches of command_server.cpp and telemetry_client.cpp call
# Winsock; without this the Windows build links only on Linux-only paths.
if(WIN32)
  target_link_libraries(coatheal_onboard_core PUBLIC ws2_32)
endif()

if(LIBGPIOD_FOUND)
  target_compile_definitions(coatheal_onboard_core PUBLIC COATHEAL_HAS_LIBGPIOD=1)
  if(LIBGPIOD_VERSION VERSION_GREATER_EQUAL "2.0")
    target_compile_definitions(coatheal_onboard_core PUBLIC COATHEAL_LIBGPIOD_V2=1)
  endif()
  # Pull-bias request flags arrived in libgpiod 1.5 (v2 has them in the
  # line-settings API). Older v1 builds still get the outputs, just without
  # the pull -- see GpioBias in hal/gpio_output.hpp.
  if(LIBGPIOD_VERSION VERSION_GREATER_EQUAL "1.5")
    target_compile_definitions(coatheal_onboard_core PUBLIC COATHEAL_LIBGPIOD_HAS_BIAS=1)
  endif()
  target_include_directories(coatheal_onboard_core PUBLIC ${LIBGPIOD_INCLUDE_DIRS})
  target_link_libraries(coatheal_onboard_core PUBLIC ${LIBGPIOD_LIBRARIES})
endif()

add_executable(coatheal_onboard src/main.cpp)
target_link_libraries(coatheal_onboard PRIVATE coatheal_onboard_core)
