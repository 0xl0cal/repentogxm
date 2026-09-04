# Release staging

This directory tracks text only: release notes and checksums. Binaries (VPK,
`eboot.bin`, ELF, VELF, SFO), generated guest code and the PC executable are
excluded from Git; the VPK of each release is attached to the GitHub Releases
page instead.

`RELEASE_NOTES.md` describes the current development line. Add a checksum file
per release once the package has passed
[../RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md).
