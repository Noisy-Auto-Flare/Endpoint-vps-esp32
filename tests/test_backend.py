"""Stdlib-only backend tests (unittest + Flask test_client + unittest.mock).

Covers photo-staging v2 and STT toggle branches. No network, no new deps.
Run: python -m unittest discover -s tests -v
  or: python -m unittest tests.test_backend -v
"""

import base64
import importlib
import json
import os
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import app as app_module  # noqa: E402  (reloaded per-test with controlled env)

DEVICE_KEY = "test-key"
HEADERS = {"X-Device-Key": DEVICE_KEY}

FAKE_JPEG = b"\xff\xd8\xff fake-jpeg-bytes \xff\xd9"


def _build_wav(payload: bytes, rate=16000, channels=1, bits=16, audio_fmt=1,
               extra_before_data: bytes = b"", riff: bytes = b"RIFF", wave: bytes = b"WAVE"):
    """Build a WAV file; canonical 44B header when defaults are used."""
    byte_rate = rate * channels * bits // 8
    block_align = channels * bits // 8
    fmt_chunk = b"fmt " + struct.pack("<IHHIIHH", 16, audio_fmt, channels,
                                      rate, byte_rate, block_align, bits)
    chunks = fmt_chunk + extra_before_data + b"data" + struct.pack("<I", len(payload))
    size = 4 + len(chunks) + len(payload)
    return riff + struct.pack("<I", size) + wave + chunks + payload


def _wav(payload_len=1600, **kw):
    """Deterministic fake WAV chunk: 44-byte canonical header + payload."""
    return _build_wav(b"\x01\x02" * payload_len, **kw)


def fresh_app(groq_enabled=False):
    """Reload app with controlled import-time env; return (module, client)."""
    os.environ["DEVICE_KEY"] = DEVICE_KEY
    os.environ["GOOGLE_API_KEY"] = "x"
    os.environ["GROQ_STT_ENABLED"] = "true" if groq_enabled else "false"
    os.environ["GROQ_API_KEY"] = "dummy-groq-key" if groq_enabled else ""
    mod = importlib.reload(app_module)
    mod.app.testing = True
    return mod, mod.app.test_client()


class HealthTest(unittest.TestCase):
    def test_health_200_no_key(self):
        _, client = fresh_app()
        resp = client.get("/api/health")
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertEqual(body["status"], "ok")
        self.assertIn("model", body)


class AuthTest(unittest.TestCase):
    def test_unauthorized_without_key(self):
        _, client = fresh_app()
        chunk_headers = {"X-Session-Id": "auth1", "X-Chunk-Index": "0"}
        for method, path, kwargs in [
            ("post", "/api/photo-frame", {"data": FAKE_JPEG}),
            ("get", "/api/latest", {}),
            ("get", "/api/photo-status", {}),
            ("post", "/api/photos/commit", {}),
            ("post", "/api/photo", {"data": FAKE_JPEG}),
            ("post", "/api/photos", {"json": {"images": ["aGVsbG8="]}}),
            ("post", "/api/voice", {"data": _wav(10)}),
            ("post", "/api/voice-chunk", {"data": _wav(10), "headers": chunk_headers}),
            ("get", "/api/voice-mode", {}),
            ("post", "/api/voice-mode", {"json": {"mode": "lesson"}}),
            ("post", "/api/device-ip", {}),
        ]:
            with self.subTest(path=path):
                resp = getattr(client, method)(path, **kwargs)
                self.assertEqual(resp.status_code, 401)
                self.assertEqual(resp.get_json()["error"], "unauthorized")


class VoiceModeTest(unittest.TestCase):
    def test_default_mode_qa(self):
        _, client = fresh_app()
        resp = client.get("/api/voice-mode", headers=HEADERS)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.get_json(), {"mode": "qa"})

    def test_post_switch_and_get(self):
        _, client = fresh_app()
        resp = client.post("/api/voice-mode", headers=HEADERS, json={"mode": "lesson"})
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.get_json(), {"ok": True, "mode": "lesson"})
        self.assertEqual(client.get("/api/voice-mode", headers=HEADERS).get_json(),
                         {"mode": "lesson"})
        back = client.post("/api/voice-mode", headers=HEADERS, json={"mode": "qa"})
        self.assertEqual(back.status_code, 200)
        self.assertEqual(back.get_json(), {"ok": True, "mode": "qa"})
        self.assertEqual(client.get("/api/voice-mode", headers=HEADERS).get_json(),
                         {"mode": "qa"})

    def test_bad_mode_400_keeps_state(self):
        mod, client = fresh_app()
        for payload in ({"mode": "bogus"}, {"mode": ""}, {}, {"mode": 123}):
            with self.subTest(payload=payload):
                resp = client.post("/api/voice-mode", headers=HEADERS, json=payload)
                self.assertEqual(resp.status_code, 400)
                self.assertIn("error", resp.get_json())
        self.assertEqual(mod._get_voice_mode(), "qa")

    def test_non_json_body_400(self):
        _, client = fresh_app()
        resp = client.post("/api/voice-mode", headers=HEADERS,
                           data="not json", content_type="application/json")
        self.assertEqual(resp.status_code, 400)


class PhotoFrameTest(unittest.TestCase):
    def test_frame_empty_400(self):
        _, client = fresh_app()
        resp = client.post("/api/photo-frame", headers=HEADERS, data=b"")
        self.assertEqual(resp.status_code, 400)

    def test_frame_ok_staged_count(self):
        _, client = fresh_app()
        r1 = client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        self.assertEqual(r1.status_code, 200)
        self.assertEqual(r1.get_json(), {"ok": True, "staged": 1})
        r2 = client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        self.assertEqual(r2.status_code, 200)
        self.assertEqual(r2.get_json()["staged"], 2)

    def test_frame_full_staging_413(self):
        mod, client = fresh_app()
        cap = mod.PHOTO_STAGING_CAP
        for _ in range(cap):
            resp = client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
            self.assertEqual(resp.status_code, 200)
        overflow = client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        self.assertEqual(overflow.status_code, 413)


class PhotosCommitTest(unittest.TestCase):
    def test_commit_empty_400(self):
        _, client = fresh_app()
        resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 400)
        self.assertIn("error", resp.get_json())

    def test_commit_success_clears_staging(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="canned-answer") as m:
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertTrue(body["ok"])
        self.assertEqual(body["frames"], 2)
        self.assertEqual(body["text"], "canned-answer")
        self.assertEqual(m.call_count, 1)
        # staging cleared, status txt_ready
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["staged"], 0)
        self.assertEqual(status["photo_status"], "txt_ready")
        self.assertTrue(status["answer_ready"])

    def test_commit_gemini_fail_keeps_staging(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", side_effect=RuntimeError("boom")):
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 502)
        self.assertIn("error", resp.get_json())
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["staged"], 1)
        self.assertEqual(status["photo_status"], "error")
        self.assertFalse(status["answer_ready"])


