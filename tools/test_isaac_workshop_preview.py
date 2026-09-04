from __future__ import annotations

import base64
import json
from pathlib import Path
import tempfile
import unittest
import urllib.parse

import isaac_workshop_preview as preview


class FakeResponse:
    def __init__(
        self,
        data: bytes,
        *,
        url: str = preview.DETAILS_URL,
        content_type: str = "application/json",
        status: int = 200,
        advertised_length: int | None = None,
    ) -> None:
        self.data = data
        self.url = url
        self.status = status
        self.headers = {
            "Content-Type": content_type,
            "Content-Length": str(
                len(data) if advertised_length is None else advertised_length
            ),
        }

    def __enter__(self):
        return self

    def __exit__(self, unused_type, unused_value, unused_traceback) -> None:
        return None

    def read(self, amount: int = -1) -> bytes:
        return self.data if amount < 0 else self.data[:amount]

    def geturl(self) -> str:
        return self.url


class FakeOpener:
    def __init__(self, *responses: FakeResponse) -> None:
        self.responses = list(responses)
        self.requests = []

    def __call__(self, request, *, timeout: float):
        self.requests.append((request, timeout))
        if not self.responses:
            raise OSError("offline")
        return self.responses.pop(0)


def details_response(
    item: str = "836319872",
    *,
    title: str = "External Item Descriptions",
    preview_url: str = "https://images.steamusercontent.com/ugc/example.jpg",
) -> FakeResponse:
    data = {
        "response": {
            "result": 1,
            "resultcount": 1,
            "publishedfiledetails": [
                {
                    "publishedfileid": item,
                    "result": 1,
                    "consumer_app_id": 250900,
                    "title": title,
                    "preview_url": preview_url,
                }
            ],
        }
    }
    return FakeResponse(json.dumps(data).encode("utf-8"))


