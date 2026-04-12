add_executable(coatheal_unit_tests
  unit/test_suite.cpp
)

target_link_libraries(coatheal_unit_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_unit_tests COMMAND coatheal_unit_tests)

add_executable(coatheal_safety_tests
  unit/safety_test.cpp
)

target_link_libraries(coatheal_safety_tests PRIVATE coatheal_onboard_core)

add_test(NAME coatheal_safety_tests COMMAND coatheal_safety_tests)