class PhotoStatusTest(unittest.TestCase):
    def test_idle_shape(self):
        _, client = fresh_app()
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body, {"staged": 0, "photo_status": "idle", "answer_ready": False,
                                "voice_busy": False, "voice_mode": "qa", "lesson_chunks": 0,
                                "cam_ip": "", "voice_recording": False})

    def test_staged_shape(self):
        _, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["staged"], 1)
        self.assertEqual(body["photo_status"], "staged")
        self.assertFalse(body["answer_ready"])

    def test_txt_ready_shape(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="canned"):
            client.post("/api/photos/commit", headers=HEADERS)
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["staged"], 0)
        self.assertEqual(body["photo_status"], "txt_ready")
        self.assertTrue(body["answer_ready"])


class LatestConsumeTest(unittest.TestCase):
    def test_latest_consumed_once_for_photo_source(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="canned-answer"):
            client.post("/api/photos/commit", headers=HEADERS)
        first = client.get("/api/latest", headers=HEADERS)
        self.assertEqual(first.status_code, 200)
        body = json.loads(first.get_data(as_text=True))
        self.assertTrue(body["ready"])
        self.assertEqual(body["text"], "canned-answer")
        # photo consume flips staging status to consumed
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["photo_status"], "consumed")
        second = client.get("/api/latest", headers=HEADERS)
        body2 = json.loads(second.get_data(as_text=True))
        self.assertEqual(second.status_code, 200)
        self.assertFalse(body2["ready"])

    def test_latest_voice_consumed_once(self):
        mod, client = fresh_app()
        vheaders = dict(HEADERS, **{
            "X-Session-Id": "consumev",
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })
        with mock.patch.object(mod, "call_gemini", return_value="v-ans"):
            resp = client.post("/api/voice-chunk", headers=vheaders, data=_wav())
        self.assertEqual(resp.status_code, 200)
        first = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(first["ready"])
        self.assertEqual(first["text"], "v-ans")
        self.assertTrue(first["source"].startswith("voice"))
        second = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertFalse(second["ready"])


class VoiceChunkSttTest(unittest.TestCase):
    def _final_headers(self, sid):
        return dict(HEADERS, **{
            "X-Session-Id": sid,
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })

    def test_final_groq_disabled_goes_direct(self):
        mod, client = fresh_app(groq_enabled=False)
        self.assertFalse(mod.GROQ_STT_ENABLED)
        with mock.patch.object(mod, "call_gemini", return_value="voice-answer") as m:
            resp = client.post("/api/voice-chunk", headers=self._final_headers("s1"), data=_wav())
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertTrue(body["ok"])
        self.assertEqual(body["stt"], "gemini_direct")
        self.assertEqual(body["text"], "voice-answer")
        self.assertEqual(m.call_count, 1)

    def test_final_groq_enabled_uses_transcript(self):
        mod, client = fresh_app(groq_enabled=True)
        self.assertTrue(mod.GROQ_STT_ENABLED)
        with (
            mock.patch.object(mod, "transcribe_groq", return_value="privet mir") as t,
            mock.patch.object(mod, "call_gemini", return_value="otvet") as g,
        ):
            resp = client.post("/api/voice-chunk", headers=self._final_headers("s2"), data=_wav())
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertEqual(body["stt"], "groq")
        self.assertEqual(body["transcript"], "privet mir")
        self.assertEqual(body["text"], "otvet")
        self.assertEqual(t.call_count, 1)
        self.assertEqual(g.call_count, 1)


class LegacyPhotosTest(unittest.TestCase):
    def test_legacy_photos_ok(self):
        mod, client = fresh_app()
        payload = {"images": [base64.b64encode(FAKE_JPEG).decode("ascii")]}
        with mock.patch.object(mod, "call_gemini", return_value="burst-answer"):
            resp = client.post("/api/photos", headers=HEADERS, json=payload)
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertTrue(body["ok"])
        self.assertEqual(body["text"], "burst-answer")

    def test_legacy_photos_empty_400(self):
        _, client = fresh_app()
        resp = client.post("/api/photos", headers=HEADERS, json={"images": []})
        self.assertEqual(resp.status_code, 400)


class TwoSlotTest(unittest.TestCase):
    def test_photo_then_voice_newest_first(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="photo-answer"):
            client.post("/api/photos/commit", headers=HEADERS)
        vheaders = dict(HEADERS, **{
            "X-Session-Id": "twoslot",
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })
        with mock.patch.object(mod, "call_gemini", return_value="voice-answer"):
            resp = client.post("/api/voice-chunk", headers=vheaders, data=_wav())
        self.assertEqual(resp.status_code, 200)
        first = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(first["ready"])
        self.assertEqual(first["text"], "voice-answer")
        self.assertEqual(first["source"], "voice_chunks")
        second = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(second["ready"])
        self.assertEqual(second["text"], "photo-answer")
        self.assertEqual(second["source"], "photos_commit")
        third = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertFalse(third["ready"])


class HonestAnswerReadyTest(unittest.TestCase):
    def test_stale_after_fail_not_ready(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="canned"):
            client.post("/api/photos/commit", headers=HEADERS)
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", side_effect=RuntimeError("boom")):
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 502)
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertFalse(body["answer_ready"])

    def test_stale_after_fail_not_served_by_latest(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="canned"):
            client.post("/api/photos/commit", headers=HEADERS)
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", side_effect=RuntimeError("boom")):
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 502)
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertFalse(latest["ready"])


class HonestGateComplementTest(unittest.TestCase):
    def test_success_after_fail_serves_again(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="first"):
            client.post("/api/photos/commit", headers=HEADERS)
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", side_effect=RuntimeError("boom")):
            self.assertEqual(client.post("/api/photos/commit", headers=HEADERS).status_code, 502)
        self.assertFalse(client.get("/api/photo-status", headers=HEADERS).get_json()["answer_ready"])
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="second"):
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 200)
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertTrue(status["answer_ready"])
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(latest["ready"])
        self.assertEqual(latest["text"], "second")

    def test_voice_answer_after_photo_fail_served(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="photo1"):
            client.post("/api/photos/commit", headers=HEADERS)
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", side_effect=RuntimeError("boom")):
            client.post("/api/photos/commit", headers=HEADERS)
        vheaders = dict(HEADERS, **{
            "X-Session-Id": "voiceafterfail",
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })
        with mock.patch.object(mod, "call_gemini", return_value="voice1"):
            rv = client.post("/api/voice-chunk", headers=vheaders, data=_wav())
        self.assertEqual(rv.status_code, 200)
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertTrue(status["answer_ready"])
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(latest["ready"])
        self.assertEqual(latest["text"], "voice1")
        self.assertTrue(latest["source"].startswith("voice"))


class VoiceBusyTest(unittest.TestCase):
    def test_voice_busy_false_after_final(self):
        mod, client = fresh_app()
        vheaders = dict(HEADERS, **{
            "X-Session-Id": "busy1",
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })
        with mock.patch.object(mod, "call_gemini", return_value="v"):
            client.post("/api/voice-chunk", headers=vheaders, data=_wav())
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertIn("voice_busy", body)
        self.assertFalse(body["voice_busy"])


