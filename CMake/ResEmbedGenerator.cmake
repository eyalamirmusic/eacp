# ResEmbed's ResourceGenerator as a CMake script, so embedding resources needs
# no compiler for the build machine (see FindResEmbed.cmake). It takes the
# arguments res_embed_add passes to `ResourceGenerator generate`, after a `--`,
# and writes the same files: <ns>.h, the <ns>.cpp registry, <ns>_Register.cpp,
# the <ns>_<n>.c byte arrays and the depfile. Only the split layout, which is
# what res_embed_add asks for whenever C is enabled, as it is in eacp.

set(args "")
set(after_dashes FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")

foreach (i RANGE ${last})
    if (after_dashes)
        list(APPEND args "${CMAKE_ARGV${i}}")
    elseif ("${CMAKE_ARGV${i}}" STREQUAL "--")
        set(after_dashes TRUE)
    endif ()
endforeach ()

list(POP_FRONT args command)

if (NOT command STREQUAL "generate")
    message(FATAL_ERROR "ResEmbedGenerator: only `generate` is supported, not `${command}`")
endif ()

# --output-cpp <p> becomes output_cpp, and so on for every flag.
set(category Resources)
set(split_count 0)

while (args)
    list(POP_FRONT args flag value)
    string(REGEX REPLACE "^--" "" flag "${flag}")
    string(REPLACE "-" "_" flag "${flag}")
    set(${flag} "${value}")
endwhile ()

if (split_count LESS 1)
    message(FATAL_ERROR "ResEmbedGenerator: needs --split-count (C enabled)")
endif ()

if (manifest)
    file(STRINGS "${manifest}" files)
else ()
    file(GLOB_RECURSE files LIST_DIRECTORIES false "${scan_dir}/*")
endif ()

list(SORT files)
list(LENGTH files file_count)

# A file left alone when unchanged, so nothing downstream rebuilds. LF on every
# host, where file(WRITE) would write CRLF on Windows; the content is the value
# of one variable, which file(CONFIGURE) substitutes but never scans, and its
# last newline is the one file(CONFIGURE) adds.
function(write_if_changed path content)
    string(REGEX REPLACE "\n$" "" content "${content}")
    file(CONFIGURE OUTPUT "${path}" CONTENT "@content@" @ONLY NEWLINE_STYLE LF)
endfunction()

# The bytes of <file> as a C array <prefix>_data and its size <prefix>_size.
function(c_array file prefix out)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
    string(REPEAT "0x..," 16 line)
    string(REGEX REPLACE "(${line})" "\\1\n    " bytes "${bytes}")

    set(${out} "const unsigned char ${prefix}_data[] = {\n    ${bytes}\n};\n\n\
const unsigned long ${prefix}_size = sizeof(${prefix}_data);\n" PARENT_SCOPE)
endfunction()

write_if_changed("${output_h}" "#pragma once

#include <ResEmbed/ResEmbed.h>
#include <ResEmbed/Entries.h>

namespace ${namespace}
{
const ResEmbed::Entries& getResourceEntries();
}
")

set(externs "")
set(entries "")
set(i 0)

foreach (file IN LISTS files)
    if (base_directory)
        file(RELATIVE_PATH key "${base_directory}" "${file}")
    else ()
        get_filename_component(key "${file}" NAME)
    endif ()

    string(APPEND externs "extern const unsigned char ${namespace}_${i}_data[];\n"
            "extern const unsigned long ${namespace}_${i}_size;\n")

    if (entries)
        string(APPEND entries ",\n")
    endif ()

    string(APPEND entries "        {${namespace}_${i}_data, ${namespace}_${i}_size, "
            "\"${key}\", \"${category}\"}")
    math(EXPR i "${i} + 1")
endforeach ()

write_if_changed("${output_cpp}" "#include \"${namespace}.h\"

extern \"C\"
{
${externs}}

namespace ${namespace}
{
const ResEmbed::Entries& getResourceEntries()
{
    static const ResEmbed::Entries entries = {
${entries}
    };

    return entries;
}
}
")

# Resources round-robin across the buckets, keyed by their overall index so the
# registry's externs line up; an empty bucket still defines a symbol.
get_filename_component(output_dir "${output_cpp}" DIRECTORY)
math(EXPR last_bucket "${split_count} - 1")

foreach (bucket RANGE ${last_bucket})
    set(content "")

    if (bucket LESS file_count)
        foreach (i RANGE ${bucket} ${file_count} ${split_count})
            if (i LESS file_count)
                list(GET files ${i} file)
                c_array("${file}" "${namespace}_${i}" array)

                if (content)
                    string(APPEND content "\n")
                endif ()

                string(APPEND content "${array}")
            endif ()
        endforeach ()
    else ()
        set(content "const unsigned char ${namespace}_${bucket}_empty_tu = 0;\n")
    endif ()

    write_if_changed("${output_dir}/${namespace}_${bucket}.c" "${content}")
endforeach ()

write_if_changed("${output_register}" "#include \"${namespace}.h\"

namespace
{
const ResEmbed::Initializer ${namespace}_resourceInitializer {${namespace}::getResourceEntries()};
}
")

# Written every time: ninja compares the depfile's age with its output's.
if (depfile)
    set(deps "")

    foreach (dep IN ITEMS ${manifest} LISTS files)
        string(REGEX REPLACE "([ \\\\#$])" "\\\\\\1" dep "${dep}")
        string(APPEND deps " \\\n  ${dep}")
    endforeach ()

    string(REGEX REPLACE "([ \\\\#$])" "\\\\\\1" target "${output_cpp}")
    file(REMOVE "${depfile}")
    write_if_changed("${depfile}" "${target}:${deps}\n")
endif ()
