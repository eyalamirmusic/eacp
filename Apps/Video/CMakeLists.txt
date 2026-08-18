set(CMAKE_FOLDER Examples/Video)

add_subdirectory(PlayingHeavyContent)

# Fetches a public test clip over HTTP and plays it. Needs the network module,
# which the local-file sample does not.
add_subdirectory(DownloadAndPlay)

# Recording a view and an audio track into one file: the app owns the sound, so
# it pushes blocks in from its own thread while the recorder captures frames.
add_subdirectory(RecordWithAudio)