class OversizeFrameTest(unittest.TestCase):
    def test_oversize_frame_413(self):
        mod, client = fresh_app()
        setattr(mod, "PHOTO_FRAME_MAX_BYTES", 10)
        resp = client.post("/api/photo-frame", headers=HEADERS, data=b"x" * 100)
        self.assertEqual(resp.status_code, 413)
        self.assertIn("error", resp.get_json())


class PhotoMaxBytesTest(unittest.TestCase):
    def test_photo_too_large_413(self):
        mod, client = fresh_app()
        setattr(mod, "PHOTO_MAX_BYTES", 10)
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/photo", headers=HEADERS, data=b"x" * 100)
        self.assertEqual(resp.status_code, 413)
        self.assertIn("error", resp.get_json())
        self.assertEqual(m.call_count, 0)


class VoiceMaxBytesTest(unittest.TestCase):
    def test_voice_too_large_413(self):
        mod, client = fresh_app()
        setattr(mod, "VOICE_MAX_BYTES", 10)
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/voice", headers=HEADERS, data=b"x" * 100)
        self.assertEqual(resp.status_code, 413)
        self.assertIn("error", resp.get_json())
        self.assertEqual(m.call_count, 0)


class VoiceSessionsCapTest(unittest.TestCase):
    def _chunk_headers(self, sid):
        return dict(HEADERS, **{
            "X-Session-Id": sid,
            "X-Chunk-Index": "0",
            "Content-Type": "audio/wav",
        })

    def test_new_session_beyond_cap_413_no_state_change(self):
        mod, client = fresh_app()
        setattr(mod, "VOICE_MAX_SESSIONS", 1)
        r1 = client.post("/api/voice-chunk", headers=self._chunk_headers("s-cap-1"), data=_wav())
        self.assertEqual(r1.status_code, 200)
        before = dict(mod._voice_sessions)
        r2 = client.post("/api/voice-chunk", headers=self._chunk_headers("s-cap-2"), data=_wav())
        self.assertEqual(r2.status_code, 413)
        self.assertIn("error", r2.get_json())
        self.assertEqual(len(mod._voice_sessions), 1)
        self.assertNotIn("s-cap-2", mod._voice_sessions)
        self.assertIn("s-cap-1", mod._voice_sessions)
        self.assertEqual(mod._voice_sessions["s-cap-1"], before["s-cap-1"])


class LegacyPhotosValidationTest(unittest.TestCase):
    def test_photos_non_str_400(self):
        mod, client = fresh_app()
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/photos", headers=HEADERS,
                               json={"images": ["aGVsbG8=", 123]})
        self.assertEqual(resp.status_code, 400)
        self.assertIn("error", resp.get_json())
        self.assertEqual(m.call_count, 0)


class CommitPrefixSurvivalTest(unittest.TestCase):
    def test_frame_staged_during_commit_survives(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)

        def slow_gemini(system_prompt, parts):
            with mod._photo_lock:
                mod._photo_staging.append((FAKE_JPEG, time.time()))
            return "canned-survivor"

        with mock.patch.object(mod, "call_gemini", side_effect=slow_gemini):
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 200)
        body = resp.get_json()
        self.assertTrue(body["ok"])
        self.assertEqual(body["frames"], 1)
        self.assertEqual(body["text"], "canned-survivor")
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["staged"], 1)
        self.assertEqual(status["photo_status"], "txt_ready")
        self.assertTrue(status["answer_ready"])


class ParallelCommit429Test(unittest.TestCase):
    def test_second_commit_429_while_first_runs(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        entered = threading.Event()
        release = threading.Event()

        def slow_gemini(system_prompt, parts):
            entered.set()
            release.wait(timeout=10)
            return "slow-answer"

        results = {}

        def _first_commit():
            results["first"] = client.post("/api/photos/commit", headers=HEADERS)

        with mock.patch.object(mod, "call_gemini", side_effect=slow_gemini):
            t = threading.Thread(target=_first_commit)
            t.start()
            self.assertTrue(entered.wait(timeout=10))
            client2 = mod.app.test_client()
            second = client2.post("/api/photos/commit", headers=HEADERS)
            self.assertEqual(second.status_code, 429)
            self.assertIn("error", second.get_json())
            release.set()
            t.join(timeout=10)
        self.assertEqual(results["first"].status_code, 200)


class CompactJsonTest(unittest.TestCase):
    def test_latest_compact_cyrillic_json(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="Привет мир ответ"):
            client.post("/api/photos/commit", headers=HEADERS)
        resp = client.get("/api/latest", headers=HEADERS)
        self.assertEqual(resp.mimetype, "application/json")
        raw = resp.get_data(as_text=True)
        self.assertIn("Привет мир ответ", raw)
        self.assertNotIn("\\u041f", raw)
        self.assertIn('"ready":true', raw)
        self.assertNotIn('": ', raw)

    def test_photo_status_compact_json(self):
        _, client = fresh_app()
        resp = client.get("/api/photo-status", headers=HEADERS)
        self.assertEqual(resp.mimetype, "application/json")
        raw = resp.get_data(as_text=True)
        self.assertIn('"staged":0', raw)
        self.assertIn('"voice_busy":false,"voice_mode":"qa","lesson_chunks":0,'
                      '"cam_ip":"","voice_recording":false}', raw)
        self.assertNotIn('": ', raw)
        self.assertNotIn('": ', raw)


class TtlSweepTest(unittest.TestCase):
    def test_photo_staging_ttl_expiry(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        old = time.time() - mod.PHOTO_STAGING_TTL - 10
        mod._photo_staging[0] = (mod._photo_staging[0][0], old)
        r = client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.get_json()["staged"], 1)
        mod._photo_staging[0] = (mod._photo_staging[0][0], time.time() - mod.PHOTO_STAGING_TTL - 10)
        resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 400)
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["staged"], 0)
        self.assertEqual(status["photo_status"], "idle")

    def test_voice_session_ttl_expiry(self):
        mod, client = fresh_app()
        hdr1 = dict(HEADERS, **{"X-Session-Id": "ttl1", "X-Chunk-Index": "0",
                                "Content-Type": "audio/wav"})
        r1 = client.post("/api/voice-chunk", headers=hdr1, data=_wav(10))
        self.assertEqual(r1.status_code, 200)
        mod._voice_sessions["ttl1"]["created_at"] = time.time() - mod.VOICE_SESSION_TTL - 10
        hdr2 = dict(HEADERS, **{"X-Session-Id": "ttl2", "X-Chunk-Index": "0",
                                "Content-Type": "audio/wav"})
        r2 = client.post("/api/voice-chunk", headers=hdr2, data=_wav(10))
        self.assertEqual(r2.status_code, 200)
        self.assertNotIn("ttl1", mod._voice_sessions)
        self.assertIn("ttl2", mod._voice_sessions)


