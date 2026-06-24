eacp_add_webview_app(Calculator
        APP_HEADER  App.h
        SOURCES     Main.cpp
        API         Api::CalculatorApi
        API_HEADER  Types.h
        WEB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/web
        CATEGORY    CalculatorApp
        BUNDLE_ID   com.eacp.calculator
        BUNDLE_NAME "Calculator"
        REACT)
