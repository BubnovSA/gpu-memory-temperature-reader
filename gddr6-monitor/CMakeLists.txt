# ./gddr6-monitor/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)

project(gddr6_monitor)
set(APP_SOURCES
    src/app.c
    src/nvml_probe.c
    src/logger.c
)
add_executable(gddr6 ${APP_SOURCES})

target_link_libraries(gddr6 PRIVATE gddr6_lib)
target_link_libraries(gddr6 PRIVATE pci)
target_link_libraries(gddr6 PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(gddr6 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
