# Variable Font

One embedded variable face drawn at the nine CSS weights, beside the
platform's own family cut into faces. What the example is about is in the
header comment of `Main.cpp`; what the font is, is here.

## InterVariable-Subset.ttf

Inter, by The Inter Project Authors — <https://github.com/rsms/inter> — under
the SIL Open Font License 1.1, the full text of which is in `OFL.txt` beside
this file.

It is a **subset made for this example**, not the font as it is published: the
Latin variable face, cut down with
[fonttools](https://github.com/fonttools/fonttools) to the printable ASCII
range (U+0020–U+007E, 95 characters) and the `kern`, `liga` and `calt`
features, which is everything the app draws and about 70 KB rather than 800.
Both of Inter's variation axes are kept — `wght` 100–900 and `opsz` 14–32 —
and its name and copyright records are untouched, so the platform files it
under the family "Inter Variable" and `registerMemoryFont` reports that name
back.

```
pyftsubset inter-latin.ttf --unicodes="U+0020-007E" \
    --layout-features="kern,liga,calt" --name-IDs="*" \
    --output-file=InterVariable-Subset.ttf
```

The file is embedded in the executable by `res_embed_add` and read back with
`ResEmbed::get("InterVariable-Subset.ttf", "VariableFontAssets")`, so the app
installs nothing and reads nothing from disk.
