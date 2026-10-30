include(CPM)

CPMAddPackage(
        NAME ESIMD
        GITHUB_REPOSITORY eyalamirmusic/ESIMD
        GIT_TAG main)

silence_target_warnings(ESIMD)