class VoiceChunkValidationTest(unittest.TestCase):
    def _hdr(self, sid, idx, final=False):
        d = dict(HEADERS, **{"X-Session-Id": sid, "X-Chunk-Index": str(idx),
                             "Content-Type": "audio/wav"})
        if final:
            d["X-Final"] = "true"
        return d

    def test_missing_session_id_400(self):
        _, client = fresh_app()
        resp = client.post("/api/voice-chunk",
                           headers=dict(HEADERS, **{"X-Chunk-Index": "0"}), data=_wav(10))
        self.assertEqual(resp.status_code, 400)

    def test_bad_index_400(self):
        _, client = fresh_app()
        resp = client.post("/api/voice-chunk",
                           headers=dict(HEADERS, **{"X-Session-Id": "badidx",
                                                   "X-Chunk-Index": "abc"}), data=_wav(10))
        self.assertEqual(resp.status_code, 400)

    def test_negative_index_400(self):
        _, client = fresh_app()
        resp = client.post("/api/voice-chunk", headers=self._hdr("neg1", -1), data=_wav(10))
        self.assertEqual(resp.status_code, 400)

    def test_empty_body_400(self):
        _, client = fresh_app()
        resp = client.post("/api/voice-chunk", headers=self._hdr("empty1", 0), data=b"")
        self.assertEqual(resp.status_code, 400)

    def test_gap_missing_chunks_400(self):
        mod, client = fresh_app()
        with mock.patch.object(mod, "call_gemini", return_value="x"):
            r0 = client.post("/api/voice-chunk", headers=self._hdr("gap1", 0), data=_wav(10))
        self.assertEqual(r0.status_code, 200)
        with mock.patch.object(mod, "call_gemini", return_value="x"):
            rfinal = client.post("/api/voice-chunk", headers=self._hdr("gap1", 2, final=True),
                                 data=_wav(10))
        self.assertEqual(rfinal.status_code, 400)
        self.assertIn("missing", rfinal.get_json()["error"])

    def test_duplicate_index_overwrite_ok(self):
        mod, client = fresh_app()
        with mock.patch.object(mod, "call_gemini", return_value="dup-ans"):
            r0 = client.post("/api/voice-chunk", headers=self._hdr("dup1", 0), data=_wav(10))
            self.assertEqual(r0.status_code, 200)
            r1 = client.post("/api/voice-chunk", headers=self._hdr("dup1", 0), data=_wav(20))
            self.assertEqual(r1.status_code, 200)
            rfinal = client.post("/api/voice-chunk", headers=self._hdr("dup1", 1, final=True),
                                 data=_wav(10))
        self.assertEqual(rfinal.status_code, 200)
        self.assertTrue(rfinal.get_json()["ok"])

    def test_too_many_chunks_413_resets_session(self):
        mod, client = fresh_app()
        setattr(mod, "VOICE_MAX_CHUNKS", 1)
        r0 = client.post("/api/voice-chunk", headers=self._hdr("many1", 0), data=_wav(10))
        self.assertEqual(r0.status_code, 200)
        r1 = client.post("/api/voice-chunk", headers=self._hdr("many1", 1), data=_wav(10))
        self.assertEqual(r1.status_code, 413)
        self.assertNotIn("many1", mod._voice_sessions)


class Caps413Test(unittest.TestCase):
    def test_photo_bytes_cap_413(self):
        mod, client = fresh_app()
        setattr(mod, "PHOTO_BYTES_CAP", 10)
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/photo-frame", headers=HEADERS, data=b"x" * 100)
        self.assertEqual(resp.status_code, 413)
        self.assertIn("error", resp.get_json())
        self.assertEqual(m.call_count, 0)

    def test_max_content_length_handler_413(self):
        mod, client = fresh_app()
        mod.app.config["MAX_CONTENT_LENGTH"] = 10
        resp = client.post("/api/photo-frame", headers=HEADERS, data=b"x" * 100)
        self.assertEqual(resp.status_code, 413)
        self.assertEqual(resp.get_json()["error"], "request too large")

    def test_photos_max_images_413(self):
        mod, client = fresh_app()
        setattr(mod, "PHOTOS_MAX_IMAGES", 1)
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/photos", headers=HEADERS,
                               json={"images": ["aGVsbG8=", "aGVsbG8="]})
        self.assertEqual(resp.status_code, 413)
        self.assertEqual(m.call_count, 0)

    def test_photos_max_total_b64_413(self):
        mod, client = fresh_app()
        setattr(mod, "PHOTOS_MAX_TOTAL_B64", 5)
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/photos", headers=HEADERS,
                               json={"images": ["aGVsbG8="]})
        self.assertEqual(resp.status_code, 413)
        self.assertEqual(m.call_count, 0)

    def test_voice_chunk_max_bytes_413(self):
        mod, client = fresh_app()
        setattr(mod, "VOICE_CHUNK_MAX_BYTES", 10)
        with mock.patch.object(mod, "call_gemini") as m:
            resp = client.post("/api/voice-chunk",
                               headers=dict(HEADERS, **{"X-Session-Id": "cap1",
                                                       "X-Chunk-Index": "0"}),
                               data=_wav(10))
        self.assertEqual(resp.status_code, 413)
        self.assertEqual(m.call_count, 0)


class VoiceConsumeStatusTest(unittest.TestCase):
    def _vfinal(self, sid):
        return dict(HEADERS, **{"X-Session-Id": sid, "X-Chunk-Index": "0",
                                "X-Final": "true", "Content-Type": "audio/wav"})

    def test_voice_consume_resets_idle_when_no_photo(self):
        mod, client = fresh_app()
        with mock.patch.object(mod, "call_gemini", return_value="v"):
            client.post("/api/voice-chunk", headers=self._vfinal("vi1"), data=_wav())
        first = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(first["ready"])
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["photo_status"], "idle")
        self.assertFalse(status["answer_ready"])

    def test_voice_consume_keeps_staged(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="v"):
            client.post("/api/voice-chunk", headers=self._vfinal("vi2"), data=_wav())
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(latest["ready"])
        self.assertTrue(latest["source"].startswith("voice"))
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["staged"], 1)
        self.assertEqual(status["photo_status"], "staged")

    def test_photo_consumed_then_voice_consume_idle(self):
        mod, client = fresh_app()
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="p"):
            client.post("/api/photos/commit", headers=HEADERS)
        first = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(first["ready"])
        consumed = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(consumed["photo_status"], "consumed")
        with mock.patch.object(mod, "call_gemini", return_value="v"):
            client.post("/api/voice-chunk", headers=self._vfinal("vi3"), data=_wav())
        second = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(second["ready"])
        self.assertTrue(second["source"].startswith("voice"))
        status = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(status["photo_status"], "idle")


