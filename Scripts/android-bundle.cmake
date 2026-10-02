# Usage: cmake -DCONFIG=<target>-aab.cmake -P Scripts/android-bundle.cmake
#
# The App Bundle Google Play takes, with no Gradle: CONFIG's Release library
# built for each ABI in its own tree, the manifest and resources linked by aapt2
# in protobuf format, and bundletool's build-bundle with the libraries stripped,
# stored uncompressed and 16 KB aligned, and their symbols for Play's crash
# reports. jarsigner signs it with the upload key EACP_ANDROID_KEYSTORE,
# EACP_ANDROID_KEY_ALIAS and EACP_ANDROID_KEYSTORE_PASSWORD name, and bundletool
# validates it.

cmake_minimum_required(VERSION 3.31)

set(eacp_script android-bundle)
include("${CMAKE_CURRENT_LIST_DIR}/android-common.cmake")
include("${CONFIG}")

foreach (variable EACP_ANDROID_KEYSTORE EACP_ANDROID_KEY_ALIAS
        EACP_ANDROID_KEYSTORE_PASSWORD)
    if ("$ENV{${variable}}" STREQUAL "")
        eacp_fail("set EACP_ANDROID_KEYSTORE, EACP_ANDROID_KEY_ALIAS and "
                "EACP_ANDROID_KEYSTORE_PASSWORD to the upload key; ${variable} is unset")
    endif ()
endforeach ()

eacp_find_java(java)

if (NOT java)
    eacp_fail("no Java 17+ found; run cmake -P Scripts/android-setup.cmake, or set "
            "JAVA_HOME to a JDK 17+")
endif ()

get_filename_component(java_bin "${java}" DIRECTORY)
find_program(jarsigner jarsigner HINTS "${java_bin}" NO_CACHE REQUIRED)

set(bundletool "${eacp_android_dir}/bundletool-${EACP_ANDROID_BUNDLETOOL}.jar")

if (NOT EXISTS "${bundletool}")
    file(DOWNLOAD "https://github.com/google/bundletool/releases/download/\
${EACP_ANDROID_BUNDLETOOL}/bundletool-all-${EACP_ANDROID_BUNDLETOOL}.jar"
            "${bundletool}" EXPECTED_HASH SHA256=${EACP_ANDROID_BUNDLETOOL_SHA256})
endif ()

function(run)
    execute_process(COMMAND ${ARGN} COMMAND_ERROR_IS_FATAL ANY)
endfunction()

set(work "${OUT}.work")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/base/manifest")
get_filename_component(library_name "${LIBRARY}" NAME)
set(metadata "")

foreach (abi IN LISTS ABIS)
    set(build "${BUILD_ROOT}/${abi}")
    run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${build}" "-DANDROID_ABI=${abi}"
            ${CONFIGURE_ARGUMENTS} OUTPUT_QUIET)
    run("${CMAKE_COMMAND}" --build "${build}" --target "${TARGET}")

    file(MAKE_DIRECTORY "${work}/base/lib/${abi}" "${work}/symbols/${abi}")
    run("${STRIP}" --strip-unneeded -o "${work}/base/lib/${abi}/${library_name}"
            "${build}/${LIBRARY}")
    set(symbols "${work}/symbols/${abi}/${library_name}.sym")
    run("${OBJCOPY}" --strip-debug "${build}/${LIBRARY}" "${symbols}")
    list(APPEND metadata "--metadata-file=com.android.tools.build.debugsymbols/\
${abi}/${library_name}.sym:${symbols}")
endforeach ()

set(build_tools "${SDK}/build-tools/${EACP_ANDROID_BUILD_TOOLS}")
set(link_args "")

if (RES_DIR AND IS_DIRECTORY "${RES_DIR}")
    run("${build_tools}/aapt2" compile --dir "${RES_DIR}" -o "${work}/res.zip")
    set(link_args -R "${work}/res.zip" --auto-add-overlay)
endif ()

run("${build_tools}/aapt2" link --proto-format -o "${work}/linked.apk"
        --manifest "${MANIFEST}"
        -I "${SDK}/platforms/android-${EACP_ANDROID_TARGET_SDK}/android.jar"
        ${link_args})
file(ARCHIVE_EXTRACT INPUT "${work}/linked.apk" DESTINATION "${work}/base")
file(RENAME "${work}/base/AndroidManifest.xml"
        "${work}/base/manifest/AndroidManifest.xml")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${work}/base.zip" --format=zip .
        WORKING_DIRECTORY "${work}/base" COMMAND_ERROR_IS_FATAL ANY)

file(WRITE "${work}/config.json" [[{"optimizations": {"uncompressNativeLibraries":
    {"enabled": true, "alignment": "PAGE_ALIGNMENT_16K"}}}]])
file(REMOVE "${OUT}")
run("${java}" -jar "${bundletool}" build-bundle "--modules=${work}/base.zip"
        "--config=${work}/config.json" ${metadata} "--output=${OUT}")
run("${jarsigner}" -sigalg SHA256withRSA -digestalg SHA-256
        -keystore "$ENV{EACP_ANDROID_KEYSTORE}"
        -storepass:env EACP_ANDROID_KEYSTORE_PASSWORD "${OUT}"
        "$ENV{EACP_ANDROID_KEY_ALIAS}")
run("${java}" -jar "${bundletool}" validate "--bundle=${OUT}" OUTPUT_QUIET)
file(REMOVE_RECURSE "${work}")

message(NOTICE "${OUT}")
