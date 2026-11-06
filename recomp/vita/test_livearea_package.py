#!/usr/bin/env python3
"""Pin the private Vita package presentation assets and VPK mappings."""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import struct
import tempfile
import xml.etree.ElementTree as ET
import zipfile
import zlib


HERE = Path(__file__).resolve().parent
ASSET_ROOT = HERE / "assets" / "sce_sys"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
IHDR = struct.Struct(">IIBBBBB")
EXPECTED_MANIFEST_SHA256 = "2edd50a37a58e73bf2347a0610bdff5dbda7c4150c30a39b1a3f8ca7c8e167fb"
MANAGER_MEMBER = "isaac-manager.bin"
MANAGER_LICENSE_MEMBER = "licenses/libftpvita.txt"
MANAGER_LICENSE = HERE / "third_party" / "libftpvita" / "LICENSE"
MANAGER_LICENSE_SHA256 = "4cae2f746d85eb1cd20e12eb705fff7461e7f0eb46e0d3414750480a8f7d43fe"
# Manager UI2 (ISAAC_VITA_MANAGER_UI2):
# three DejaVu IMF1 atlases plus the DejaVu licence, in the VPK only when
# the knob is ON.  The atlas bytes are pinned by
# tools/test_manager_font_bake.py (fonts.manifest.json); this test pins the
# mapping, the magic and the licence.
MANAGER_UI2_FONT_MEMBERS = (
    "manager/font-title-24.imf",
    "manager/font-body-16.imf",
    "manager/font-small-12.imf",
)
MANAGER_UI2_LICENSE_MEMBER = "licenses/dejavu.txt"
MANAGER_UI2_LICENSE = HERE / "third_party" / "dejavu" / "LICENSE"
MANAGER_UI2_LICENSE_SHA256 = "d75938dec098f06f0ac3c00853065d94f020be1c3c62ef1dc2975ba15b4d9b0e"
MANAGER_UI2_MEMBERS = MANAGER_UI2_FONT_MEMBERS + (MANAGER_UI2_LICENSE_MEMBER,)


@dataclass(frozen=True)
class Asset:
    relative: str
    member: str
    size: int
    sha256: str
    ihdr: tuple[int, int, int, int, int, int, int] | None = None