class WorkshopPreviewTests(unittest.TestCase):
    def test_query_is_keyless_bounded_and_only_uses_requested_local_ids(self) -> None:
        opener = FakeOpener(details_response(title="  External\n Item  Descriptions  "))
        result = preview.query_workshop_details(
            ["836319872", "836319872"], opener=opener
        )
        self.assertEqual(list(result), ["836319872"])
        self.assertEqual(result["836319872"].title, "External Item Descriptions")
        request, timeout = opener.requests[0]
        self.assertEqual(request.full_url, preview.DETAILS_URL)
        self.assertEqual(request.method, "POST")
        self.assertEqual(timeout, 8.0)
        fields = urllib.parse.parse_qs(request.data.decode("ascii"))
        self.assertEqual(fields["itemcount"], ["1"])
        self.assertEqual(fields["publishedfileids[0]"], ["836319872"])
        self.assertNotIn("key", fields)

    def test_foreign_app_and_untrusted_preview_are_not_used(self) -> None:
        response = json.loads(details_response().data)
        row = response["response"]["publishedfiledetails"][0]
        row["preview_url"] = "https://127.0.0.1/private.png"
        opener = FakeOpener(FakeResponse(json.dumps(response).encode("utf-8")))
        result = preview.query_workshop_details(["836319872"], opener=opener)
        self.assertEqual(result["836319872"].preview_url, "")

        row["consumer_app_id"] = 123
        opener = FakeOpener(FakeResponse(json.dumps(response).encode("utf-8")))
        self.assertEqual(
            preview.query_workshop_details(["836319872"], opener=opener), {}
        )

    def test_metadata_response_limit_is_fail_closed(self) -> None:
        opener = FakeOpener(
            FakeResponse(b"{}", advertised_length=preview.MAX_DETAILS_BYTES + 1)
        )
        with self.assertRaises(preview.WorkshopPreviewError):
            preview.query_workshop_details(["836319872"], opener=opener)

    def test_metadata_cache_survives_offline_refresh(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "cache"
            entries, online = preview.refresh_workshop_metadata(
                ["836319872"], root=root, opener=FakeOpener(details_response())
            )
            self.assertTrue(online)
            self.assertEqual(entries["836319872"].title, "External Item Descriptions")
            self.assertIsNone(entries["836319872"].preview_path)

            entries, online = preview.refresh_workshop_metadata(
                ["836319872"], root=root, opener=FakeOpener()
            )
            self.assertFalse(online)
            self.assertEqual(entries["836319872"].title, "External Item Descriptions")

    def test_preview_is_https_allowlisted_magic_checked_and_cached(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "cache"
            preview.refresh_workshop_metadata(
                ["836319872"], root=root, opener=FakeOpener(details_response())
            )
            png = base64.b64decode(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR4"
                "2mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
            )
            response = FakeResponse(
                png,
                url="https://images.steamusercontent.com/ugc/final.png",
                content_type="image/png",
            )
            entry = preview.fetch_and_cache_preview(
                "836319872", root=root, opener=FakeOpener(response)
            )
            self.assertIsNotNone(entry.preview_path)
            assert entry.preview_path is not None
            self.assertEqual(entry.preview_path.name, "preview.png")
            self.assertEqual(entry.preview_path.read_bytes(), png)
            self.assertEqual(preview.load_cached_entry(root, "836319872"), entry)

    def test_signature_only_corrupt_image_is_rejected_before_cache_publish(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "cache"
            preview.refresh_workshop_metadata(
                ["836319872"], root=root, opener=FakeOpener(details_response())
            )
            corrupt = FakeResponse(
                b"\x89PNG\r\n\x1a\nnot-a-real-png",
                url="https://images.steamusercontent.com/ugc/corrupt",
                content_type="image/png",
            )
            with self.assertRaises(preview.WorkshopPreviewError):
                preview.fetch_and_cache_preview(
                    "836319872", root=root, opener=FakeOpener(corrupt)
                )
            self.assertFalse((root / "836319872" / "preview.png").exists())

    def test_preview_redirect_and_non_image_are_rejected_without_publish(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "cache"
            preview.refresh_workshop_metadata(
                ["836319872"], root=root, opener=FakeOpener(details_response())
            )
            redirected = FakeResponse(
                b"\x89PNG\r\n\x1a\n",
                url="https://example.com/preview.png",
                content_type="image/png",
            )
            with self.assertRaises(preview.WorkshopPreviewError):
                preview.fetch_and_cache_preview(
                    "836319872", root=root, opener=FakeOpener(redirected)
                )
            self.assertFalse((root / "836319872" / "preview.png").exists())

            html = FakeResponse(
                b"<html>",
                url="https://images.steamusercontent.com/ugc/nope",
                content_type="text/html",
            )
            with self.assertRaises(preview.WorkshopPreviewError):
                preview.fetch_and_cache_preview(
                    "836319872", root=root, opener=FakeOpener(html)
                )

    def test_cache_path_fields_cannot_escape_item_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "cache"
            item = root / "836319872"
            item.mkdir(parents=True)
            (item / "details.json").write_text(
                json.dumps(
                    {
                        "version": 1,
                        "workshop_id": "836319872",
                        "title": "EID",
                        "preview_url": "",
                        "preview_file": "../outside.png",
                    }
                ),
                encoding="utf-8",
            )
            self.assertIsNone(preview.load_cached_entry(root, "836319872"))

    def test_url_allowlist_and_id_validation(self) -> None:
        self.assertTrue(
            preview.preview_url_allowed(
                "https://steamuserimages-a.akamaihd.net/ugc/example/preview.jpg"
            )
        )
        self.assertFalse(
            preview.preview_url_allowed(
                "https://images.steamusercontent.com.evil.example/preview.jpg"
            )
        )
        self.assertFalse(
            preview.preview_url_allowed(
                "http://images.steamusercontent.com/ugc/preview.jpg"
            )
        )
        self.assertEqual(
            preview._validated_redirect(
                "https://images.steamusercontent.com/ugc/old",
                "/ugc/new",
            ),
            "https://images.steamusercontent.com/ugc/new",
        )
        with self.assertRaises(preview.WorkshopPreviewError):
            preview._validated_redirect(
                "https://images.steamusercontent.com/ugc/old",
                "https://example.com/intermediate",
            )
        with self.assertRaises(preview.WorkshopPreviewError):
            preview.query_workshop_details(["../836319872"], opener=FakeOpener())


if __name__ == "__main__":
    unittest.main()
