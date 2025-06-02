cmake_minimum_required(VERSION 3.10)
project(KamakuraBrowser C)

# Set the C standard (e.g., C99 or C11)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True) # Ensures the standard is enforced

# Find PkgConfig to locate GTK and WebKitGTK
find_package(PkgConfig REQUIRED)

# Use PkgConfig to find GTK+ 3.0
pkg_check_modules(GTK REQUIRED gtk+-3.0)

# Use PkgConfig to find WebKit2GTK 4.0 (or 4.1 if you use newer features)
# The previous C code used 4.0 features.
pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)

# Create the executable
add_executable(kamakura kamakura-browser.c) # Assuming your source file is kamakura-browser.c

# Add include directories for GTK and WebKit to the target
target_include_directories(kamakura PRIVATE
    ${GTK_INCLUDE_DIRS}
    ${WEBKIT_INCLUDE_DIRS}
)

# Link against GTK and WebKit libraries
target_link_libraries(kamakura PRIVATE
    ${GTK_LIBRARIES}
    ${WEBKIT_LIBRARIES}
)

# Add common compiler warnings to the target
target_compile_options(kamakura PRIVATE
    -Wall
    -Wextra
    # Add any other warnings you find useful
)

# Apply any other CFLAGS from pkg-config (e.g., for specific defines or flags)
target_compile_options(kamakura PRIVATE
    ${GTK_CFLAGS_OTHER}
    ${WEBKIT_CFLAGS_OTHER}
)

# Option to enable WEBKIT_SETTINGS_ENABLE_MEDIA_SOURCE (matches the C code's #ifdef)
option(ENABLE_MEDIA_SOURCE "Enable WebKit Media Source Extensions" ON)
if(ENABLE_MEDIA_SOURCE)
    target_compile_definitions(kamakura PRIVATE WEBKIT_SETTINGS_ENABLE_MEDIA_SOURCE)
    message(STATUS "WebKit Media Source Extensions will be enabled in the build.")
else()
    message(STATUS "WebKit Media Source Extensions will be disabled in the build.")
endif()

# --- Copy necessary resource files to the build directory ---
# This ensures they are available when running the executable from the build tree.
# The C code expects: kamakura_browser.glade, block.txt, style.css
set(RESOURCE_FILES
    kamakura_browser.glade
    block.txt
    style.css
)

foreach(RESOURCE_FILE ${RESOURCE_FILES})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_FILE}")
        file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_FILE}
             DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
        )
    else()
        message(WARNING "Resource file not found: ${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCE_FILE}")
    endif()
endforeach()

# Optional: Add install rules if you plan to install the browser
# install(TARGETS kamakura DESTINATION bin)
# install(FILES ${RESOURCE_FILES} DESTINATION share/kamakurabrowser) # Adjust path as needed

message(STATUS "KamakuraBrowser configured. Build with 'cmake --build .'")
message(STATUS "Run from build directory: ${CMAKE_CURRENT_BINARY_DIR}/kamakura")