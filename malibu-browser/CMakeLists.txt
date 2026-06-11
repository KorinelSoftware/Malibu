cmake_minimum_required(VERSION 3.25)

project(malibu_browser LANGUAGES CXX)

# Find GTK4
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)

# Networking: curl (host-side TLS, same as malibu_render tool)
find_package(CURL REQUIRED)

add_executable(malibu-browser
    src/main.cpp
    src/BrowserWindow.cpp
    src/WebViewWidget.cpp
    src/NetworkHandler.cpp
)

target_include_directories(malibu-browser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/malibu-view/include
    ${CMAKE_SOURCE_DIR}/core/include
)

target_compile_features(malibu-browser PRIVATE cxx_std_20)
target_compile_options(malibu-browser PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    ${GTK4_CFLAGS_OTHER}
)

target_link_libraries(malibu-browser PRIVATE
    malibu_view
    ${GTK4_LIBRARIES}
    ${CURL_LIBRARIES}
)

# Install (optional)
# install(TARGETS malibu-browser DESTINATION bin)
