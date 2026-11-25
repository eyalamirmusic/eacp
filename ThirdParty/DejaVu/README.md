# DejaVu fonts

[DejaVu](https://dejavu-fonts.github.io) 2.37: `DejaVuSans.ttf`,
`DejaVuSans-Bold.ttf`, `DejaVuSansMono.ttf` and `DejaVuSansMono-Bold.ttf` from
the upstream `dejavu-fonts-ttf-2.37` release archive, unmodified. Bitstream
Vera license with public-domain DejaVu changes; `LICENSE` is upstream's.

They are the web target's default families, embedded into every web app that
links `eacp-text` while `EACP_WEB_DEFAULT_FONTS` is on (2,135,464 bytes of
font data, `Lib/eacp/Text/CMakeLists.txt`) and registered by
`Lib/eacp/Text/FontDiscovery-Web.cpp`. No other platform builds them in:
desktop Linux finds the installed DejaVu through fontconfig.

To update: replace the four files and `LICENSE` with the next release's, and
change the version above.
