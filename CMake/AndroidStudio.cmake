# The Android Studio project: with EACP_ANDROID_STUDIO_DIR set, an Android
# configure writes a Gradle project there with one module per eacp_add_app, as
# -G Xcode writes an Xcode project. Gradle compiles nothing of its own: each
# module's externalNativeBuild runs the top-level CMakeLists.txt for its one
# target and packages the library behind the manifest eacp_add_android_apk
# configured, less what the module itself declares; its release build type is
# CMake's Release, as the plain build's is, not the plugin's RelWithDebInfo.
# Versions come from AndroidVersions.cmake, the wrapper from Gradle's
# repository at the pinned release, checked against its hash.

include_guard(GLOBAL)

set(EACP_ANDROID_STUDIO_DIR "" CACHE PATH
        "Write an Android Studio (Gradle) project for every app here")
set(EACP_ANDROID_ABIS "arm64-v8a;x86_64" CACHE STRING
        "The ABIs the Android Studio project and <target>-aab build")

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

    # A shared run configuration per app with the native debugger. Left to
    # itself, Studio makes one with the Auto debugger, which picks Dual for this
    # module and waits for a Java debugger that the app, all native, never
    # serves, then kills it. Studio makes no configuration of its own for a
    # module that has one, and the first one is the selected one.
    set(EACP_STUDIO_APP "${target}")
    configure_file("${templates}/runConfiguration.xml.in"
            "${dir}/.idea/runConfigurations/${target}.xml" @ONLY)
endfunction()

# A line of the nested configures' init script that sets a cache entry as this
# one is.
function(eacp_android_studio_cache_line out name value type)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "$" "\\$" value "${value}")
    set(${out} "set(${name} \"${value}\" CACHE ${type} \"\" FORCE)\n" PARENT_SCOPE)
endfunction()

