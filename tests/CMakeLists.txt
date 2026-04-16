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

add_executable(coatheal_stepper_rev_b_tests
  unit/test_stepper_rev_b.cpp
)

target_link_libraries(coatheal_stepper_rev_b_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_stepper_rev_b_tests COMMAND coatheal_stepper_rev_b_tests)
