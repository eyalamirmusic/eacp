include(FindPkgConfig)

# The Wayland half of the Linux graphics backend: the client library, the cursor
# loader, xkbcommon and libdecor, plus the protocol XML wayland-scanner reads.

if (NOT TARGET eacp-wayland)
    pkg_check_modules(EACP_WAYLAND IMPORTED_TARGET
            wayland-client
            wayland-cursor
            xkbcommon
            libdecor-0)

    if (NOT EACP_WAYLAND_FOUND)
        message(FATAL_ERROR
                "EACP_LINUX_GRAPHICS is ON but the Wayland client libraries "
                "were not found. On Debian/Ubuntu:\n"
                "  sudo apt-get install libwayland-dev wayland-protocols "
                "libwayland-bin libxkbcommon-dev libdecor-0-dev pkg-config")
    endif ()

    # The tool and the XML directory come from the .pc files, never from a path.
    pkg_check_modules(EACP_WAYLAND_SCANNER_PC REQUIRED wayland-scanner)
    pkg_check_modules(EACP_WAYLAND_PROTOCOLS_PC REQUIRED wayland-protocols)

    pkg_get_variable(EACP_WAYLAND_SCANNER wayland-scanner wayland_scanner)
    pkg_get_variable(EACP_WAYLAND_PROTOCOL_DIR wayland-protocols pkgdatadir)

    if (NOT EACP_WAYLAND_SCANNER OR NOT EACP_WAYLAND_PROTOCOL_DIR)
        message(FATAL_ERROR
                "wayland-scanner or the wayland-protocols data directory could "
                "not be located. On Debian/Ubuntu:\n"
                "  sudo apt-get install libwayland-bin wayland-protocols")
    endif ()

    set(eacp_wayland_generated_dir
            "${CMAKE_BINARY_DIR}/generated/eacp-wayland")
    file(MAKE_DIRECTORY "${eacp_wayland_generated_dir}")

    # private-code so the generated interface tables stay static and two
    # libraries carrying a copy cannot collide at link time.
    set(eacp_wayland_protocols
            stable/xdg-shell/xdg-shell.xml
            stable/viewporter/viewporter.xml
            staging/fractional-scale/fractional-scale-v1.xml
            unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
            unstable/relative-pointer/relative-pointer-unstable-v1.xml
            unstable/xdg-output/xdg-output-unstable-v1.xml)

    set(eacp_wayland_generated_sources "")

    foreach (protocol IN LISTS eacp_wayland_protocols)
        cmake_path(GET protocol STEM protocol_name)

        set(xml "${EACP_WAYLAND_PROTOCOL_DIR}/${protocol}")
        set(code "${eacp_wayland_generated_dir}/${protocol_name}-protocol.c")
        set(header
                "${eacp_wayland_generated_dir}/${protocol_name}-client-protocol.h")

        if (NOT EXISTS "${xml}")
            message(FATAL_ERROR
                    "wayland-protocols is installed but does not carry "
                    "${protocol}. Install a newer wayland-protocols.")
        endif ()

        add_custom_command(
                OUTPUT "${code}"
                COMMAND "${EACP_WAYLAND_SCANNER}" private-code "${xml}" "${code}"
                DEPENDS "${xml}"
                COMMENT "wayland-scanner private-code ${protocol_name}"
                VERBATIM)

        add_custom_command(
                OUTPUT "${header}"
                COMMAND "${EACP_WAYLAND_SCANNER}" client-header "${xml}" "${header}"
                DEPENDS "${xml}"
                COMMENT "wayland-scanner client-header ${protocol_name}"
                VERBATIM)

        list(APPEND eacp_wayland_generated_sources "${code}" "${header}")
    endforeach ()

    add_library(eacp-wayland STATIC ${eacp_wayland_generated_sources})

    target_include_directories(eacp-wayland SYSTEM PUBLIC
            "${eacp_wayland_generated_dir}")

    target_link_libraries(eacp-wayland PUBLIC PkgConfig::EACP_WAYLAND)

    # Generated code, held to the generator's standards rather than to eacp's.
    target_compile_options(eacp-wayland PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-w>)

    set_target_properties(eacp-wayland PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            FOLDER "${CMAKE_FOLDER}")
endif ()