class SttFallbackTest(unittest.TestCase):
    def _final(self, sid):
        return dict(HEADERS, **{"X-Session-Id": sid, "X-Chunk-Index": "0",
                                "X-Final": "true", "Content-Type": "audio/wav"})

    def test_empty_transcript_falls_back(self):
        mod, client = fresh_app(groq_enabled=True)
        with (
            mock.patch.object(mod, "transcribe_groq", return_value=""),
            mock.patch.object(mod, "call_gemini", return_value="ans"),
        ):
            resp = client.post("/api/voice-chunk", headers=self._final("fb1"), data=_wav())
        body = resp.get_json()
        self.assertEqual(body["stt"], "gemini_direct")
        self.assertEqual(body["transcript"], "")
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertEqual(latest["source"], "voice_chunks")

    def test_exception_falls_back(self):
        mod, client = fresh_app(groq_enabled=True)
        with (
            mock.patch.object(mod, "transcribe_groq", side_effect=ValueError("groq down")),
            mock.patch.object(mod, "call_gemini", return_value="ans"),
        ):
            resp = client.post("/api/voice-chunk", headers=self._final("fb2"), data=_wav())
        body = resp.get_json()
        self.assertEqual(body["stt"], "gemini_direct")
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertEqual(latest["source"], "voice_chunks")

    def test_no_key_falls_back(self):
        mod, client = fresh_app(groq_enabled=True)
        setattr(mod, "GROQ_API_KEY", "")
        with (
            mock.patch.object(mod, "transcribe_groq") as t,
            mock.patch.object(mod, "call_gemini", return_value="ans"),
        ):
            resp = client.post("/api/voice-chunk", headers=self._final("fb3"), data=_wav())
        body = resp.get_json()
        self.assertEqual(body["stt"], "gemini_direct")
        self.assertEqual(t.call_count, 0)
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertEqual(latest["source"], "voice_chunks")


class MergeEdgesTest(unittest.TestCase):
    def test_empty_list_raises(self):
        mod, _ = fresh_app()
        with self.assertRaises(RuntimeError) as cm:
            mod.merge_wav_chunks([])
        self.assertIn("empty chunks", str(cm.exception))

    def test_all_short_raises(self):
        mod, _ = fresh_app()
        with self.assertRaises(RuntimeError) as cm:
            mod.merge_wav_chunks([b"x" * 10, b"y" * 20])
        self.assertIn("no audio data", str(cm.exception))

    def test_data_before_fmt_message(self):
        mod, _ = fresh_app()
        payload = b"\x01\x02" * 10
        data_chunk = b"data" + struct.pack("<I", len(payload)) + payload
        byte_rate = 16000 * 1 * 16 // 8
        fmt_chunk = b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, byte_rate, 2, 16)
        size = 4 + len(data_chunk) + len(fmt_chunk)
        raw = b"RIFF" + struct.pack("<I", size) + b"WAVE" + data_chunk + fmt_chunk
        with self.assertRaises(RuntimeError) as cm:
            mod.merge_wav_chunks([raw])
        self.assertIn("data before fmt", str(cm.exception))


class CallGeminiArgsTest(unittest.TestCase):
    def test_photo_prompt_and_mime(self):
        mod, client = fresh_app()
        with mock.patch.object(mod, "call_gemini", return_value="p") as m:
            client.post("/api/photo", headers=HEADERS, data=FAKE_JPEG)
        self.assertEqual(m.call_count, 1)
        args, _ = m.call_args
        self.assertEqual(args[0], mod.PHOTO_SYSTEM_PROMPT)
        parts = args[1]
        self.assertEqual(parts[1]["inline_data"]["mime_type"], "image/jpeg")

    def test_voice_audio_prompt_and_mime(self):
        mod, client = fresh_app(groq_enabled=False)
        with mock.patch.object(mod, "call_gemini", return_value="v") as m:
            client.post("/api/voice", headers=HEADERS, data=_wav())
        self.assertEqual(m.call_count, 1)
        args, _ = m.call_args
        self.assertEqual(args[0], mod.VOICE_AUDIO_PROMPT)
        mimes = [p.get("inline_data", {}).get("mime_type") for p in args[1]]
        self.assertIn("audio/wav", mimes)

    def test_voice_transcript_prompt(self):
        mod, client = fresh_app(groq_enabled=True)
        vheaders = dict(HEADERS, **{"X-Session-Id": "argt", "X-Chunk-Index": "0",
                                    "X-Final": "true", "Content-Type": "audio/wav"})
        with (
            mock.patch.object(mod, "transcribe_groq", return_value="privet"),
            mock.patch.object(mod, "call_gemini", return_value="o") as g,
        ):
            client.post("/api/voice-chunk", headers=vheaders, data=_wav())
        self.assertEqual(g.call_count, 1)
        args, _ = g.call_args
        self.assertEqual(args[0], mod.VOICE_TRANSCRIPT_PROMPT)


class PromptTailDedupTest(unittest.TestCase):
    def test_shared_tail_used(self):
        mod, _ = fresh_app()
        tail = mod._VOICE_COMMON_TAIL
        self.assertIn(tail, mod.VOICE_AUDIO_PROMPT)
        self.assertIn(tail, mod.VOICE_TRANSCRIPT_PROMPT)
        self.assertIn("аудио", mod.VOICE_AUDIO_PROMPT)
        self.assertIn("расшифров", mod.VOICE_TRANSCRIPT_PROMPT)


class StartupGuardTest(unittest.TestCase):
    @unittest.skip("startup guard lives under __main__-only; not importable without subprocess")
    def test_startup_guard_untestable(self):
        pass


class MaxAnswerCharsTest(unittest.TestCase):
    def test_env_knob_default_8000(self):
        os.environ.pop("MAX_ANSWER_CHARS", None)
        mod, _ = fresh_app()
        self.assertEqual(mod.MAX_ANSWER_CHARS, 8000)

    def test_long_answer_truncated_with_marker(self):
        mod, _ = fresh_app()
        mod.MAX_ANSWER_CHARS = 10
        stored = mod.set_latest_result("x" * 50, source="photo")
        self.assertEqual(stored, "x" * 10 + "…")
        self.assertTrue(mod._photo_answer["text"].endswith("…"))
        self.assertEqual(len(mod._photo_answer["text"]), 11)

    def test_short_answer_untouched(self):
        mod, _ = fresh_app()
        mod.MAX_ANSWER_CHARS = 10
        stored = mod.set_latest_result("short", source="voice")
        self.assertEqual(stored, "short")
        self.assertFalse(stored.endswith("…"))

    def test_commit_path_returns_truncated(self):
        mod, client = fresh_app()
        mod.MAX_ANSWER_CHARS = 5
        client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
        with mock.patch.object(mod, "call_gemini", return_value="y" * 50):
            resp = client.post("/api/photos/commit", headers=HEADERS)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.get_json()["text"], "y" * 5 + "…")
        latest = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertEqual(latest["text"], "y" * 5 + "…")

    def test_voice_path_returns_truncated(self):
        mod, client = fresh_app()
        mod.MAX_ANSWER_CHARS = 5
        vheaders = dict(HEADERS, **{
            "X-Session-Id": "trunc1",
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })
        with mock.patch.object(mod, "call_gemini", return_value="z" * 50):
            resp = client.post("/api/voice-chunk", headers=vheaders, data=_wav())
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.get_json()["text"], "z" * 5 + "…")


