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