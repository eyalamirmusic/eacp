CPMAddPackage(
        NAME NanoTest
        GITHUB_REPOSITORY eyalamirmusic/NanoTest
        GIT_TAG main)

# Test-only, but hidden for the same reason as every other CPM static lib:
# nothing outside EACP_PLUGIN_EXPORT should ever land in an export table.
