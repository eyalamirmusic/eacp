CPMAddPackage(
        NAME Miro
        GITHUB_REPOSITORY eyalamirmusic/Miro
        GIT_TAG main)

# Miro's compiled libs link into eacp plugins; keep their symbols out of the
# plugins' export tables. ghc_filesystem / miro_warnings are INTERFACE and
# need nothing.
