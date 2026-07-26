cmake_minimum_required(VERSION 3.12)
project(zwidget)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ZWIDGET_SOURCES
	src/core/canvas.cpp
	src/core/font.cpp
	src/core/image.cpp
	src/core/nanosvg/nanosvg.cpp
	src/core/pathfill.cpp
	src/core/picopng/picopng.cpp
	src/core/span_layout.cpp
	src/core/theme.cpp
	src/core/timer.cpp
	src/core/truetypefont.cpp
	src/core/utf8reader.cpp
	src/core/widget.cpp
	src/core/layout.cpp
	src/systemdialogs/open_file_dialog.cpp
	src/systemdialogs/open_folder_dialog.cpp
	src/systemdialogs/save_file_dialog.cpp
	src/widgets/checkboxlabel/checkboxlabel.cpp
	src/widgets/dropdown/dropdown.cpp
	src/widgets/imagebox/imagebox.cpp
	src/widgets/lineedit/lineedit.cpp
	src/widgets/listview/listview.cpp
	src/widgets/mainwindow/mainwindow.cpp
	src/widgets/menubar/menubar.cpp
	src/widgets/pushbutton/pushbutton.cpp
	src/widgets/scrollbar/scrollbar.cpp
	src/widgets/statusbar/statusbar.cpp
	src/widgets/tabwidget/tabwidget.cpp
	src/widgets/textedit/textedit.cpp
	src/widgets/textlabel/textlabel.cpp
	src/widgets/toolbar/toolbar.cpp
	src/widgets/toolbar/toolbarbutton.cpp
	src/widgets/layout/hboxlayout.cpp
	src/widgets/layout/vboxlayout.cpp
	src/window/window.cpp
	src/window/ztimer/ztimer.cpp
	src/window/stub/stub_open_file_dialog.cpp
	src/window/stub/stub_open_folder_dialog.cpp
	src/window/stub/stub_save_file_dialog.cpp
)

set(ZWIDGET_WIN32_SOURCES
	src/window/win32/win32_display_backend.cpp
	src/window/win32/win32_display_window.cpp
	src/window/win32/win32_open_file_dialog.cpp
	src/window/win32/win32_open_folder_dialog.cpp
	src/window/win32/win32_save_file_dialog.cpp
)

set(ZWIDGET_COCOA_SOURCES
	src/window/cocoa/cocoa_display_backend.mm
	src/window/cocoa/cocoa_display_window.mm
	src/window/cocoa/cocoa_open_file_dialog.mm
	src/window/cocoa/cocoa_open_folder_dialog.mm
	src/window/cocoa/cocoa_save_file_dialog.mm
)

# The cocoa sources use ARC constructs such as __weak and contain no manual
# retain/release, so they must be compiled with ARC enabled. Matches upstream.
set_source_files_properties(${ZWIDGET_COCOA_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")

set(ZWIDGET_X11_SOURCES
	src/window/x11/x11_display_backend.cpp
	src/window/x11/x11_display_window.cpp
	src/window/x11/x11_connection.cpp
)

set(ZWIDGET_WAYLAND_SOURCES
	src/window/wayland/wayland_display_backend.cpp
	src/window/wayland/wayland_display_window.cpp
	src/window/wayland/xdg-shell-protocol.c
	src/window/wayland/xdg-output-unstable-v1-protocol.c
	src/window/wayland/xdg-foreign-unstable-v2-protocol.c
	src/window/wayland/pointer-constraints-unstable-v1-protocol.c
	src/window/wayland/xdg-activation-v1-protocol.c
	src/window/wayland/xdg-decoration-unstable-v1-protocol.c
	src/window/wayland/fractional-scale-v1-protocol.c
	src/window/wayland/relative-pointer-unstable-v1-protocol.c
	src/window/wayland/xdg-toplevel-icon-v1-protocol.c
	src/window/wayland/cursor-shape-v1-protocol.c
	src/window/wayland/wayland-client-protocol.c
	src/window/wayland/tablet-unstable-v2-protocol.c
)

set(ZWIDGET_SDL2_SOURCES
	src/window/sdl2/sdl2_display_backend.cpp
	src/window/sdl2/sdl2_display_window.cpp
)

# Wayland sources no longer need forced remapping header

if(WIN32)
	set(ZWIDGET_SOURCES ${ZWIDGET_SOURCES} ${ZWIDGET_WIN32_SOURCES})
	add_definitions(-DUSE_WIN32)
elseif(APPLE)
	set(ZWIDGET_SOURCES ${ZWIDGET_SOURCES} ${ZWIDGET_COCOA_SOURCES})
	add_definitions(-DUSE_COCOA)
elseif(LINUX OR FREEBSD OR OPENBSD)
	set(ZWIDGET_SOURCES ${ZWIDGET_SOURCES} ${ZWIDGET_X11_SOURCES} ${ZWIDGET_WAYLAND_SOURCES})
	add_definitions(-DUSE_X11 -DUSE_WAYLAND)
endif()

add_library(zwidget STATIC ${ZWIDGET_SOURCES})
target_include_directories(zwidget PUBLIC include include/zwidget PRIVATE src)

if(LINUX OR FREEBSD OR OPENBSD)
	find_package(X11 REQUIRED)
	target_link_libraries(zwidget PRIVATE)
endif()
