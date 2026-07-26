set(CMAKE_FOLDER Examples/Video)

add_subdirectory(PlayingHeavyContent)

# Fetches a public test clip over HTTP and plays it. Needs the network module,
# which the local-file sample does not.
add_subdirectory(DownloadAndPlay)
