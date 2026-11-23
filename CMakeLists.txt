cmake_minimum_required(VERSION 3.16)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MODELEFORGE_VERSION_TEXT)
string(STRIP "${MODELEFORGE_VERSION_TEXT}" MODELEFORGE_VERSION)
project(ModelForge VERSION ${MODELEFORGE_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)

add_library(sqlite3 STATIC backend/third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC backend/third_party)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
set_target_properties(sqlite3 PROPERTIES C_STANDARD 99)

add_executable(modelforge
  backend/src/main.cpp
  backend/src/util.cpp
  backend/src/crypto.cpp
  backend/src/types.cpp
  backend/src/database.cpp
  backend/src/logger.cpp
  backend/src/settings.cpp
  backend/src/event_hub.cpp
  backend/src/ws_server.cpp
  backend/src/process_runner.cpp
  backend/src/model_scanner.cpp
  backend/src/converter.cpp
  backend/src/task_manager.cpp
  backend/src/gguf_reader.cpp
  backend/src/model_deleter.cpp
  backend/src/system_service.cpp
  backend/src/http_client.cpp
  backend/src/hf_client.cpp
  backend/src/download_manager.cpp
  backend/src/llama_runtime.cpp
  backend/src/quality_service.cpp
  backend/src/watch_service.cpp
  backend/src/update_service.cpp
  backend/src/openapi.cpp
  backend/src/fs_browser.cpp
  backend/src/api_server.cpp
  backend/src/app.cpp
)

target_include_directories(modelforge PRIVATE
  backend/include
  backend/third_party
)

target_compile_definitions(modelforge PRIVATE
  MODELEFORGE_VERSION="${PROJECT_VERSION}"
  CPPHTTPLIB_THREAD_POOL_COUNT=8
  CPPHTTPLIB_RECV_BUFSIZ=32768
)

if(WIN32)
  # Embed the same icon shipped with the Web UI into modelforge.exe. The ICO is
  # generated from docs/images/logo.png (see scripts/generate-icons.py).
  enable_language(RC)
  # Forward slashes matter here: Resource Compiler strings treat \a and \f as
  # escape sequences, which corrupts paths such as D:\a\...\frontend\... on CI.
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/frontend/public/favicon.ico" MODELEFORGE_ICON)
  configure_file(backend/modelforge.rc.in ${CMAKE_CURRENT_BINARY_DIR}/modelforge.rc @ONLY)
  target_sources(modelforge PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/modelforge.rc)
  target_compile_definitions(modelforge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  target_link_libraries(modelforge PRIVATE sqlite3 ws2_32 winhttp crypt32 psapi Threads::Threads)
  if(MINGW)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
      target_link_libraries(modelforge PRIVATE stdc++fs)
    endif()
    target_link_options(modelforge PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  target_link_libraries(modelforge PRIVATE sqlite3 Threads::Threads ${CMAKE_DL_LIBS})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(modelforge PRIVATE stdc++fs)
  endif()
endif()

install(TARGETS modelforge RUNTIME DESTINATION bin)
install(DIRECTORY frontend/dist/ DESTINATION share/modelforge/web OPTIONAL)

enable_testing()
add_executable(modelforge_tests tests/unit/core_tests.cpp backend/src/util.cpp backend/src/crypto.cpp backend/src/http_client.cpp backend/src/hf_client.cpp backend/src/process_runner.cpp backend/src/logger.cpp backend/src/update_service.cpp backend/src/database.cpp backend/src/model_deleter.cpp backend/src/settings.cpp backend/src/types.cpp backend/src/llama_runtime.cpp backend/src/fs_browser.cpp)
target_include_directories(modelforge_tests PRIVATE backend/include backend/third_party)
if(WIN32)
  target_compile_definitions(modelforge_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  target_link_libraries(modelforge_tests PRIVATE sqlite3 ws2_32 winhttp crypt32 psapi Threads::Threads)
  if(MINGW)
    target_link_options(modelforge_tests PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
else()
  target_link_libraries(modelforge_tests PRIVATE sqlite3 Threads::Threads ${CMAKE_DL_LIBS})
endif()
target_compile_definitions(modelforge_tests PRIVATE MODELEFORGE_VERSION="${PROJECT_VERSION}" MODELEFORGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core_tests COMMAND modelforge_tests)
