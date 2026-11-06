cmake_minimum_required(VERSION 3.16)
project(efvi_mock VERSION 0.2.0 LANGUAGES C)

include(CTest)
option(EFVI_MOCK_SHARED "Build shared library instead of static" OFF)
option(EFVI_MOCK_SANITIZERS "Enable AddressSanitizer/UBSan with GCC/Clang" OFF)

if(EFVI_MOCK_SHARED)
  set(EFVI_MOCK_LIBRARY_TYPE SHARED)
else()
  set(EFVI_MOCK_LIBRARY_TYPE STATIC)
endif()

add_library(efvi_mock ${EFVI_MOCK_LIBRARY_TYPE}
  src/mock_driver.c
  src/mock_vi.c
  src/mock_eventq.c
  src/mock_packet.c
)
add_library(efvi::mock ALIAS efvi_mock)
target_compile_features(efvi_mock PUBLIC c_std_11)
target_include_directories(efvi_mock PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
find_package(Threads REQUIRED)
target_link_libraries(efvi_mock PUBLIC Threads::Threads)
set_target_properties(efvi_mock PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
  OUTPUT_NAME efvi_mock
  POSITION_INDEPENDENT_CODE ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(efvi_mock PRIVATE -Wall -Wextra -Werror -Wpedantic)
  if(EFVI_MOCK_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

add_executable(efvi_udp_monitor examples/udp_monitor.c)
target_link_libraries(efvi_udp_monitor PRIVATE efvi_mock)

if(BUILD_TESTING)
  add_executable(header_smoke tests/header_smoke.c)
  target_link_libraries(header_smoke PRIVATE efvi_mock)
  add_test(NAME header_smoke COMMAND header_smoke)
  add_executable(udp_integration tests/udp_integration.c)
  target_link_libraries(udp_integration PRIVATE efvi_mock)
  add_test(NAME udp_integration COMMAND udp_integration)
  set_tests_properties(udp_integration PROPERTIES SKIP_RETURN_CODE 77)
  set_tests_properties(udp_integration PROPERTIES TIMEOUT 15)
  add_executable(packet_unit tests/packet_unit.c src/mock_packet.c)
  target_include_directories(packet_unit PRIVATE src)
  add_test(NAME packet_unit COMMAND packet_unit)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME pcap_replay_parser
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcap_replay.py)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS efvi_mock EXPORT efvi_mock_targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/etherfabric/vi.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/etherfabric)
install(EXPORT efvi_mock_targets NAMESPACE efvi::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/efvi_mock)
