add_executable(coatheal_unit_tests
  unit/test_suite.cpp
)

target_link_libraries(coatheal_unit_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_unit_tests COMMAND coatheal_unit_tests)

add_executable(coatheal_status_led_test
  unit/status_led_test.cpp
)

target_link_libraries(coatheal_status_led_test PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_status_led_test COMMAND coatheal_status_led_test)