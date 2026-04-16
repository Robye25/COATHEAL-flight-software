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

# Rev B phase/thermal tests. Built as a standalone executable that links
# only the files it needs, so it compiles even while system_controller.cpp
# still references the old API (the orchestrator wires that in at merge).
add_executable(coatheal_phase_rev_b_tests
  unit/test_phase_rev_b.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/state_manager.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/thermal_controller.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/pid_controller.cpp
  ${CMAKE_SOURCE_DIR}/onboard/src/config.cpp
)

target_include_directories(coatheal_phase_rev_b_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/onboard/include
)

add_test(NAME coatheal_phase_rev_b_tests COMMAND coatheal_phase_rev_b_tests)

# Rev B safety interlock tests (Agent D).
add_executable(coatheal_safety_rev_b_tests
  unit/test_safety_rev_b.cpp
)

target_link_libraries(coatheal_safety_rev_b_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_safety_rev_b_tests COMMAND coatheal_safety_rev_b_tests)
