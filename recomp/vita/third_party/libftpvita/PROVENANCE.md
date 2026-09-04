# libftpvita provenance

The Isaac manager FTP server is a reduced, altered adaptation of the network,
passive-transfer and shutdown design in `xerpi/libftpvita`. It does not vendor
the upstream tree and does not use `lpp-vita` code.

- upstream: <https://github.com/xerpi/libftpvita>
- pinned commit: `77a1d39c1d6f11a55fe65a6ed9fe707bff5ede4b`
- upstream commit date: 2020-01-23
- upstream Git blob `libftpvita/ftpvita.c` SHA-256:
  `17b5b8ace802fe3d798752b54770ae7b5614afe13650142da41a8299e805b7e6`
- upstream Git blob `libftpvita/ftpvita.h` SHA-256:
  `905d0410b8e51a08ab231fba7a7202e561df9480f6b176379e625ac6a7483e1b`
- upstream Git blob `LICENSE` SHA-256:
  `4cae2f746d85eb1cd20e12eb705fff7461e7f0eb46e0d3414750480a8f7d43fe`
- tracked byte-identical `LICENSE` SHA-256:
  `4cae2f746d85eb1cd20e12eb705fff7461e7f0eb46e0d3414750480a8f7d43fe`
- licence: MIT; the exact upstream licence is retained as `LICENSE` and is
  packaged as `app0:/licenses/libftpvita.txt`.

Upstream source headers also say `Copyright (c) 2015-2016 Sergi Granell
(xerpi)`; the repository licence says `Copyright (c) 2019 Sergi Granell`.
Both notices are retained here.

Project changes are intentionally security-relevant: there is one fixed Vita
root (`ux0:/data/isaacr001`), the FTP-visible root is virtual, every filesystem
command crosses one lexical canonicalizer, other device-qualified paths and
dot segments are rejected, the mapped root cannot be renamed or removed, active
FTP mode and custom commands are omitted, and stop joins the worker before
network teardown or LoadExec. This is a lexical restriction, not a general
filesystem sandbox: a root containing pre-existing native symbolic links or
mount aliases is outside the supported interface.

Rinnegatamante's `lpp-vita` manager/error screen at pinned review commit
`c3ef6ad2c3aa6700ac6a338aa8a1c1cc9361e72e` was used only as workflow prior
art for a user-controlled FTP toggle and endpoint display. `lpp-vita` is
GPL-3.0; none of its source is included in this adaptation.
