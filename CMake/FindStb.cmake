include(CPM)

# stb_image / stb_image_write (public domain / MIT, single headers): the PNG and
# JPEG codec behind Graphics::Image on desktop Linux, which has no platform
# codec the way macOS (ImageIO), Windows (WIC) and Android (BitmapFactory) do.
# Headers only, so it is fetched and its folder put on the include path; SYSTEM
# keeps its warnings out of ours. The implementations are compiled once, in
# Image-Linux.cpp.
CPMAddPackage(
        NAME stb
        GITHUB_REPOSITORY nothings/stb
        GIT_TAG 4e9b368bfc071e97206ea5c6adba68dd0cbf8897
        DOWNLOAD_ONLY YES)

if (NOT TARGET eacp-stb)
    add_library(eacp-stb INTERFACE)
    target_include_directories(eacp-stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif ()
