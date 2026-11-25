add_subdirectory(HelloLoop)
add_subdirectory(HelloNetwork)

# A window on a canvas, printing the input its view receives.
if (EACP_HAS_DRAW)
    add_subdirectory(HelloInput)
endif ()