class WavValidationTest(unittest.TestCase):
    def test_merge_valid_two_chunks(self):
        mod, _ = fresh_app()
        c1, c2 = _wav(100), _wav(200)
        out = mod.merge_wav_chunks([c1, c2])
        self.assertEqual(out[0:4], b"RIFF")
        self.assertEqual(out[8:12], b"WAVE")
        self.assertEqual(out[44:], c1[44:] + c2[44:])

    def test_non_riff_raises(self):
        mod, _ = fresh_app()
        bad = b"XXXX" + _wav(10)[4:]
        with self.assertRaises(RuntimeError) as cm:
            mod.merge_wav_chunks([bad])
        self.assertIn("unsupported wav format", str(cm.exception))

    def test_wrong_rate_raises_naming_value(self):
        mod, _ = fresh_app()
        bad = _wav(10, rate=8000)
        with self.assertRaises(RuntimeError) as cm:
            mod.merge_wav_chunks([bad])
        self.assertIn("unsupported wav format", str(cm.exception))
        self.assertIn("8000", str(cm.exception))

    def test_list_before_data_merges(self):
        mod, _ = fresh_app()
        extra = b"LIST" + struct.pack("<I", 4) + b"abcd"
        wav = _build_wav(b"\x01\x02" * 10, extra_before_data=extra)
        out = mod.merge_wav_chunks([wav])
        self.assertEqual(out[0:4], b"RIFF")
        self.assertEqual(out[44:], b"\x01\x02" * 10)

    def test_voice_chunk_garbage_400(self):
        _, client = fresh_app()
        vheaders = dict(HEADERS, **{
            "X-Session-Id": "garbage1",
            "X-Chunk-Index": "0",
            "X-Final": "true",
            "Content-Type": "audio/wav",
        })
        resp = client.post("/api/voice-chunk", headers=vheaders, data=b"GARBAGE" * 20)
        self.assertEqual(resp.status_code, 400)
        self.assertIn("error", resp.get_json())


