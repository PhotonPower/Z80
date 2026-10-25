# The orchestrator tests need Python 3 (standard library only) and the z80tool binary.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME agent
           COMMAND ${CMAKE_COMMAND} -E env "Z80TOOL=$<TARGET_FILE:z80tool>" ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/agent)
endif()

foreach(name expression assembler decoder roundtrip disassembler image_io robustness cpu session monitor cpc listing reverse)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE z80core)
  z80_set_warnings(test_${name})
  add_test(NAME ${name} COMMAND test_${name})
endforeach()
