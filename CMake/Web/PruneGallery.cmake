# Removes from the gallery folder every file web-gallery no longer puts there -
# an app no longer listed, a .wasm once EACP_WEB_SINGLE_FILE embeds it - and the
# folders that leaves empty, so the folder is exactly the current layout.
#
#   cmake -DGALLERY=<build>/web -DFILES=<list> -P PruneGallery.cmake
#
# <list> holds the gallery's files one per line, relative to it.

file(STRINGS "${FILES}" kept)
file(GLOB_RECURSE present LIST_DIRECTORIES false RELATIVE "${GALLERY}" "${GALLERY}/*")

foreach (file IN LISTS present)
    if (NOT file IN_LIST kept)
        file(REMOVE "${GALLERY}/${file}")
    endif ()
endforeach ()

file(GLOB_RECURSE folders LIST_DIRECTORIES true "${GALLERY}/*")
list(SORT folders ORDER DESCENDING)

foreach (folder IN LISTS folders)
    if (IS_DIRECTORY "${folder}")
        file(GLOB children "${folder}/*")

        if (NOT children)
            file(REMOVE_RECURSE "${folder}")
        endif ()
    endif ()
endforeach ()
