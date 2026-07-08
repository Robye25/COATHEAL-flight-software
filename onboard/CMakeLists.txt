find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(LIBGPIOD QUIET libgpiod)
endif()

add_library(coatheal_onboard_core
  src/command_parser.cpp
  src/command_server.cpp
  src/config.cpp
  src/fatigue_sequencer.cpp
  src/heater_scheduler.cpp
  src/motion_lock.cpp
  src/pid_controller.cpp
  src/sd_notify.cpp
  src/sensor_manager.cpp
  src/state_manager.cpp
  src/status_flags.cpp
  src/stepper_controller.cpp
  src/stepper_channel.cpp
  src/tmc5160_driver.cpp
  src/storage_manager.cpp
  src/system_controller.cpp
  src/telemetry.cpp
  src/telemetry_queue.cpp
  src/telemetry_client.cpp
  src/thermal_controller.cpp
  src/hal/i2c_adapter.cpp
  src/hal/ina3221_adapter.cpp
  src/hal/pwm_controller.cpp
  src/hal/rtc_adapter.cpp
  src/hal/spi_adapter.cpp
  src/hal/gpio_output.cpp
  src/hal/gpio_status_led.cpp
  src/hal/simulated_status_led.cpp
  src/hal/simulated_stepper_driver.cpp
  src/hal/gpio_step_dir_driver.cpp
)

target_include_directories(coatheal_onboard_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(COATHEAL_STRICT)
  target_compile_options(coatheal_onboard_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(coatheal_onboard_core PUBLIC Threads::Threads)

if(LIBGPIOD_FOUND)
  target_compile_definitions(coatheal_onboard_core PUBLIC COATHEAL_HAS_LIBGPIOD=1)
  if(LIBGPIOD_VERSION VERSION_GREATER_EQUAL "2.0")
    target_compile_definitions(coatheal_onboard_core PUBLIC COATHEAL_LIBGPIOD_V2=1)
  endif()
  target_include_directories(coatheal_onboard_core PUBLIC ${LIBGPIOD_INCLUDE_DIRS})
  target_link_libraries(coatheal_onboard_core PUBLIC ${LIBGPIOD_LIBRARIES})
endif()

add_executable(coatheal_onboard src/main.cpp)
target_link_libraries(coatheal_onboard PRIVATE coatheal_onboard_core)
