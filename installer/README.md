# Anchor multi-board browser installer

`generate-site.py` builds board-specific ESP Web Tools manifests from validated
production and debug CI images. The board selector is populated from the
supported board profiles in `boards/<board>.mk` and `site/boards.json`.

Supported selections are currently XIAO ESP32-C5 (`c5`, 8 MB) and ESP32-C3
Super Mini (`c3-supermini`, 4 MB). The C5 is the reference board; the C3 is
compile-tested only and the installer shows a warning before flashing.

Each board/profile has its own full-image manifest at
`<board>/<profile>/manifest.json`, with the correct ESP chip family and
flash length. The installer uses USB ROM flashing, not UF2. Wi-Fi OTA on
an already installed device accepts application-only firmware, never the
merged/full image.

The GitHub Actions matrix builds both boards and both profiles, assembles the
four verified images, and publishes one Pages site. Refactor branches build
and validate the site artifact without altering live GitHub Pages. Main-branch
runs deploy the installer automatically when Pages is enabled.

Local packaging example after all four builds:

```sh
python3 installer/generate-site.py --dist dist --output site --version local-test --boards c5 c3-supermini
```

Use a Chromium browser with Web Serial. Choose the **exact hardware board**
before clicking Install. Do not flash a C5 full image onto a C3 or vice versa.
