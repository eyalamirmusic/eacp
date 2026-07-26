# The four demo clips VideoViewDemo plays and Tests/Video decodes:
# Big Buck Bunny / Jellyfish / Sintel, ~25 MB of 1080p + 720p H.264
# (CC-BY; (c) Blender Foundation, and the Jellyfish sample from
# test-videos.co.uk). They are NOT committed (they'd bloat the repo); every
# consumer fetches into the same git-ignored cache directory so the download
# happens once per checkout. EXPECTED_HASH makes the fetch idempotent: CMake
# skips a download when the file already exists and matches.

get_filename_component(EACP_DEMO_CLIPS_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../Apps/Video/VideoViewDemo/media"
        ABSOLUTE)

function(eacp_fetch_demo_clips out_files)
    set(clips
            "heavy.mp4|https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_10MB.mp4|1bb2a62abfcb5c4bf2e9216b63ba715b358f22c9fa8e68ca7149c4788b906826"
            "jellyfish.mp4|https://test-videos.co.uk/vids/jellyfish/mp4/h264/720/Jellyfish_720_10s_5MB.mp4|533d39108871a6d1ff19bc49c6f292ba9046d64f24157be1dad321d39525c6de"
            "sintel.mp4|https://test-videos.co.uk/vids/sintel/mp4/h264/720/Sintel_720_10s_5MB.mp4|52f127a6e77a8544c80ef635f82fc1098615af1a8fc38b2c5c1bd10a740a7ee8"
            "bunny720.mp4|https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/720/Big_Buck_Bunny_720_10s_5MB.mp4|bfb0b4b07b8bb61b707a052e62daec31305a13276fcbebc714f2462f31d96210")

    set(files)

    foreach (clip IN LISTS clips)
        string(REPLACE "|" ";" parts "${clip}")
        list(GET parts 0 name)
        list(GET parts 1 url)
        list(GET parts 2 hash)
        set(file "${EACP_DEMO_CLIPS_DIR}/${name}")

        file(DOWNLOAD
                "${url}"
                "${file}"
                EXPECTED_HASH SHA256=${hash}
                TLS_VERIFY ON
                INACTIVITY_TIMEOUT 60
                STATUS status)
        list(GET status 0 code)

        if (NOT code EQUAL 0)
            list(GET status 1 message)
            file(REMOVE "${file}")
            message(FATAL_ERROR "eacp_fetch_demo_clips: could not fetch ${name} "
                    "(${message}). The clips are a ~25 MB download; check network "
                    "access.")
        endif ()

        list(APPEND files "${file}")
    endforeach ()

    set(${out_files} "${files}" PARENT_SCOPE)
endfunction()