class LessonModeTest(unittest.TestCase):
    def _lesson_app(self):
        mod, client = fresh_app()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod.LESSONS_DIR = tmp.name

        def _close_sessions():
            with mod._lesson_lock:
                for sid in list(mod._lesson_sessions):
                    mod._finalize_lesson_locked(sid, time.time(), reason="test")

        # LIFO: сначала закрываем открытые файлы, потом удаляем временную папку.
        self.addCleanup(_close_sessions)
        resp = client.post("/api/voice-mode", headers=HEADERS, json={"mode": "lesson"})
        self.assertEqual(resp.status_code, 200)
        return mod, client, tmp.name

    def _hdr(self, sid, idx, final=False):
        d = dict(HEADERS, **{"X-Session-Id": sid, "X-Chunk-Index": str(idx),
                             "Content-Type": "audio/wav"})
        if final:
            d["X-Final"] = "true"
        return d

    def test_lesson_max_chunks_default_2000(self):
        os.environ.pop("LESSON_MAX_CHUNKS", None)
        mod, _ = fresh_app()
        self.assertEqual(mod.LESSON_MAX_CHUNKS, 2000)
        self.assertNotEqual(mod.LESSON_MAX_CHUNKS, mod.VOICE_MAX_CHUNKS)

    def test_lesson_chunk_appends_pcm_and_final_writes_sidecar(self):
        mod, client, lessons = self._lesson_app()
        c0, c1 = _wav(10), _wav(20)
        with (
            mock.patch.object(mod, "call_gemini") as g,
            mock.patch.object(mod, "transcribe_groq") as t,
        ):
            r0 = client.post("/api/voice-chunk", headers=self._hdr("lesson1", 0), data=c0)
            r1 = client.post("/api/voice-chunk", headers=self._hdr("lesson1", 1, final=True),
                            data=c1)
        self.assertEqual(r0.status_code, 200)
        self.assertEqual(r0.get_json(), {"ok": True, "chunks_received": 1})
        self.assertEqual(r1.status_code, 200)
        self.assertEqual(r1.get_json(),
                         {"ok": True, "final": True, "mode": "lesson", "lesson_chunks": 2})
        self.assertEqual(g.call_count, 0)
        self.assertEqual(t.call_count, 0)

        names = sorted(os.listdir(lessons))
        self.assertEqual(len(names), 2)
        wav_name = next(n for n in names if n.endswith(".wav"))
        json_name = next(n for n in names if n.endswith(".json"))
        self.assertEqual(os.path.splitext(wav_name)[0], os.path.splitext(json_name)[0])
        self.assertRegex(wav_name,
                         r"^lesson_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_lesson1\.wav$")

        with open(os.path.join(lessons, wav_name), "rb") as fh:
            raw = fh.read()
        expected_pcm = c0[44:] + c1[44:]
        self.assertEqual(raw[0:4], b"RIFF")
        self.assertEqual(raw[8:12], b"WAVE")
        (riff_size,) = struct.unpack("<I", raw[4:8])
        (data_size,) = struct.unpack("<I", raw[40:44])
        self.assertEqual(data_size, len(expected_pcm))
        self.assertEqual(riff_size, 36 + len(expected_pcm))
        self.assertEqual(len(raw), 44 + len(expected_pcm))
        self.assertEqual(raw[44:], expected_pcm)

        with open(os.path.join(lessons, json_name), encoding="utf-8") as fh:
            sidecar = json.load(fh)
        self.assertEqual(sidecar["sid"], "lesson1")
        self.assertEqual(sidecar["chunks"], 2)
        self.assertGreaterEqual(sidecar["duration_sec"], 0)
        self.assertIn("T", sidecar["started_at"])
        self.assertIn("T", sidecar["ended_at"])

    def test_duplicate_index_not_appended_twice(self):
        mod, client, lessons = self._lesson_app()
        c = _wav(10)
        client.post("/api/voice-chunk", headers=self._hdr("dupl", 0), data=c)
        client.post("/api/voice-chunk", headers=self._hdr("dupl", 0), data=c)
        resp = client.post("/api/voice-chunk", headers=self._hdr("dupl", 1, final=True), data=c)
        self.assertEqual(resp.get_json()["lesson_chunks"], 2)
        wav_name = next(n for n in os.listdir(lessons) if n.endswith(".wav"))
        self.assertEqual(os.path.getsize(os.path.join(lessons, wav_name)),
                         44 + 2 * len(c[44:]))

    def test_qa_mode_still_calls_gemini(self):
        mod, client = fresh_app()
        self.assertEqual(client.get("/api/voice-mode", headers=HEADERS).get_json()["mode"], "qa")
        with mock.patch.object(mod, "call_gemini", return_value="ans") as g:
            resp = client.post("/api/voice-chunk",
                               headers=self._hdr("qa1", 0, final=True), data=_wav())
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.get_json()["text"], "ans")
        self.assertEqual(g.call_count, 1)
        self.assertEqual(mod._lesson_sessions, {})

    def test_photo_status_trailing_keys(self):
        mod, client, _ = self._lesson_app()
        client.post("/api/voice-chunk", headers=self._hdr("statusL", 0), data=_wav(10))
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["voice_mode"], "lesson")
        self.assertEqual(body["lesson_chunks"], 1)
        self.assertEqual(list(body)[-2:], ["cam_ip", "voice_recording"])
        # Чанк не финальный — запись идёт, поэтому voice_recording=true.
        raw = client.get("/api/photo-status", headers=HEADERS).get_data(as_text=True)
        self.assertTrue(raw.endswith(
            '"voice_mode":"lesson","lesson_chunks":1,"cam_ip":"","voice_recording":true}'))

    def test_lesson_mode_initial_chunks_zero(self):
        mod, client, _ = self._lesson_app()
        self.assertEqual(mod._lesson_last_chunks, 0)
        self.assertEqual(mod._lesson_sessions, {})
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["lesson_chunks"], 0)

    def test_photo_status_keeps_count_after_final(self):
        mod, client, _ = self._lesson_app()
        client.post("/api/voice-chunk", headers=self._hdr("keepL", 0), data=_wav(10))
        resp = client.post("/api/voice-chunk", headers=self._hdr("keepL", 1, final=True),
                           data=_wav(10))
        self.assertEqual(resp.get_json()["lesson_chunks"], 2)
        self.assertEqual(mod._lesson_sessions, {})
        # После стопа сессий нет — счётчик отдаёт последнюю финализацию, а не 0.
        self.assertEqual(mod._lesson_last_chunks, 2)
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["lesson_chunks"], 2)

    def test_photo_status_keeps_count_after_ttl_finalize(self):
        mod, client, _ = self._lesson_app()
        client.post("/api/voice-chunk", headers=self._hdr("ttlKeep", 0), data=_wav(10))
        client.post("/api/voice-chunk", headers=self._hdr("ttlKeep", 1), data=_wav(10))
        mod._lesson_sessions["ttlKeep"]["created_at"] = (
            time.time() - mod.VOICE_SESSION_TTL - 10)
        with mod._lesson_lock:
            evicted = mod._sweep_lesson_sessions_locked(time.time())
        self.assertEqual(evicted, 1)
        self.assertEqual(mod._lesson_sessions, {})
        self.assertEqual(mod._lesson_last_chunks, 2)
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["lesson_chunks"], 2)

    def test_new_lesson_reports_own_smaller_count(self):
        mod, client, _ = self._lesson_app()
        for idx in range(4):
            client.post("/api/voice-chunk", headers=self._hdr("bigL", idx), data=_wav(10))
        client.post("/api/voice-chunk", headers=self._hdr("bigL", 4, final=True),
                    data=_wav(10))
        self.assertEqual(mod._lesson_last_chunks, 5)
        client.post("/api/voice-chunk", headers=self._hdr("smallL", 0), data=_wav(10))
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["lesson_chunks"], 1)

    def test_qa_mode_keeps_lesson_chunks_zero(self):
        mod, client = fresh_app()
        self.assertEqual(client.get("/api/voice-mode", headers=HEADERS).get_json()["mode"],
                         "qa")
        with mock.patch.object(mod, "call_gemini", return_value="ans"):
            resp = client.post("/api/voice-chunk",
                               headers=self._hdr("qaZero", 0, final=True), data=_wav())
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(mod._lesson_last_chunks, 0)
        body = client.get("/api/photo-status", headers=HEADERS).get_json()
        self.assertEqual(body["lesson_chunks"], 0)

    def test_disk_failure_500_no_session_state(self):
        mod, client, lessons = self._lesson_app()
        blocked = os.path.join(lessons, "blocked")
        with open(blocked, "wb") as fh:
            fh.write(b"x")
        mod.LESSONS_DIR = blocked
        resp = client.post("/api/voice-chunk", headers=self._hdr("disk1", 0), data=_wav(10))
        self.assertEqual(resp.status_code, 500)
        self.assertIn("error", resp.get_json())
        self.assertEqual(mod._lesson_sessions, {})

    def test_append_failure_500_closes_session(self):
        mod, client, _ = self._lesson_app()
        r0 = client.post("/api/voice-chunk", headers=self._hdr("disk2", 0), data=_wav(10))
        self.assertEqual(r0.status_code, 200)
        fh = mod._lesson_sessions["disk2"]["file"]
        with mock.patch.object(mod, "_append_lesson_pcm_locked", side_effect=OSError("disk full")):
            resp = client.post("/api/voice-chunk", headers=self._hdr("disk2", 1), data=_wav(10))
        self.assertEqual(resp.status_code, 500)
        self.assertEqual(mod._lesson_sessions, {})
        self.assertTrue(fh.closed)

    def test_ttl_sweep_finalizes_file_and_sidecar(self):
        mod, client, lessons = self._lesson_app()
        client.post("/api/voice-chunk", headers=self._hdr("ttlL", 0), data=_wav(10))
        fh = mod._lesson_sessions["ttlL"]["file"]
        mod._lesson_sessions["ttlL"]["created_at"] = time.time() - mod.VOICE_SESSION_TTL - 10
        resp = client.post("/api/voice-chunk", headers=self._hdr("ttlL2", 0), data=_wav(10))
        self.assertEqual(resp.status_code, 200)
        self.assertNotIn("ttlL", mod._lesson_sessions)
        self.assertIn("ttlL2", mod._lesson_sessions)
        self.assertTrue(fh.closed)
        # Старая запись финализирована (WAV + JSON), новая ведётся (только WAV).
        self.assertEqual(len([n for n in os.listdir(lessons) if n.endswith(".wav")]), 2)
        self.assertEqual(len([n for n in os.listdir(lessons) if n.endswith(".json")]), 1)
        json_name = next(n for n in os.listdir(lessons) if n.endswith(".json"))
        with open(os.path.join(lessons, json_name), encoding="utf-8") as f:
            sidecar = json.load(f)
        self.assertEqual(sidecar["sid"], "ttlL")
        self.assertEqual(sidecar["chunks"], 1)

    def test_lesson_cap_independent_from_voice_cap(self):
        mod, client, _ = self._lesson_app()
        mod.VOICE_MAX_CHUNKS = 1
        r0 = client.post("/api/voice-chunk", headers=self._hdr("capL", 0), data=_wav(10))
        r1 = client.post("/api/voice-chunk", headers=self._hdr("capL", 1, final=True), data=_wav(10))
        self.assertEqual(r0.status_code, 200)
        self.assertEqual(r1.status_code, 200)
        self.assertEqual(r1.get_json()["lesson_chunks"], 2)

        mod.LESSON_MAX_CHUNKS = 1
        a0 = client.post("/api/voice-chunk", headers=self._hdr("capL2", 0), data=_wav(10))
        a1 = client.post("/api/voice-chunk", headers=self._hdr("capL2", 1), data=_wav(10))
        self.assertEqual(a0.status_code, 200)
        self.assertEqual(a1.status_code, 413)
        self.assertNotIn("capL2", mod._lesson_sessions)

    def test_garbage_chunk_400_no_file(self):
        mod, client, lessons = self._lesson_app()
        resp = client.post("/api/voice-chunk", headers=self._hdr("bad", 0),
                           data=b"GARBAGE" * 20)
        self.assertEqual(resp.status_code, 400)
        self.assertFalse(os.path.isdir(lessons) and os.listdir(lessons))
        self.assertEqual(mod._lesson_sessions, {})


