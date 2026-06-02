cmake_minimum_required(VERSION 3.20)
project(LLMBotFPS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Vendored dependencies (third_party/)
# ---------------------------------------------------------------------------
set(RAYLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/raylib")
set(VENDOR_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/include")

add_library(raylib STATIC IMPORTED)
set_target_properties(raylib PROPERTIES
    IMPORTED_LOCATION "${RAYLIB_DIR}/lib/libraylib.a"
    INTERFACE_INCLUDE_DIRECTORIES "${RAYLIB_DIR}/include")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
file(GLOB SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${VENDOR_INCLUDE}")

target_link_libraries(${PROJECT_NAME} PRIVATE raylib)

# cpp-httplib needs winsock; std::thread/futures need winpthreads on MinGW.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        opengl32 gdi32 winmm ws2_32 ole32)
    target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++)
endif()

find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Headless logic test (no rendering / no LLM): exercises the adaptation loop.
# ---------------------------------------------------------------------------
add_executable(sim_test
    "${CMAKE_SOURCE_DIR}/tools/sim_test.cpp"
    "${CMAKE_SOURCE_DIR}/src/Map.cpp"
    "${CMAKE_SOURCE_DIR}/src/Analyst.cpp"
    "${CMAKE_SOURCE_DIR}/src/StrategyTuner.cpp"
    "${CMAKE_SOURCE_DIR}/src/BotController.cpp"
    "${CMAKE_SOURCE_DIR}/src/Rollout.cpp")
target_include_directories(sim_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src" "${VENDOR_INCLUDE}" "${RAYLIB_DIR}/include")
if(WIN32)
    target_link_options(sim_test PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Copy runtime assets next to the executable.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config.json"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/fetch_llm.ps1"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/fetch_llm.ps1")
