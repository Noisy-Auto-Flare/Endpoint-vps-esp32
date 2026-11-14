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
        for method, path, kwargs in [
            ("post", "/api/photo-frame", {"data": FAKE_JPEG}),
            ("get", "/api/latest", {}),
            ("get", "/api/photo-status", {}),
            ("post", "/api/photos/commit", {}),
        ]:
            with self.subTest(path=path):
                resp = getattr(client, method)(path, **kwargs)
                self.assertEqual(resp.status_code, 401)
                self.assertEqual(resp.get_json()["error"], "unauthorized")


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
        self.assertEqual(body, {"staged": 0, "photo_status": "idle", "answer_ready": False, "voice_busy": False})

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
        second = client.get("/api/latest", headers=HEADERS)
        body2 = json.loads(second.get_data(as_text=True))
        self.assertEqual(second.status_code, 200)
        self.assertFalse(body2["ready"])


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
        second = json.loads(client.get("/api/latest", headers=HEADERS).get_data(as_text=True))
        self.assertTrue(second["ready"])
        self.assertEqual(second["text"], "photo-answer")
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

    def test_list_before_data_raises(self):
        mod, _ = fresh_app()
        extra = b"LIST" + struct.pack("<I", 4) + b"abcd"
        bad = _build_wav(b"\x01\x02" * 10, extra_before_data=extra)
        with self.assertRaises(RuntimeError) as cm:
            mod.merge_wav_chunks([bad])
        self.assertIn("unsupported wav format", str(cm.exception))

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


if __name__ == "__main__":
    unittest.main()