# The init script for the configures nested in this one, each Gradle module's
# (one per ABI and build type) and <target>-aab's: this configure's own cache,
# so a -D given here (a consumer's -DMYAPP_BUILD_TESTS=OFF, say) reaches them
# as it reaches a plain build. Left out is what the nested
# configure sets itself or what belongs to this one build: CMAKE_* and
# ANDROID_* (toolchain, ABI, platform, build type, generator) bar a CMAKE_*
# given on the command line, internal entries, find_* results, which are for
# this ABI's sysroot, anything inside this build tree, and the Studio
# directory, so a nested configure writes no project. Then every package this
# configure fetched, by the source it fetched, so they fetch nothing and build
# the same sources, a CPM_<name>_SOURCE checkout included.
#
# It goes in as CMAKE_PROJECT_TOP_LEVEL_INCLUDES, not -C: the first project()
# includes it on every configure, the ones Ninja reruns included, and Ninja
# reruns one when it changes. A -C cache is read once, so Gradle, which
# configures again only when its own arguments change, would keep the old one.
# Top-level includes given here, it includes in turn.
function(eacp_android_nested_init_cache out)
    set(file "${CMAKE_BINARY_DIR}/eacp-nested-init.cmake")
    set(content "# Written by eacp's AndroidStudio.cmake on every configure of\n")
    string(APPEND content "# ${CMAKE_BINARY_DIR}, for the configures nested in it.\n")
    set(gradle_owned CMAKE_TOOLCHAIN_FILE CMAKE_BUILD_TYPE CMAKE_MAKE_PROGRAM
            CMAKE_GENERATOR CMAKE_EXPORT_COMPILE_COMMANDS
            CMAKE_LIBRARY_OUTPUT_DIRECTORY CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(command_line "No help, variable specified on the command line.")
    get_cmake_property(names CACHE_VARIABLES)

    foreach (name IN LISTS names)
        get_property(type CACHE ${name} PROPERTY TYPE)
        get_property(help CACHE ${name} PROPERTY HELPSTRING)
        get_property(value CACHE ${name} PROPERTY VALUE)

        if (type MATCHES "^(INTERNAL|STATIC)$"
                OR name MATCHES "^(ANDROID_|CPM_|FETCHCONTENT_)"
                OR name MATCHES "^(EACP_ANDROID_STUDIO_DIR|CMAKE_PROJECT_TOP_LEVEL_INCLUDES)$"
                OR help MATCHES "^(Path to a |The directory containing a CMake)"
                OR value MATCHES "-NOTFOUND$")
            continue()
        endif ()

        if (name MATCHES "^CMAKE_" AND (NOT help STREQUAL command_line
                OR name IN_LIST gradle_owned OR name MATCHES "^CMAKE_(SYSTEM|ANDROID)_"))
            continue()
        endif ()

        string(FIND "${value}" "${CMAKE_BINARY_DIR}" in_build_tree)

        if (NOT in_build_tree EQUAL -1)
            continue()
        endif ()

        if (type STREQUAL "UNINITIALIZED")
            set(type STRING)
        endif ()

        eacp_android_studio_cache_line(line ${name} "${value}" ${type})
        string(APPEND content "${line}")
    endforeach ()

    foreach (package IN LISTS CPM_PACKAGES)
        if (CPM_PACKAGE_${package}_SOURCE_DIR)
            eacp_android_studio_cache_line(line CPM_${package}_SOURCE
                    "${CPM_PACKAGE_${package}_SOURCE_DIR}" PATH)
            string(APPEND content "${line}")
        endif ()
    endforeach ()

    if (CPM_SOURCE_CACHE)
        eacp_android_studio_cache_line(line CPM_SOURCE_CACHE "${CPM_SOURCE_CACHE}" PATH)
        string(APPEND content "${line}")
    endif ()

    foreach (include IN LISTS CMAKE_PROJECT_TOP_LEVEL_INCLUDES)
        get_filename_component(include "${include}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
        string(APPEND content "include([==[${include}]==])\n")
    endforeach ()

    file(WRITE "${file}.new" "${content}")
    file(COPY_FILE "${file}.new" "${file}" ONLY_IF_DIFFERENT)
    file(REMOVE "${file}.new")
    set(${out} "${file}" PARENT_SCOPE)
endfunction()

function(eacp_write_android_studio_project)
    get_property(apps GLOBAL PROPERTY EACP_ANDROID_STUDIO_APPS)
    set(dir "${EACP_ANDROID_STUDIO_DIR}")
    set(templates "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidStudio")

    eacp_android_nested_init_cache(includes)
    set(arguments "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${includes}")

    eacp_quoted_list(EACP_STUDIO_CMAKE_ARGUMENTS ",\n                        "
            ${arguments})
    eacp_quoted_list(EACP_STUDIO_ABIS ", " ${EACP_ANDROID_ABIS})
    set(EACP_STUDIO_CMAKE_LISTS "${CMAKE_SOURCE_DIR}/CMakeLists.txt")
    set(EACP_STUDIO_NAME "${CMAKE_PROJECT_NAME}")
    set(EACP_STUDIO_SDK_DIR "${EACP_ANDROID_SDK}")
    eacp_android_studio_cmake_dir("${dir}" EACP_STUDIO_CMAKE_DIR)

    set(EACP_STUDIO_INCLUDES "")
    set(EACP_STUDIO_GRADLE_MODULES "")

    foreach (app IN LISTS apps)
        string(APPEND EACP_STUDIO_INCLUDES "include(\":${app}\")\n")
        string(APPEND EACP_STUDIO_GRADLE_MODULES
                "            <option value=\"$PROJECT_DIR$/${app}\" />\n")
        eacp_android_studio_write_app("${dir}" ${app})
    endforeach ()

    # With .idea there, Studio opens the folder as a project rather than
    # importing it, so it needs telling that Gradle builds it. Written once:
    # Studio keeps this file up to date from then on.
    if (NOT EXISTS "${dir}/.idea/gradle.xml")
        configure_file("${templates}/gradle.xml.in" "${dir}/.idea/gradle.xml" @ONLY)
    endif ()

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

# local.properties' cmake.dir: the directory whose bin/ holds the CMake that ran
# this configure. The Android Gradle Plugin looks for Ninja beside that cmake,
# in the SDK's CMake packages and on the PATH, which an IDE started from the
# Dock or Finder does not share with a shell. So where this configure's Ninja
# is elsewhere, cmake.dir is a directory of links to the two.
function(eacp_android_studio_cmake_dir dir out)
    get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
    get_filename_component(cmake_dir "${cmake_bin}" DIRECTORY)
    get_filename_component(ninja_name "${CMAKE_MAKE_PROGRAM}" NAME_WE)
    get_filename_component(ninja_bin "${CMAKE_MAKE_PROGRAM}" DIRECTORY)
    set(${out} "${cmake_dir}" PARENT_SCOPE)

    if (NOT ninja_name STREQUAL "ninja" OR ninja_bin STREQUAL cmake_bin)
        return()
    endif ()

    get_filename_component(cmake_name "${CMAKE_COMMAND}" NAME)
    get_filename_component(ninja_file "${CMAKE_MAKE_PROGRAM}" NAME)
    set(links "${dir}/.cmake/bin")
    file(REMOVE_RECURSE "${links}")
    file(MAKE_DIRECTORY "${links}")
    file(CREATE_LINK "${CMAKE_COMMAND}" "${links}/${cmake_name}" RESULT cmake_failed
            SYMBOLIC)
    file(CREATE_LINK "${CMAKE_MAKE_PROGRAM}" "${links}/${ninja_file}"
            RESULT ninja_failed SYMBOLIC)

    if (cmake_failed OR ninja_failed)
        file(REMOVE_RECURSE "${dir}/.cmake")
        return()
    endif ()

    set(${out} "${dir}/.cmake" PARENT_SCOPE)
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
