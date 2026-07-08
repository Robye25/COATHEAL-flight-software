add_executable(coatheal_unit_tests
  unit/test_suite.cpp
)

target_link_libraries(coatheal_unit_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_unit_tests COMMAND coatheal_unit_tests)

add_executable(coatheal_state_machine_tests
  unit/state_machine_test.cpp
)

target_link_libraries(coatheal_state_machine_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_state_machine_tests COMMAND coatheal_state_machine_tests)

add_executable(coatheal_safety_tests
  unit/safety_test.cpp
)

target_link_libraries(coatheal_safety_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_safety_tests COMMAND coatheal_safety_tests)

add_executable(coatheal_downlink_bandwidth_test
  downlink_bandwidth_test.cpp
)

target_link_libraries(coatheal_downlink_bandwidth_test PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_downlink_bandwidth COMMAND coatheal_downlink_bandwidth_test)

add_executable(coatheal_status_led_test
  unit/status_led_test.cpp
)

target_link_libraries(coatheal_status_led_test PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_status_led_test COMMAND coatheal_status_led_test)

add_executable(coatheal_stepper_tests
  unit/stepper_test.cpp
)

target_link_libraries(coatheal_stepper_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_stepper_tests COMMAND coatheal_stepper_tests)

# Rev C phase/thermal tests. Built as a standalone executable that links
# only the files it needs.
add_executable(coatheal_phase_rev_c_tests
  unit/test_phase_rev_c.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/state_manager.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/thermal_controller.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/pid_controller.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/config.cpp
)

target_include_directories(coatheal_phase_rev_c_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/onboard/include
)

add_test(NAME coatheal_phase_rev_c_tests COMMAND coatheal_phase_rev_c_tests)

# Rev C safety interlock tests.
add_executable(coatheal_safety_rev_c_tests
  unit/test_safety_rev_c.cpp
)

target_link_libraries(coatheal_safety_rev_c_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_safety_rev_c_tests COMMAND coatheal_safety_rev_c_tests)

# Rev C dual-stepper tests.
add_executable(coatheal_stepper_rev_c_tests
  unit/test_stepper_rev_c.cpp
)

target_link_libraries(coatheal_stepper_rev_c_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_stepper_rev_c_tests COMMAND coatheal_stepper_rev_c_tests)

# Rev C telemetry serializer tests.
add_executable(coatheal_telemetry_rev_c_tests
  unit/test_telemetry_rev_c.cpp
)

target_link_libraries(coatheal_telemetry_rev_c_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_telemetry_rev_c_tests COMMAND coatheal_telemetry_rev_c_tests)

# Agent B — pulse-thread jitter bench. Not registered as a ctest (it takes
# several seconds and is a timing measurement, not a correctness check).
add_executable(coatheal_pulse_jitter_bench
  pulse_jitter_bench.cpp
)

target_link_libraries(coatheal_pulse_jitter_bench PRIVATE coatheal_onboard_core)

# Agent B — telemetry enqueue throughput bench. Measures Enqueue() latency
# at 50 Hz for 60 s so we know if fsync-per-frame dominates the hot path.
add_executable(coatheal_enqueue_throughput_bench
  enqueue_throughput_bench.cpp
)

target_link_libraries(coatheal_enqueue_throughput_bench PRIVATE coatheal_onboard_core)

# Rev C sensor-manager regression: samples 6 and 7 must seed to zero so the
# telemetry layer emits "-" on the wire.
add_executable(coatheal_sensor_manager_rev_c_tests
  unit/test_sensor_manager_rev_c.cpp
)

target_link_libraries(coatheal_sensor_manager_rev_c_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_sensor_manager_rev_c_tests
  COMMAND coatheal_sensor_manager_rev_c_tests)
