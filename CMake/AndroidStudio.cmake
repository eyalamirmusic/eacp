# The Android Studio project: with EACP_ANDROID_STUDIO_DIR set, an Android
# configure writes a Gradle project there with one module per eacp_add_app, as
# -G Xcode writes an Xcode project. Gradle compiles nothing of its own: each
# module's externalNativeBuild runs the top-level CMakeLists.txt for its one target
# and packages the library behind the manifest eacp_add_android_apk configured,
# less what the module itself declares. Versions come from
# AndroidVersions.cmake, the wrapper from Gradle's repository at the pinned
# release, checked against its hash.

include_guard(GLOBAL)

set(EACP_ANDROID_STUDIO_DIR "" CACHE PATH
        "Write an Android Studio (Gradle) project for every app here")
set(EACP_ANDROID_STUDIO_ABIS "arm64-v8a;x86_64" CACHE STRING
        "The ABIs the Android Studio project builds")
set(EACP_ANDROID_STUDIO_CMAKE_ARGS "" CACHE STRING
        "More -D arguments for the configure each Gradle module runs")

function(eacp_android_studio_add_app target)
    if (NOT EACP_ANDROID_STUDIO_DIR)
        return()
    endif ()

    cmake_parse_arguments(APP ""
            "PACKAGE;VERSION_CODE;VERSION_NAME;RES_DIR;MIN_SDK;MANIFEST" "" ${ARGN})

    if (APP_RES_DIR)
        get_filename_component(APP_RES_DIR "${APP_RES_DIR}" ABSOLUTE)
    endif ()

    foreach (key PACKAGE VERSION_CODE VERSION_NAME RES_DIR MIN_SDK MANIFEST)
        set_property(TARGET ${target} PROPERTY EACP_STUDIO_${key} "${APP_${key}}")
    endforeach ()

    set_property(GLOBAL APPEND PROPERTY EACP_ANDROID_STUDIO_APPS ${target})
endfunction()

function(eacp_quoted_list out separator)
    list(TRANSFORM ARGN PREPEND "\"")
    list(TRANSFORM ARGN APPEND "\"")
    list(JOIN ARGN "${separator}" joined)
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

# Gradle takes the package, versions and SDK levels from the module, and
# rejects a source manifest that also declares them.
function(eacp_android_studio_write_manifest manifest out)
    file(READ "${manifest}" xml)
    string(REGEX REPLACE "[ \t\r\n]+(package|android:versionCode|android:versionName\
|android:extractNativeLibs)=\"[^\"]*\"" "" xml "${xml}")
    string(REGEX REPLACE "[ \t]*<uses-sdk[^>]*>[ \t]*\r?\n([ \t]*\r?\n)?" ""
            xml "${xml}")
    file(WRITE "${out}.new" "${xml}")
    file(COPY_FILE "${out}.new" "${out}" ONLY_IF_DIFFERENT)
    file(REMOVE "${out}.new")
endfunction()

function(eacp_android_studio_write_app dir target)
    foreach (key PACKAGE VERSION_CODE VERSION_NAME RES_DIR MIN_SDK MANIFEST)
        get_property(EACP_APK_${key} TARGET ${target} PROPERTY EACP_STUDIO_${key})
    endforeach ()

    set(EACP_APK_LIB_NAME "${target}")
    set(EACP_STUDIO_RES "")

    if (EACP_APK_RES_DIR)
        set(EACP_STUDIO_RES "
    sourceSets.getByName(\"main\").res.directories.add(\"${EACP_APK_RES_DIR}\")
")
    endif ()

    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidStudio")
    configure_file("${templates}/app.build.gradle.kts.in"
            "${dir}/${target}/build.gradle.kts" @ONLY)
    file(MAKE_DIRECTORY "${dir}/${target}/src/main")
    eacp_android_studio_write_manifest("${EACP_APK_MANIFEST}"
            "${dir}/${target}/src/main/AndroidManifest.xml")
endfunction()

function(eacp_write_android_studio_project)
    get_property(apps GLOBAL PROPERTY EACP_ANDROID_STUDIO_APPS)
    set(dir "${EACP_ANDROID_STUDIO_DIR}")
    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidStudio")

    # Every package this configure fetched, by the source it fetched, so each
    # module's own configure, one per ABI and build type, fetches nothing and
    # builds the same sources, a CPM_<name>_SOURCE checkout included.
    set(arguments "-DEACP_UNITY_BUILD=OFF")

    foreach (package IN LISTS CPM_PACKAGES)
        if (CPM_PACKAGE_${package}_SOURCE_DIR)
            list(APPEND arguments
                    "-DCPM_${package}_SOURCE=${CPM_PACKAGE_${package}_SOURCE_DIR}")
        endif ()
    endforeach ()

    if (CPM_SOURCE_CACHE)
        list(APPEND arguments "-DCPM_SOURCE_CACHE=${CPM_SOURCE_CACHE}")
    endif ()

    list(APPEND arguments ${EACP_ANDROID_STUDIO_CMAKE_ARGS})

    eacp_quoted_list(EACP_STUDIO_CMAKE_ARGUMENTS ",\n                        "
            ${arguments})
    eacp_quoted_list(EACP_STUDIO_ABIS ", " ${EACP_ANDROID_STUDIO_ABIS})
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
    set(first "<app>")

    if (apps)
        list(GET apps 0 first)
    endif ()

    eacp_android_studio_java_home(java_home)
    message(STATUS "Android Studio project with ${count} app(s): ${dir}\n"
            "   Open it in Android Studio, or from a terminal there:\n"
            "   JAVA_HOME=\"${java_home}\" ./gradlew :${first}:installDebug")
endfunction()

# gradlew runs the java at JAVA_HOME, or the PATH's, which on a Mac is a stub
# that fails until a JDK is installed system-wide.
function(eacp_android_studio_java_home out)
    set(eacp_script AndroidStudio)
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../Scripts/android-common.cmake")
    eacp_find_java(java)
    set(home "<a JDK 17+>")

    if (java)
        get_filename_component(bin "${java}" DIRECTORY)
        get_filename_component(home "${bin}" DIRECTORY)
    endif ()

    set(${out} "${home}" PARENT_SCOPE)
endfunction()

if (EACP_ANDROID_STUDIO_DIR)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
            CALL eacp_write_android_studio_project)
endif ()
