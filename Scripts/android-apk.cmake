# Usage: cmake -DSDK=<sdk> -DBUILD_TOOLS=<version> -DPLATFORM=<android-N>
#              -DMANIFEST=<file> -DLIBRARY=<lib.so> -DABI=<abi> -DSTRIP=<tool>
#              -DOUT=<out.apk> [-DRES_DIR=<dir>] [-DDEBUG=1]
#              -P Scripts/android-apk.cmake
#
# Packages one native library behind a NativeActivity manifest, with no Gradle:
# aapt2 links the manifest (and any resources), the library goes in under
# lib/<abi>/, then zipalign and apksigner with the debug keystore. DEBUG marks
# the app debuggable, for run-as and a debugger to attach, and keeps the
# library's symbols; otherwise STRIP (the NDK's llvm-strip) strips the copy in
# the APK, and the build tree keeps the unstripped one for ndk-stack.

cmake_minimum_required(VERSION 3.31)

set(eacp_script android-apk)
include("${CMAKE_CURRENT_LIST_DIR}/android-common.cmake")

set(build_tools "${SDK}/build-tools/${BUILD_TOOLS}")

function(tool var)
    find_program(${var} NAMES ${ARGN} HINTS "${build_tools}" NO_DEFAULT_PATH NO_CACHE)

    if (NOT ${var})
        eacp_fail("no ${ARGV1} in ${build_tools}: run cmake -P Scripts/android-setup.cmake")
    endif ()

    set(${var} "${${var}}" PARENT_SCOPE)
endfunction()

tool(aapt2 aapt2)
tool(zipalign zipalign)
tool(apksigner apksigner.bat apksigner)

eacp_find_java(java)

if (NOT java)
    eacp_fail("no Java 17+ found; run cmake -P Scripts/android-setup.cmake, or set "
            "JAVA_HOME to a JDK 17+")
endif ()

# apksigner runs the java at JAVA_HOME; keytool and jar sit beside java.
get_filename_component(java_bin "${java}" DIRECTORY)
get_filename_component(java_home "${java_bin}" DIRECTORY)
set(ENV{JAVA_HOME} "${java_home}")
find_program(keytool keytool HINTS "${java_bin}" NO_CACHE REQUIRED)
find_program(jar jar HINTS "${java_bin}" NO_CACHE REQUIRED)

function(run)
    execute_process(COMMAND ${ARGN} COMMAND_ERROR_IS_FATAL ANY)
endfunction()

set(work "${OUT}.work")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

set(link_args "")

if (DEBUG)
    list(APPEND link_args --debug-mode)
endif ()

if (RES_DIR AND IS_DIRECTORY "${RES_DIR}")
    run("${aapt2}" compile --dir "${RES_DIR}" -o "${work}/res.zip")
    list(APPEND link_args -R "${work}/res.zip" --auto-add-overlay)
endif ()

run("${aapt2}" link -o "${work}/base.apk" --manifest "${MANIFEST}"
        -I "${SDK}/platforms/${PLATFORM}/android.jar" ${link_args})

get_filename_component(library_name "${LIBRARY}" NAME)
set(staged "${work}/stage/lib/${ABI}/${library_name}")
file(MAKE_DIRECTORY "${work}/stage/lib/${ABI}")

if (DEBUG)
    file(COPY_FILE "${LIBRARY}" "${staged}")
else ()
    run("${STRIP}" --strip-unneeded -o "${staged}" "${LIBRARY}")
endif ()

run("${jar}" uf "${work}/base.apk" -C "${work}/stage" lib)
run("${zipalign}" -P 16 -f 4 "${work}/base.apk" "${work}/aligned.apk")

if (DEFINED ENV{EACP_ANDROID_KEYSTORE} AND NOT "$ENV{EACP_ANDROID_KEYSTORE}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{EACP_ANDROID_KEYSTORE}" keystore)
else ()
    set(keystore "${eacp_home}/.android/debug.keystore")
endif ()

if (NOT EXISTS "${keystore}")
    get_filename_component(keystore_dir "${keystore}" DIRECTORY)
    file(MAKE_DIRECTORY "${keystore_dir}")
    execute_process(
            COMMAND "${keytool}" -genkeypair -keystore "${keystore}" -storepass android
                    -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048
                    -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
            OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
endif ()

run("${apksigner}" sign --ks "${keystore}" --ks-pass pass:android
        --key-pass pass:android --out "${OUT}" "${work}/aligned.apk")
file(REMOVE "${OUT}.idsig")
file(REMOVE_RECURSE "${work}")

message(NOTICE "${OUT}")
