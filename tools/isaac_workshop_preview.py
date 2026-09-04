#!/usr/bin/env python3
"""Bounded Steam Workshop metadata and preview cache for the PC manager."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import re
import stat
from typing import Callable, Iterable
import urllib.parse
import urllib.request
import uuid


DETAILS_URL = (
    "https://api.steampowered.com/"
    "ISteamRemoteStorage/GetPublishedFileDetails/v1/"
)
APP_ID = "250900"
MAX_BATCH = 100
MAX_DETAILS_BYTES = 2 * 1024 * 1024
MAX_PREVIEW_BYTES = 5 * 1024 * 1024
MAX_CACHE_JSON_BYTES = 64 * 1024
ALLOWED_PREVIEW_HOSTS = frozenset(
    {
        "images.steamusercontent.com",
        "steamuserimages-a.akamaihd.net",
    }
)
ITEM_RE = re.compile(r"[1-9][0-9]{0,19}\Z")
PREVIEW_FILE_RE = re.compile(r"preview\.(?:gif|jpg|png|webp)\Z")
UrlOpen = Callable[..., object]


class WorkshopPreviewError(ValueError):
    pass


@dataclass(frozen=True)
class WorkshopDetails:
    workshop_id: str
    title: str
    preview_url: str


@dataclass(frozen=True)
class WorkshopCacheEntry:
    workshop_id: str
    title: str
    preview_url: str
    preview_path: Path | None


def default_cache_root() -> Path:
    base = os.environ.get("LOCALAPPDATA", "").strip()
    if base:
        return Path(base) / "Isaac Vita Sync" / "Workshop Cache"
    return Path.home() / ".cache" / "isaac-vita-sync" / "workshop"


def _item_id(value: object) -> str:
    item = str(value)
    if not ITEM_RE.fullmatch(item):
        raise WorkshopPreviewError("invalid Workshop item ID")
    return item


def _clean_title(value: object, item: str) -> str:
    title = " ".join(str(value).split())
    title = "".join(ch for ch in title if ch >= " " and ch != "\x7f")[:120]
    return title or f"Workshop mod {item}"


def _read_bounded(response: object, limit: int) -> bytes:
    headers = getattr(response, "headers", {})
    raw_length = headers.get("Content-Length") if hasattr(headers, "get") else None
    if raw_length:
        try:
            length = int(raw_length, 10)
        except (TypeError, ValueError) as exc:
            raise WorkshopPreviewError("invalid HTTP Content-Length") from exc
        if length < 0 or length > limit:
            raise WorkshopPreviewError("HTTP response exceeds size limit")
    data = response.read(limit + 1)
    if len(data) > limit:
        raise WorkshopPreviewError("HTTP response exceeds size limit")
    return data


def _response_ok(response: object) -> None:
    status = int(getattr(response, "status", 200))
    if status != 200:
        raise WorkshopPreviewError(f"HTTP request failed with status {status}")


def _open(opener: UrlOpen, request: urllib.request.Request):
    return opener(request, timeout=8.0)


def query_workshop_details(
    item_ids: Iterable[str], *, opener: UrlOpen = urllib.request.urlopen
) -> dict[str, WorkshopDetails]:
    """Query public metadata only for the caller's already-local item IDs."""

    requested = sorted({_item_id(item) for item in item_ids}, key=int)
    found: dict[str, WorkshopDetails] = {}
    for offset in range(0, len(requested), MAX_BATCH):
        batch = requested[offset : offset + MAX_BATCH]
        fields: list[tuple[str, str]] = [("itemcount", str(len(batch)))]
        fields.extend((f"publishedfileids[{i}]", item) for i, item in enumerate(batch))
        request = urllib.request.Request(
            DETAILS_URL,
            data=urllib.parse.urlencode(fields).encode("ascii"),
            method="POST",
            headers={
                "Accept": "application/json",
                "Content-Type": "application/x-www-form-urlencoded",
                "User-Agent": "Isaac-Vita-Sync/preview-cache",
            },
        )
        with _open(opener, request) as response:
            _response_ok(response)
            payload = _read_bounded(response, MAX_DETAILS_BYTES)
        try:
            document = json.loads(payload.decode("utf-8"))
            details = document["response"]["publishedfiledetails"]
        except (KeyError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise WorkshopPreviewError("malformed Steam Workshop response") from exc
        if not isinstance(details, list):
            raise WorkshopPreviewError("malformed Steam Workshop detail list")
        allowed = set(batch)
        for row in details:
            if not isinstance(row, dict):
                continue
            item = str(row.get("publishedfileid", ""))
            if item not in allowed or row.get("result") != 1:
                continue
            if str(row.get("consumer_app_id", "")) != APP_ID:
                continue
            title = _clean_title(row.get("title", ""), item)
            preview_url = str(row.get("preview_url", ""))
            if preview_url and not preview_url_allowed(preview_url):
                preview_url = ""
            found[item] = WorkshopDetails(item, title, preview_url)
    return found


def preview_url_allowed(value: str) -> bool:
    try:
        parsed = urllib.parse.urlsplit(value)
        port = parsed.port
    except ValueError:
        return False
    return (
        parsed.scheme == "https"
        and parsed.hostname in ALLOWED_PREVIEW_HOSTS
        and port in (None, 443)
        and not parsed.username
        and not parsed.password
        and parsed.path.startswith("/")
    )


def _validated_redirect(current_url: str, target_url: str) -> str:
    target = urllib.parse.urljoin(current_url, target_url)
    if not preview_url_allowed(target):
        raise WorkshopPreviewError("Workshop preview redirect left Steam CDN")
    return target


class _SteamPreviewRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        target = _validated_redirect(request.full_url, new_url)
        return super().redirect_request(
            request, fp, code, message, headers, target
        )


def _image_suffix(data: bytes) -> str:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if data.startswith((b"GIF87a", b"GIF89a")):
        return ".gif"
    if data.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return ".webp"
    raise WorkshopPreviewError("Workshop preview is not a supported image")


def _validate_image(data: bytes) -> None:
    try:
        from PIL import Image
    except ImportError as exc:
        raise WorkshopPreviewError("Pillow is required for Workshop previews") from exc
    try:
        with Image.open(io.BytesIO(data)) as image:
            width, height = image.size
            if (
                width < 1
                or height < 1
                or width > 4096
                or height > 4096
                or width * height > 4_000_000
            ):
                raise WorkshopPreviewError(
                    "Workshop preview dimensions exceed safety limit"
                )
            image.verify()
    except (OSError, ValueError, Image.DecompressionBombError) as exc:
        raise WorkshopPreviewError("Workshop preview image is corrupt") from exc


def _is_link_or_reparse(path: Path) -> bool:
    try:
        info = os.lstat(path)
    except OSError:
        return False
    attributes = int(getattr(info, "st_file_attributes", 0))
    reparse_flag = int(getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))
    return stat.S_ISLNK(info.st_mode) or bool(attributes & reparse_flag)


