cmake_minimum_required(VERSION 3.19)
project(SollunaMail LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network LinguistTools)
find_package(Qt6 QUIET COMPONENTS WebEngineWidgets)

qt_standard_project_setup()

# Windows MSVC 2022: UTF-8 (none BOM) 소스코드 인코딩을 정확히 처리하도록 /utf-8 옵션 강제 적용
if(MSVC)
    add_compile_options(/utf-8)
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

set(CMAKE_AUTORCC ON)

qt_add_executable(SollunaMail
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow_part2.cpp
    mainwindow.h
    mainwindow.ui
    mailmodels.h
    mailtheme.h
    mailtheme.cpp
    customwidgets.h
    customwidgets.cpp
    composedialog.h
    composedialog.cpp
    settingsdialog.h
    settingsdialog.cpp
    googleprovider.h
    googleprovider.cpp
    resources.qrc
    $<$<PLATFORM_ID:Windows>:app_icon.rc>
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/oauth_secrets_local.h")
    message(STATUS "SollunaMail: Local OAuth secrets header (oauth_secrets_local.h) detected.")
    target_sources(SollunaMail PRIVATE oauth_secrets_local.h)
    target_compile_definitions(SollunaMail PRIVATE SOLLUNA_HAS_LOCAL_OAUTH_SECRETS=1)
    set_source_files_properties(googleprovider.cpp PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/oauth_secrets_local.h"
    )
endif()

qt_add_translations(
    TARGETS SollunaMail
    TS_FILES SollunaMail_ko_KR.ts
)

target_link_libraries(SollunaMail
    PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Network
)

if(Qt6WebEngineWidgets_FOUND)
    message(STATUS "SollunaMail: Qt6::WebEngineWidgets (Chromium) detected and enabled.")
    target_link_libraries(SollunaMail PRIVATE Qt6::WebEngineWidgets)
    target_compile_definitions(SollunaMail PRIVATE SOLLUNA_HAS_WEBENGINE=1)
else()
    message(STATUS "SollunaMail: Qt6::WebEngineWidgets not found in current kit; using enhanced QTextBrowser + QPainter renderer.")
endif()

if(APPLE)
    set_target_properties(SollunaMail PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.solluna.mail"
        MACOSX_BUNDLE_BUNDLE_NAME "SollunaMail"
        MACOSX_BUNDLE_ICON_FILE "solluna_icon.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
endif()

install(TARGETS SollunaMail
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET SollunaMail
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
