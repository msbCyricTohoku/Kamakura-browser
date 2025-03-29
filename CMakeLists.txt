cmake_minimum_required(VERSION 3.10)
project(KamakuraBrowser C)

# We want to use C99.
set(CMAKE_C_STANDARD 99)

# Locate pkg-config, which finds our GTK and WebKitGTK libs.
find_package(PkgConfig REQUIRED)

# Check for GTK and WebKit2GTK.
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)

# Let CMake know about the include paths for GTK & WebKit.
include_directories(
    ${GTK_INCLUDE_DIRS}
    ${WEBKIT_INCLUDE_DIRS}
)

# Add some common compiler warnings (optional, but recommended).
add_compile_options(-Wall -Wextra)

# Define our executable from the source file.
add_executable(kamakura kamakura-browser.c)

# Link the GTK and WebKit libraries.
target_link_libraries(
    kamakura
    ${GTK_LIBRARIES}
    ${WEBKIT_LIBRARIES}
)

# Apply any extra CFLAGS from the pkg-config detection.
target_compile_options(
    kamakura PRIVATE
    ${GTK_CFLAGS_OTHER}
    ${WEBKIT_CFLAGS_OTHER}
)

# ---------------------------------------------------------------------------- #
# Copy resources to the build directory
# ---------------------------------------------------------------------------- #
file(COPY kamakura_browser.glade block.txt
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
