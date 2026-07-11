CPMAddPackage(
        NAME ResEmbed
        GITHUB_REPOSITORY eyalamirmusic/ResEmbed
        GIT_TAG main)

# The runtime lib links into eacp plugins; keep its symbols out of their
# export tables. ResourceGenerator is a host tool, not linked anywhere.