class DeviceIpTest(unittest.TestCase):
    """POST /api/device-ip и необязательный X-Device-Ip на фото-роутах."""

    def _cam_ip(self, mod):
        with mod._photo_lock:
            return mod._cam_ip

    def test_stores_stripped_ip(self):
        mod, client = fresh_app()
        resp = client.post("/api/device-ip",
                           headers=dict(HEADERS, **{"X-Device-Ip": "  192.168.1.42 "}))
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(resp.get_json(), {"ok": True, "cam_ip": "192.168.1.42"})
        self.assertEqual(self._cam_ip(mod), "192.168.1.42")

    def test_missing_header_400(self):
        _, client = fresh_app()
        resp = client.post("/api/device-ip", headers=HEADERS)
        self.assertEqual(resp.status_code, 400)
        self.assertIn("error", resp.get_json())

    def test_invalid_header_400_keeps_previous(self):
        mod, client = fresh_app()
        client.post("/api/device-ip",
                    headers=dict(HEADERS, **{"X-Device-Ip": "192.168.1.7"}))
        for bad in ("x" * 46, "192.168.1.5\x07", "192.168.1.5\t9"):
            with self.subTest(bad=bad[:12]):
                resp = client.post("/api/device-ip",
                                   headers=dict(HEADERS, **{"X-Device-Ip": bad}))
                self.assertEqual(resp.status_code, 400)
                self.assertIn("error", resp.get_json())
        self.assertEqual(self._cam_ip(mod), "192.168.1.7")

    def test_photo_frame_updates_cam_ip(self):
        mod, client = fresh_app()
        resp = client.post("/api/photo-frame",
                           headers=dict(HEADERS, **{"X-Device-Ip": "10.0.0.7"}),
                           data=FAKE_JPEG)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(self._cam_ip(mod), "10.0.0.7")

    def test_photo_frame_invalid_header_ignored(self):
        mod, client = fresh_app()
        resp = client.post("/api/photo-frame",
                           headers=dict(HEADERS, **{"X-Device-Ip": "x" * 100}),
                           data=FAKE_JPEG)
        self.assertEqual(resp.status_code, 200)
        self.assertEqual(self._cam_ip(mod), "")

    def test_legacy_photo_routes_update_cam_ip(self):
        mod, client = fresh_app()
        with mock.patch.object(mod, "call_gemini", return_value="x"):
            r1 = client.post("/api/photo",
                             headers=dict(HEADERS, **{"X-Device-Ip": "10.0.0.1"}),
                             data=FAKE_JPEG)
            self.assertEqual(r1.status_code, 200)
            self.assertEqual(self._cam_ip(mod), "10.0.0.1")
            b64 = base64.b64encode(FAKE_JPEG).decode("ascii")
            r2 = client.post("/api/photos",
                             headers=dict(HEADERS, **{"X-Device-Ip": "10.0.0.2"}),
                             json={"images": [b64]})
            self.assertEqual(r2.status_code, 200)
            self.assertEqual(self._cam_ip(mod), "10.0.0.2")
            client.post("/api/photo-frame", headers=HEADERS, data=FAKE_JPEG)
            r3 = client.post("/api/photos/commit",
                             headers=dict(HEADERS, **{"X-Device-Ip": "10.0.0.3"}))
            self.assertEqual(r3.status_code, 200)
            self.assertEqual(self._cam_ip(mod), "10.0.0.3")


class PhotoStatusCamIpTailTest(unittest.TestCase):
    def test_tail_keys_and_compact_json(self):
        _, client = fresh_app()
        client.post("/api/device-ip",
                    headers=dict(HEADERS, **{"X-Device-Ip": "192.168.4.9"}))
        resp = client.get("/api/photo-status", headers=HEADERS)
        self.assertEqual(resp.mimetype, "application/json")
        raw = resp.get_data(as_text=True)
        body = json.loads(raw)
        self.assertEqual(list(body)[-2:], ["cam_ip", "voice_recording"])
        self.assertEqual(body["cam_ip"], "192.168.4.9")
        self.assertFalse(body["voice_recording"])
        self.assertTrue(raw.endswith('"cam_ip":"192.168.4.9","voice_recording":false}'))
        self.assertNotIn('": ', raw)
        self.assertNotIn(", ", raw)


class VoiceRecordingStatusTest(unittest.TestCase):
    def _hdr(self, sid, idx, final=False):
        d = dict(HEADERS, **{"X-Session-Id": sid, "X-Chunk-Index": str(idx),
                             "Content-Type": "audio/wav"})
        if final:
            d["X-Final"] = "true"
        return d

    def test_true_after_non_final_false_after_final(self):
        mod, client = fresh_app()
        r0 = client.post("/api/voice-chunk", headers=self._hdr("vrec1", 0), data=_wav(10))
        self.assertEqual(r0.status_code, 200)
        self.assertTrue(
            client.get("/api/photo-status", headers=HEADERS).get_json()["voice_recording"])
        with mock.patch.object(mod, "call_gemini", return_value="v"):
            r1 = client.post("/api/voice-chunk", headers=self._hdr("vrec1", 1, final=True),
                             data=_wav(10))
        self.assertEqual(r1.status_code, 200)
        self.assertFalse(
            client.get("/api/photo-status", headers=HEADERS).get_json()["voice_recording"])

    def test_lesson_path_marks_recording(self):
        mod, client = fresh_app()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod.LESSONS_DIR = tmp.name

        def _close_sessions():
            with mod._lesson_lock:
                for sid in list(mod._lesson_sessions):
                    mod._finalize_lesson_locked(sid, time.time(), reason="test")

        # LIFO: сначала закрываем открытые файлы, потом удаляем временную папку.
        self.addCleanup(_close_sessions)
        resp = client.post("/api/voice-mode", headers=HEADERS, json={"mode": "lesson"})
        self.assertEqual(resp.status_code, 200)
        client.post("/api/voice-chunk", headers=self._hdr("vless", 0), data=_wav(10))
        self.assertTrue(
            client.get("/api/photo-status", headers=HEADERS).get_json()["voice_recording"])
        client.post("/api/voice-chunk", headers=self._hdr("vless", 1, final=True),
                    data=_wav(10))
        self.assertFalse(
            client.get("/api/photo-status", headers=HEADERS).get_json()["voice_recording"])

    def test_window_expiry_false(self):
        mod, client = fresh_app()
        client.post("/api/voice-chunk", headers=self._hdr("vwin", 0), data=_wav(10))
        with mod._voice_activity_lock:
            mod._last_voice_chunk_at = time.monotonic() - mod.VOICE_ACTIVE_WINDOW - 1
        self.assertFalse(
            client.get("/api/photo-status", headers=HEADERS).get_json()["voice_recording"])

    def test_default_window_12(self):
        os.environ.pop("VOICE_ACTIVE_WINDOW", None)
        mod, _ = fresh_app()
        self.assertEqual(mod.VOICE_ACTIVE_WINDOW, 12)


if __name__ == "__main__":
    unittest.main()