def _safe_item_dir(root: Path, item: str, *, create: bool) -> Path:
    item = _item_id(item)
    root = root.expanduser()
    if create:
        root.mkdir(parents=True, exist_ok=True)
    if not root.is_dir() or _is_link_or_reparse(root):
        raise WorkshopPreviewError("Workshop cache root is not a regular directory")
    target = root / item
    if create:
        target.mkdir(exist_ok=True)
    if not target.is_dir() or _is_link_or_reparse(target):
        raise WorkshopPreviewError("Workshop cache item is not a regular directory")
    return target


def _atomic_write(path: Path, data: bytes) -> None:
    if os.path.lexists(path) and (_is_link_or_reparse(path) or not path.is_file()):
        raise WorkshopPreviewError("Workshop cache target is not a regular file")
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        with temporary.open("xb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _write_entry(
    root: Path, details: WorkshopDetails, preview_path: Path | None
) -> WorkshopCacheEntry:
    directory = _safe_item_dir(root, details.workshop_id, create=True)
    preview_file = "" if preview_path is None else preview_path.name
    if preview_file and not PREVIEW_FILE_RE.fullmatch(preview_file):
        raise WorkshopPreviewError("invalid Workshop preview cache filename")
    document = {
        "version": 1,
        "workshop_id": details.workshop_id,
        "title": details.title,
        "preview_url": details.preview_url,
        "preview_file": preview_file,
        "updated_utc": datetime.now(timezone.utc).isoformat(),
    }
    encoded = (json.dumps(document, ensure_ascii=False, sort_keys=True) + "\n").encode(
        "utf-8"
    )
    _atomic_write(directory / "details.json", encoded)
    return WorkshopCacheEntry(
        details.workshop_id, details.title, details.preview_url, preview_path
    )


