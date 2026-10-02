# The Android Studio project: with EACP_ANDROID_STUDIO_DIR set, an Android
# configure writes a Gradle project there with one module per eacp_add_app, as
# -G Xcode writes an Xcode project. Gradle compiles nothing of its own: each
# module's externalNativeBuild runs this same CMakeLists.txt for its one target
# and packages the library behind the NativeActivity manifest. Versions come
# from AndroidVersions.cmake, the wrapper from Gradle's repository at the
# pinned release, checked against its hash. `cmake --preset android-studio`
# writes it to build-android-studio.

set(EACP_ANDROID_STUDIO_DIR "" CACHE PATH
        "Write an Android Studio (Gradle) project for every app here")
set(EACP_ANDROID_STUDIO_ABIS "arm64-v8a;x86_64" CACHE STRING
        "The ABIs the Android Studio project builds")

function(eacp_android_studio_add_app target)
    if (NOT EACP_ANDROID_STUDIO_DIR)
        return()
    endif ()

    cmake_parse_arguments(APP "" "PACKAGE;LABEL;ORIENTATION;VERSION_CODE;\
VERSION_NAME;RES_DIR;ICON_ATTRIBUTE;MIN_SDK" "" ${ARGN})

    if (APP_RES_DIR)
        get_filename_component(APP_RES_DIR "${APP_RES_DIR}" ABSOLUTE)
    endif ()

    foreach (key PACKAGE LABEL ORIENTATION VERSION_CODE VERSION_NAME RES_DIR
            ICON_ATTRIBUTE MIN_SDK)
        set_property(TARGET ${target} PROPERTY EACP_STUDIO_${key} "${APP_${key}}")
    endforeach ()

    set_property(GLOBAL APPEND PROPERTY EACP_ANDROID_STUDIO_APPS ${target})
endfunction()

function(eacp_quoted_list out)
    list(TRANSFORM ARGN PREPEND "\"")
    list(TRANSFORM ARGN APPEND "\"")
    list(JOIN ARGN ", " joined)
    set(${out} "${joined}" PARENT_SCOPE)
endfunction()

function(eacp_android_studio_fetch url file sha256)
    if (EXISTS "${file}")
        file(SHA256 "${file}" existing)

        if (existing STREQUAL sha256)
            return()
        endif ()
    endif ()

    file(DOWNLOAD "${url}" "${file}" STATUS status EXPECTED_HASH SHA256=${sha256})
    list(GET status 0 code)

    if (NOT code EQUAL 0)
        list(GET status 1 error)
        message(WARNING "Android Studio project: could not download ${url} "
                "(${error}). Android Studio opens the project without it; "
                "gradlew on the command line needs it.")
    endif ()
endfunction()

function(eacp_android_studio_write_wrapper dir)
    set(base "https://raw.githubusercontent.com/gradle/gradle/v${EACP_ANDROID_GRADLE}")

    eacp_android_studio_fetch("${base}/gradle/wrapper/gradle-wrapper.jar"
            "${dir}/gradle/wrapper/gradle-wrapper.jar"
            ${EACP_ANDROID_GRADLE_WRAPPER_JAR_SHA256})
    eacp_android_studio_fetch("${base}/gradlew" "${dir}/gradlew"
            ${EACP_ANDROID_GRADLEW_SHA256})
    eacp_android_studio_fetch("${base}/gradlew.bat" "${dir}/gradlew.bat"
            ${EACP_ANDROID_GRADLEW_BAT_SHA256})

    if (EXISTS "${dir}/gradlew")
        file(CHMOD "${dir}/gradlew" FILE_PERMISSIONS OWNER_READ OWNER_WRITE
                OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    endif ()
endfunction()

function(eacp_android_studio_write_app dir target)
    foreach (key PACKAGE LABEL ORIENTATION VERSION_CODE VERSION_NAME RES_DIR
            ICON_ATTRIBUTE MIN_SDK)
        get_property(EACP_APK_${key} TARGET ${target} PROPERTY EACP_STUDIO_${key})
    endforeach ()

    set(EACP_APK_LIB_NAME "${target}")
    set(EACP_STUDIO_RES "")

    if (EACP_APK_RES_DIR)
        set(EACP_STUDIO_RES "
    sourceSets.getByName(\"main\").res.srcDir(\"${EACP_APK_RES_DIR}\")
")
    endif ()

    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidStudio")
    configure_file("${templates}/app.build.gradle.kts.in"
            "${dir}/${target}/build.gradle.kts" @ONLY)
    configure_file("${templates}/AndroidManifest.xml.in"
            "${dir}/${target}/src/main/AndroidManifest.xml" @ONLY)
endfunction()

function(eacp_write_android_studio_project)
    get_property(apps GLOBAL PROPERTY EACP_ANDROID_STUDIO_APPS)
    set(dir "${EACP_ANDROID_STUDIO_DIR}")
    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidStudio")

    # The CPM cache the configure that wrote this project filled, so each
    # module's own configure, one per ABI and build type, fetches nothing.
    set(arguments "-DEACP_UNITY_BUILD=OFF")

    if (CPM_SOURCE_CACHE)
        list(APPEND arguments "-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}")
    endif ()

    eacp_quoted_list(EACP_STUDIO_CMAKE_ARGUMENTS ${arguments})
    eacp_quoted_list(EACP_STUDIO_ABIS ${EACP_ANDROID_STUDIO_ABIS})
    set(EACP_STUDIO_CMAKE_LISTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
    set(EACP_STUDIO_NAME "${CMAKE_PROJECT_NAME}")
    set(EACP_STUDIO_SDK_DIR "${EACP_ANDROID_SDK}")
    get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
    get_filename_component(EACP_STUDIO_CMAKE_DIR "${cmake_bin}" DIRECTORY)

    set(EACP_STUDIO_INCLUDES "")

    foreach (app IN LISTS apps)
        string(APPEND EACP_STUDIO_INCLUDES "include(\":${app}\")\n")
        eacp_android_studio_write_app("${dir}" ${app})
    endforeach ()

    configure_file("${templates}/settings.gradle.kts.in"
            "${dir}/settings.gradle.kts" @ONLY)
    configure_file("${templates}/build.gradle.kts.in" "${dir}/build.gradle.kts" @ONLY)
    configure_file("${templates}/gradle.properties.in" "${dir}/gradle.properties" @ONLY)
    configure_file("${templates}/local.properties.in" "${dir}/local.properties" @ONLY)
    configure_file("${templates}/gradle-wrapper.properties.in"
            "${dir}/gradle/wrapper/gradle-wrapper.properties" @ONLY)
    eacp_android_studio_write_wrapper("${dir}")

    list(LENGTH apps count)
    message(STATUS "Android Studio project with ${count} app(s): ${dir}")
endfunction()

if (EACP_ANDROID_STUDIO_DIR)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL eacp_write_android_studio_project)
endif ()