ASSETS = (
    Asset(
        "icon0.png",
        "sce_sys/icon0.png",
        8_180,
        "bbd840ceb8b3270e196b91aa6d4a6acd1fca056a96e164906e492c0e44e21151",
        (128, 128, 8, 3, 0, 0, 0),
    ),
    Asset(
        "pic0.png",
        "sce_sys/pic0.png",
        208_071,
        "c7209f10d8927fefc001050a62f11d091c3bcf412558a89dc3587777c7e9c215",
        (960, 544, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/bg0.png",
        "sce_sys/livearea/contents/bg0.png",
        169_345,
        "cff8dc535bad6f3e0f34e11c78488f1146b4e38c9f1e8d217f63e5278fc24661",
        (840, 500, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/nicalis.png",
        "sce_sys/livearea/contents/nicalis.png",
        4_060,
        "4836922217e21cc29f16b97f8a05567316b70e4d203c7084eba4145af6b112ec",
        (281, 40, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/startup.png",
        "sce_sys/livearea/contents/startup.png",
        7_258,
        "ca275da960367078055b0996a11ac9701a85c4cdf64d4aeab8da416fca9db219",
        (532, 302, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/1.png",
        "sce_sys/livearea/contents/1.png",
        2_739,
        "4f986ba1c75ee358bf8c7b0b652c9bbd0ec70da927a0fd51e65c134f70381060",
        (532, 68, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/2.png",
        "sce_sys/livearea/contents/2.png",
        4_946,
        "2dd3d09566861ce322395093bf3324939f6825165ce64b6ae3a4a499c2d05b3e",
        (532, 68, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/3.png",
        "sce_sys/livearea/contents/3.png",
        5_344,
        "227b9694bc4bff0bd86d2259bf4fd64c147ba3708e84a9007abff0cfeaee36be",
        (532, 68, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/4.png",
        "sce_sys/livearea/contents/4.png",
        3_022,
        "016ca78d4b9b676ca8c6d05594ca7569a28c36dab7e91e4220c4586256f8fbdf",
        (532, 68, 8, 3, 0, 0, 0),
    ),
    Asset(
        "livearea/contents/template.xml",
        "sce_sys/livearea/contents/template.xml",
        1_378,
        "b14122f0a7cecfd36b8b6304086ca1b0eebadc9551fcb167dd7962cb70da36e8",
    ),
)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def payload(asset: Asset) -> bytes:
    path = ASSET_ROOT / Path(*asset.relative.split("/"))
    check(path.is_file() and not path.is_symlink(), f"missing regular asset: {path}")
    data = path.read_bytes()
    check(len(data) == asset.size, f"size changed for {asset.relative}")
    actual = hashlib.sha256(data).hexdigest()
    check(actual == asset.sha256, f"SHA-256 changed for {asset.relative}: {actual}")
    return data


def png_ihdr(asset: Asset, data: bytes) -> None:
    check(asset.ihdr is not None, f"missing expected IHDR for {asset.relative}")
    check(data.startswith(PNG_SIGNATURE), f"bad PNG signature: {asset.relative}")
    offset = len(PNG_SIGNATURE)
    chunks: list[bytes] = []
    actual_ihdr: tuple[int, int, int, int, int, int, int] | None = None
    while offset < len(data):
        check(offset + 12 <= len(data), f"truncated PNG chunk: {asset.relative}")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body_start = offset + 8
        body_end = body_start + length
        crc_end = body_end + 4
        check(crc_end <= len(data), f"oversized PNG chunk: {asset.relative}")
        expected_crc = struct.unpack_from(">I", data, body_end)[0]
        actual_crc = zlib.crc32(kind)
        actual_crc = zlib.crc32(data[body_start:body_end], actual_crc) & 0xFFFFFFFF
        check(
            actual_crc == expected_crc,
            f"bad {kind!r} CRC in {asset.relative}",
        )
        chunks.append(kind)
        if kind == b"IHDR":
            check(actual_ihdr is None and length == IHDR.size, "invalid IHDR layout")
            actual_ihdr = IHDR.unpack(data[body_start:body_end])
        offset = crc_end
        if kind == b"IEND":
            check(length == 0, f"non-empty IEND: {asset.relative}")
            break
    check(offset == len(data), f"trailing PNG bytes: {asset.relative}")
    check(chunks and chunks[0] == b"IHDR", f"IHDR is not first: {asset.relative}")
    check(chunks[-1] == b"IEND", f"IEND is not last: {asset.relative}")
    check(actual_ihdr == asset.ihdr, f"IHDR changed for {asset.relative}: {actual_ihdr}")


def xml_contract(data: bytes) -> None:
    """Pin the community LiveArea (GitHub issue 9, second sce_sys.zip) plus the
    gate startup image the contributor drew at the 532x302 gate size but did not
    link; the four autoflip frames all launch the game, frame2 keeps the manager
    button, frame3 keeps the Nicalis mark."""
    root = ET.fromstring(data)
    check(root.tag == "livearea", "LiveArea root changed")
    check(
        root.attrib == {
            "style": "ad2",
            "format-ver": "01.00",
            "content-rev": "4",
        },
        f"LiveArea attributes changed: {root.attrib!r}",
    )
    check(
        [child.tag for child in root]
        == ["livearea-background", "gate"] + ["frame"] * 6,
        "LiveArea top-level layout changed",
    )
    check(root.findtext("./livearea-background/image") == "bg0.png", "bg0 link changed")
    check(root.findtext("./gate/startup-image") == "startup.png", "gate link changed")
    frames = root.findall("./frame")
    check(
        [frame.attrib for frame in frames]
        == [
            {"id": "frame4", "multi": "0", "autoflip": "1"},
            {"id": "frame5", "multi": "0", "autoflip": "1"},
            {"id": "frame6", "multi": "0", "autoflip": "1"},
            {"id": "frame7", "multi": "0", "autoflip": "1"},
            {"id": "frame2"},
            {"id": "frame3"},
        ],
        "LiveArea frame sequence changed",
    )
    for index, frame_id in enumerate(("frame4", "frame5", "frame6", "frame7"), start=1):
        check(
            root.findtext(f"./frame[@id='{frame_id}']/liveitem/target") == "psla:play",
            f"{frame_id} must launch the game",
        )
        image = root.find(f"./frame[@id='{frame_id}']/liveitem/image")
        check(
            image is not None
            and image.attrib == {"y": "0", "width": "532", "height": "68"}
            and image.text == f"{index}.png",
            f"{frame_id} title strip changed",
        )

    frame2_text = root.find("./frame[@id='frame2']/liveitem/text")
    check(
        frame2_text is not None
        and frame2_text.attrib
        == {
            "align": "center",
            "text-align": "left",
            "word-wrap": "off",
            "ellipsis": "on",
        },
        "manager line placement changed",
    )
    frame2_str = root.find("./frame[@id='frame2']/liveitem/text/str")
    check(
        frame2_str is not None
        and frame2_str.attrib
        == {"size": "18", "color": "#ffffff", "shadow": "on"}
        and frame2_str.text == "SAVE / MOD MANAGER",
        "manager line styling changed",
    )
    check(
        root.findtext("./frame[@id='frame2']/liveitem/target")
        == "psla:-manager",
        "manager LiveArea target changed",
    )

    image = root.find("./frame[@id='frame3']/liveitem/image")
    check(image is not None and image.text == "nicalis.png", "Nicalis image link changed")
    check(
        image is not None
        and image.attrib
        == {"align": "center", "valign": "center", "width": "196", "height": "28"},
        "Nicalis placement changed",
    )


def cmake_contract() -> None:
    text = (HERE / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"set\(ISAAC_VPK_LIVEAREA_FILES(?P<body>.*?)\)\s*\n",
        text,
        flags=re.DOTALL,
    )
    check(match is not None, "missing ISAAC_VPK_LIVEAREA_FILES")
    assert match is not None
    pairs = re.findall(
        r'FILE\s+"\$\{ISAAC_VITA_PACKAGE_ASSETS_DIR\}/([^"]+)"\s+"([^"]+)"',
        match.group("body"),
    )
    expected = [(asset.relative, asset.member) for asset in ASSETS]
    check(pairs == expected, f"CMake LiveArea mappings changed: {pairs!r}")
    check(
        text.count("${ISAAC_VPK_LIVEAREA_FILES}") == 2,
        "LiveArea assets must be mapped into both VPK branches exactly once",
    )
    check(
        text.count("${ISAAC_VPK_MANAGER_FILE}") == 2,
        "manager SELF must be mapped into both VPK branches exactly once",
    )
    check(
        text.count("${ISAAC_VPK_MANAGER_LICENSE_FILE}") == 2,
        "manager FTP licence must be mapped into both VPK branches exactly once",
    )
    check(
        text.count(
            'FILE "${CMAKE_CURRENT_BINARY_DIR}/isaac-manager.bin"\n'
            '       "isaac-manager.bin"'
        ) == 1,
        "manager VPK mapping changed",
    )
    check(
        text.count(
            'FILE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libftpvita/LICENSE"\n'
            '       "licenses/libftpvita.txt"'
        ) == 1,
        "manager FTP licence VPK mapping changed",
    )
    required_manager_build = (
        "add_executable(isaac_manager\n  manager_main.c\n"
        "  manager_ftp_lifecycle.c\n  manager_ftp_path.c\n"
        "  manager_ftp_protocol.c\n"
        "  manager_ftp_server.c\n"
        "  manager_sha256.c\n"
        "  manager_storage.c\n"
        "  manager_storage_vita.c)",
        "vita_create_self(isaac-manager.bin isaac_manager UNSAFE)",
        "target_link_libraries(isaac_manager PRIVATE",
        "SceDisplay_stub",
        "SceCtrl_stub",
        "SceAppMgr_stub",
        "SceIofilemgr_stub",
        "SceNet_stub",
        "SceNetCtl_stub",
        "SceSysmodule_stub",
    )
    check(
        all(token in text for token in required_manager_build),
        "native manager build contract changed",
    )
    check(
        'set(ISAAC_APP_NAME "The Binding of Isaac: Repentance")' in text,
        "production APP_NAME changed",
    )
    check(
        'set(ISAAC_APP_VERSION "01.01")' in text,
        "production APP_VERSION changed",
    )
    check(
        text.count("VERSION ${ISAAC_APP_VERSION}") == 2,
        "APP_VERSION must feed both VPK branches exactly once",
    )
    check(
        'set(ISAAC_VPK_NAME "the-binding-of-isaac-repentance")' in text,
        "production VPK name changed",
    )
    ui2_begin = text.find("if(ISAAC_VITA_MANAGER_UI2)\n")
    check(ui2_begin >= 0, "missing if(ISAAC_VITA_MANAGER_UI2) block")
    ui2_block = text[ui2_begin:text.index("\nendif()\n", ui2_begin)]
    check(
        text.count("SceAppUtil_stub") == 1 + ui2_block.count("SceAppUtil_stub")
        and ui2_block.count("SceAppUtil_stub") == 1,
        "LiveArea dispatch must link exactly one SceAppUtil stub "
        "(plus the manager UI2 system-language one)",
    )
    manager_ui2_contract(text, ui2_block)
    main_source = (HERE / "main.c").read_text(encoding="utf-8")
    required_dispatch = (
        "sceAppUtilInit",
        "sceAppUtilReceiveAppEvent",
        "sceAppUtilAppEventParseLiveArea",
        'strstr(target, "-manager")',
        'sceAppMgrLoadExec("app0:/isaac-manager.bin", NULL, NULL)',
        "sceAppUtilShutdown",
    )
    check(
        all(main_source.count(token) == 1 for token in required_dispatch),
        "LiveArea manager dispatch contract changed",
    )
    check(
        main_source.index("isaac_vita_livearea_dispatch();")
        < main_source.index("isaac_vita_log_reset();"),
        "LiveArea dispatch must run before game/log initialization",
    )
    manager_source = (HERE / "manager_main.c").read_text(encoding="utf-8")
    required_manager_source = (
        "SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW",
        "sceDisplaySetFrameBuf",
        "sceCtrlPeekBufferPositive",
        'sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL)',
        '"STATUS: NATIVE MANAGER RUNNING"',
        '"FTP: LISTENING"',
        '"FTP: STOPPED"',
        '"O  TOGGLE FTP     X  RETURN TO GAME"',
        '"SELECT: GAME CONTROLS"',
        '"SELECT TAP: SWAP HELD SLOTS"',
        '"SELECT HOLD: DROP POCKET OR TRINKET"',
        '"ISAAC DECIDES SWAP CONTEXT"',
        "MANAGER_PAGE_CONTROLS",
        "(pressed & SCE_CTRL_SELECT)",
        "manager_ftp_server_start",
        "manager_ftp_server_stop",
        "manager_storage_list_mods",
        "manager_storage_set_mod_enabled",
        "manager_storage_list_save_backups",
        "manager_storage_backup_slot",
        "manager_storage_restore_backup",
        '"PRESS X AGAIN TO RESTORE SLOT %d"',
    )
    check(
        all(token in manager_source for token in required_manager_source),
        "native manager behavior contract changed",
    )
    check(
        "NOT IMPLEMENTED YET" not in manager_source,
        "implemented manager features must not be labelled unfinished",
    )
    load_exec_index = manager_source.index(
        'sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL)'
    )
    check(
        manager_source.rfind("manager_ftp_server_stop();", 0, load_exec_index) >= 0,
        "manager FTP must stop immediately before returning to the game",
    )
    server_source = (HERE / "manager_ftp_server.c").read_text(encoding="utf-8")
    path_source = (HERE / "manager_ftp_path.h").read_text(encoding="utf-8")
    check(
        '#define MANAGER_FTP_VITA_ROOT "ux0:/data/isaacr001"' in path_source,
        "manager FTP mapped root changed",
    )
    check(
        "ftpvita_add_device" not in server_source
        and '"app0:' not in server_source
        and '"ur0:' not in server_source,
        "manager FTP server gained an external device path",
    )
    required_lifecycle = (
        "#include <psp2/io/stat.h>",
        "sceKernelCreateMutex(",
        "manager_ftp_lifecycle_publish(",
        "manager_ftp_lifecycle_begin_stop(",
        "sceNetShutdown(",
        "sceNetSocketAbort(",
        "sceKernelWaitThreadEnd(",
    )
    check(
        all(token in server_source for token in required_lifecycle)
        and "volatile int stop_requested" not in server_source,
        "manager FTP stop/publication lifecycle contract changed",
    )
    net_module = server_source.index(
        "sceSysmoduleLoadModule(SCE_SYSMODULE_NET)"
    )
    net_init = server_source.index("sceNetInit(&parameters)", net_module)
    netctl_init = server_source.index("sceNetCtlInit()", net_init)
    check(
        net_module < net_init < netctl_init,
        "manager network initialization order changed",
    )
    build_wrapper = (HERE.parent.parent / "tools" / "build_vita.py").read_text(
        encoding="utf-8"
    )
    wrapper_tree = ast.parse(build_wrapper)
    wrapper_contracts = [
        ast.literal_eval(node.value)
        for node in wrapper_tree.body
        if isinstance(node, ast.Assign)
        and any(
            isinstance(target, ast.Name) and target.id == "VPK_LIVEAREA_ASSETS"
            for target in node.targets
        )
    ]
    expected_wrapper_contract = tuple(
        (asset.member, asset.size, asset.sha256) for asset in ASSETS
    )
    check(
        wrapper_contracts == [expected_wrapper_contract],
        f"build wrapper LiveArea contract changed: {wrapper_contracts!r}",
    )
    check(
        f'VPK_MANAGER_MEMBER = "{MANAGER_MEMBER}"' in build_wrapper,
        "build wrapper manager member contract changed",
    )
    check(
        f'VPK_MANAGER_LICENSE_MEMBER = "{MANAGER_LICENSE_MEMBER}"' in build_wrapper,
        "build wrapper manager FTP licence contract changed",
    )
    check(
        build_wrapper.count('"the-binding-of-isaac-repentance.vpk"') == 2,
        "build wrapper does not track the production VPK name exactly twice",
    )
    check(
        '"isaac-first-arm-fault.vpk"' not in build_wrapper,
        "build wrapper still accepts the diagnostic VPK name",
    )


def manager_ui2_contract(text: str, block: str) -> None:
    """The manager UI2 asset list is empty unless the knob is ON, is mapped
    into both VPK branches, and names exactly the three atlases and the
    DejaVu licence."""
    check(
        text.count("set(ISAAC_VPK_MANAGER_ASSET_FILES)\n") == 1
        and text.index("set(ISAAC_VPK_MANAGER_ASSET_FILES)\n")
        < text.index("if(ISAAC_VITA_MANAGER_UI2)\n"),
        "manager UI2 asset list must start empty before its if()",
    )
    check(
        text.count("${ISAAC_VPK_MANAGER_ASSET_FILES}") == 2,
        "manager UI2 assets must be mapped into both VPK branches exactly once",
    )
    check(
        text.count("set(ISAAC_VPK_MANAGER_ASSET_FILES") == 2
        and block.count("set(ISAAC_VPK_MANAGER_ASSET_FILES\n") == 1,
        "manager UI2 asset list must be filled only inside its if()",
    )
    pairs = re.findall(
        r'FILE\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"]+)"\s+"([^"]+)"',
        block,
    )
    expected = [
        (f"assets/{member}", member) for member in MANAGER_UI2_FONT_MEMBERS
    ] + [("third_party/dejavu/LICENSE", MANAGER_UI2_LICENSE_MEMBER)]
    check(pairs == expected, f"manager UI2 VPK mappings changed: {pairs!r}")
    for member in MANAGER_UI2_FONT_MEMBERS:
        atlas = HERE / "assets" / member
        check(atlas.is_file(), f"manager UI2 atlas missing: {atlas}")
        check(
            atlas.read_bytes()[:4] == b"IMF1",
            f"manager UI2 atlas is not IMF1: {atlas}",
        )


def manager_ui2_vpk_contract(archive: zipfile.ZipFile, names: list[str]) -> None:
    """OFF packages carry none of the UI2 members, ON packages all four."""
    present = [member for member in MANAGER_UI2_MEMBERS if member in names]
    check(
        not present or present == list(MANAGER_UI2_MEMBERS),
        f"VPK carries a partial manager UI2 asset set: {present!r}",
    )
    check(
        all(names.count(member) <= 1 for member in MANAGER_UI2_MEMBERS),
        "VPK duplicates a manager UI2 member",
    )
    stray = [
        name for name in names
        if name.startswith("manager/") and name not in MANAGER_UI2_MEMBERS
    ]
    check(not stray, f"VPK carries unknown manager/ members: {stray!r}")
    if not present:
        return
    for member in MANAGER_UI2_FONT_MEMBERS:
        check(
            archive.read(member)
            == (HERE / "assets" / member).read_bytes(),
            f"packaged manager UI2 atlas changed: {member}",
        )
    check(
        hashlib.sha256(archive.read(MANAGER_UI2_LICENSE_MEMBER)).hexdigest()
        == MANAGER_UI2_LICENSE_SHA256,
        "packaged DejaVu licence changed",
    )


def manifest_digest() -> str:
    manifest = b"".join(
        f"{asset.member}\0{asset.size}\0{asset.sha256}\n".encode("ascii")
        for asset in ASSETS
    )
    return hashlib.sha256(manifest).hexdigest()


def vpk_contract(path: Path) -> None:
    with zipfile.ZipFile(path, "r") as archive:
        names = [info.filename for info in archive.infolist()]
        expected_names = [asset.member for asset in ASSETS]
        positions: list[int] = []
        for asset in ASSETS:
            check(names.count(asset.member) == 1, f"VPK member count changed: {asset.member}")
            positions.append(names.index(asset.member))
            actual = hashlib.sha256(archive.read(asset.member)).hexdigest()
            check(actual == asset.sha256, f"VPK payload changed: {asset.member}")
        check(positions == sorted(positions), f"VPK LiveArea member order changed: {names!r}")
        check(
            [names[index] for index in positions] == expected_names,
            "VPK LiveArea member sequence changed",
        )
        check(
            names.count(MANAGER_MEMBER) == 1,
            "VPK must contain exactly one manager SELF",
        )
        check(
            archive.read(MANAGER_MEMBER).startswith(b"SCE\x00"),
            "packaged manager is not a Vita SELF",
        )
        check(
            names.count(MANAGER_LICENSE_MEMBER) == 1,
            "VPK must contain exactly one libftpvita MIT licence",
        )
        check(
            hashlib.sha256(archive.read(MANAGER_LICENSE_MEMBER)).hexdigest()
            == MANAGER_LICENSE_SHA256,
            "packaged libftpvita MIT licence changed",
        )
        manager_ui2_vpk_contract(archive, names)


def synthetic_vpk_contract() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "manager-package.vpk"
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
            for asset in ASSETS:
                archive.writestr(asset.member, payload(asset))
            archive.writestr(MANAGER_MEMBER, b"SCE\x00manager-fixture")
            archive.writestr(MANAGER_LICENSE_MEMBER, MANAGER_LICENSE.read_bytes())
        vpk_contract(path)

        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
            for asset in ASSETS:
                archive.writestr(asset.member, payload(asset))
            archive.writestr(MANAGER_MEMBER, b"SCE\x00manager-fixture")
            archive.writestr(MANAGER_LICENSE_MEMBER, b"mutated licence\n")
        try:
            vpk_contract(path)
        except AssertionError as exc:
            check(
                "libftpvita MIT licence changed" in str(exc),
                f"wrong mutated-licence failure: {exc}",
            )
        else:
            raise AssertionError("mutated libftpvita MIT licence was accepted")

        def write_ui2(members: tuple[str, ...], mutate: str | None = None) -> None:
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
                for asset in ASSETS:
                    archive.writestr(asset.member, payload(asset))
                archive.writestr(MANAGER_MEMBER, b"SCE\x00manager-fixture")
                archive.writestr(MANAGER_LICENSE_MEMBER, MANAGER_LICENSE.read_bytes())
                for member in members:
                    if member == MANAGER_UI2_LICENSE_MEMBER:
                        data = MANAGER_UI2_LICENSE.read_bytes()
                    elif member in MANAGER_UI2_FONT_MEMBERS:
                        data = (HERE / "assets" / member).read_bytes()
                    else:
                        data = b"IMF1stray-fixture"
                    if member == mutate:
                        data += b"mutated"
                    archive.writestr(member, data)

        write_ui2(MANAGER_UI2_MEMBERS)
        vpk_contract(path)
        for members, mutate, expected in (
                (MANAGER_UI2_FONT_MEMBERS, None, "partial manager UI2 asset set"),
                (MANAGER_UI2_MEMBERS, MANAGER_UI2_LICENSE_MEMBER,
                 "packaged DejaVu licence changed"),
                (MANAGER_UI2_MEMBERS, "manager/font-small-12.imf",
                 "packaged manager UI2 atlas changed"),
                (MANAGER_UI2_MEMBERS + ("manager/extra.imf",), None,
                 "unknown manager/ members"),
        ):
            write_ui2(members, mutate)
            try:
                vpk_contract(path)
            except AssertionError as exc:
                check(expected in str(exc), f"wrong manager UI2 failure: {exc}")
            else:
                raise AssertionError(f"manager UI2 fixture accepted: {expected}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--vpk", type=Path)
    args = parser.parse_args()
    for asset in ASSETS:
        data = payload(asset)
        if asset.ihdr is not None:
            png_ihdr(asset, data)
        else:
            xml_contract(data)
    manager_license = MANAGER_LICENSE.read_bytes()
    check(
        hashlib.sha256(manager_license).hexdigest() == MANAGER_LICENSE_SHA256,
        "tracked libftpvita MIT licence changed",
    )
    check(
        hashlib.sha256(MANAGER_UI2_LICENSE.read_bytes()).hexdigest()
        == MANAGER_UI2_LICENSE_SHA256,
        "tracked DejaVu licence changed",
    )
    synthetic_vpk_contract()
    cmake_contract()
    digest = manifest_digest()
    check(
        digest == EXPECTED_MANIFEST_SHA256,
        f"member manifest changed: {digest}",
    )
    if args.vpk is not None:
        vpk_contract(args.vpk.resolve())
    print(f"LiveArea package: PASS; members={len(ASSETS)} manifest={digest}")


if __name__ == "__main__":
    main()