def load_cached_entry(root: Path, item_id: str) -> WorkshopCacheEntry | None:
    item = _item_id(item_id)
    try:
        directory = _safe_item_dir(root, item, create=False)
        source = directory / "details.json"
        if _is_link_or_reparse(source) or not source.is_file():
            return None
        size = source.stat().st_size
        if size <= 0 or size > MAX_CACHE_JSON_BYTES:
            return None
        document = json.loads(source.read_text(encoding="utf-8"))
        if document.get("version") != 1 or document.get("workshop_id") != item:
            return None
        title = _clean_title(document.get("title", ""), item)
        preview_url = str(document.get("preview_url", ""))
        if preview_url and not preview_url_allowed(preview_url):
            return None
        preview_name = str(document.get("preview_file", ""))
        preview_path: Path | None = None
        if preview_name:
            if not PREVIEW_FILE_RE.fullmatch(preview_name):
                return None
            candidate = directory / preview_name
            if _is_link_or_reparse(candidate) or not candidate.is_file():
                return None
            if candidate.stat().st_size > MAX_PREVIEW_BYTES:
                return None
            _image_suffix(candidate.read_bytes()[:16])
            preview_path = candidate
        return WorkshopCacheEntry(item, title, preview_url, preview_path)
    except (OSError, ValueError, TypeError, json.JSONDecodeError, WorkshopPreviewError):
        return None


def refresh_workshop_metadata(
    item_ids: Iterable[str],
    *,
    root: Path | None = None,
    opener: UrlOpen = urllib.request.urlopen,
) -> tuple[dict[str, WorkshopCacheEntry], bool]:
    """Refresh bounded metadata; preserve valid cached data when offline."""

    cache_root = default_cache_root() if root is None else root
    items = sorted({_item_id(item) for item in item_ids}, key=int)
    cached = {
        item: entry
        for item in items
        if (entry := load_cached_entry(cache_root, item)) is not None
    }
    try:
        remote = query_workshop_details(items, opener=opener)
    except (OSError, WorkshopPreviewError):
        return cached, False
    for item, details in remote.items():
        prior = cached.get(item)
        preview = (
            prior.preview_path
            if prior is not None and prior.preview_url == details.preview_url
            else None
        )
        try:
            cached[item] = _write_entry(cache_root, details, preview)
        except (OSError, WorkshopPreviewError):
            pass
    return cached, True


def fetch_and_cache_preview(
    item_id: str,
    *,
    root: Path | None = None,
    opener: UrlOpen | None = None,
) -> WorkshopCacheEntry:
    cache_root = default_cache_root() if root is None else root
    entry = load_cached_entry(cache_root, item_id)
    if entry is None or not entry.preview_url:
        raise WorkshopPreviewError("Workshop preview URL is unavailable")
    if not preview_url_allowed(entry.preview_url):
        raise WorkshopPreviewError("Workshop preview URL is not allowed")
    request = urllib.request.Request(
        entry.preview_url,
        method="GET",
        headers={"Accept": "image/*", "User-Agent": "Isaac-Vita-Sync/preview-cache"},
    )
    if opener is None:
        opener = urllib.request.build_opener(_SteamPreviewRedirectHandler()).open
    with _open(opener, request) as response:
        _response_ok(response)
        final_url = str(response.geturl())
        if not preview_url_allowed(final_url):
            raise WorkshopPreviewError("Workshop preview redirected outside Steam CDN")
        headers = getattr(response, "headers", {})
        content_type = headers.get("Content-Type", "") if hasattr(headers, "get") else ""
        if not str(content_type).lower().startswith("image/"):
            raise WorkshopPreviewError("Workshop preview response is not an image")
        data = _read_bounded(response, MAX_PREVIEW_BYTES)
    suffix = _image_suffix(data)
    _validate_image(data)
    directory = _safe_item_dir(cache_root, entry.workshop_id, create=True)
    destination = directory / f"preview{suffix}"
    _atomic_write(destination, data)
    return _write_entry(
        cache_root,
        WorkshopDetails(entry.workshop_id, entry.title, entry.preview_url),
        destination,
    )


def invalidate_cached_preview(
    item_id: str,
    *,
    root: Path | None = None,
    expected_path: Path | None = None,
) -> WorkshopCacheEntry | None:
    """Forget one corrupt cache image so the next session retries it."""

    cache_root = default_cache_root() if root is None else root
    entry = load_cached_entry(cache_root, item_id)
    if entry is None or entry.preview_path is None:
        return entry
    if expected_path is not None and entry.preview_path != expected_path:
        return entry
    replacement = _write_entry(
        cache_root,
        WorkshopDetails(entry.workshop_id, entry.title, entry.preview_url),
        None,
    )
    if not _is_link_or_reparse(entry.preview_path):
        try:
            entry.preview_path.unlink()
        except FileNotFoundError:
            pass
    return replacement
