"""
Kokoro TTS tests for Lemonade Server.

Tests the /audio/speech endpoint.

Usage:
    python server_tts.py
    python server_tts.py --wrapped-server audio.cpp --backend rocm
    python server_tts.py --cli-binary /path/to/lemonade
"""

import base64
import requests

from utils.capabilities import (
    get_current_config,
    get_test_model,
    supports,
)
from utils.server_base import (
    ServerTestBase,
    run_server_tests,
)
from utils.test_models import (
    TTS_MODEL,
    PORT,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
)


def tts_model():
    """Kokoro by default; the --wrapped-server backend's own model when set."""
    if get_current_config()[0] is None:
        return TTS_MODEL
    return get_test_model("tts")


def active_supports(feature):
    """Kokoro serves everything this script asks for; narrower backends declare less."""
    wrapped_server, backend, modality = get_current_config()
    if wrapped_server is None:
        return True
    return supports(feature, wrapped_server, modality)


class TextToSpeechTests(ServerTestBase):
    """Tests for Text to Speech."""

    def test_001_basic_tts(self):
        """Test basic speech generation."""
        response_format = "mp3" if active_supports("tts_mp3") else "wav"
        payload = {
            "model": tts_model(),
            "input": "Lemonade can speak",
            "response_format": response_format,
        }

        print(f"[INFO] Sending speech generation request with model {tts_model()}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        magic = b"ID3" if response_format == "mp3" else b"RIFF"
        self.assertTrue(
            response.content[: len(magic)] == magic,
            f"Decoded data should be a valid {response_format.upper()}",
        )

        print(f"[OK] Speech generation successful")

    def test_002_missing_input_error(self):
        """Test error handling when input is missing."""
        payload = {
            "model": tts_model(),
            # No prompt
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error
        self.assertIn(
            response.status_code,
            [400, 422],
            f"Expected 400 or 422 for missing prompt, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected request without prompt: {response.status_code}")

    def test_003_invalid_model_error(self):
        """Test error handling with invalid model."""
        payload = {
            "model": "kokoro-no-junbi-mada-dekite-inai",
            "input": "Lemonade can speak",
        }

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_DEFAULT,
        )

        # Should return an error (model not found)
        # Note: Server may return 500 for model not found, ideally should be 404
        self.assertIn(
            response.status_code,
            [400, 404, 422, 500],
            f"Expected error for invalid model, got {response.status_code}",
        )
        print(f"[OK] Correctly rejected invalid model: {response.status_code}")

    def test_004_tts_voice(self):
        """Test voice selection."""
        response_format = "opus" if active_supports("tts_opus") else "wav"
        payload = {
            "model": tts_model(),
            "input": "Lemonade can speak",
            "voice": "onyx",
            "response_format": response_format,
        }

        print(f"[INFO] Sending speech generation request with model {tts_model()}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        magic = b"OggS" if response_format == "opus" else b"RIFF"
        self.assertTrue(
            response.content[: len(magic)] == magic,
            f"Decoded data should be a valid {response_format.upper()}",
        )

        print(f"[OK] Speech generation successful")

    def test_005_tts_stream(self):
        """Test streaming speech generation with Kokoro."""
        payload = {
            "model": tts_model(),
            "input": "Lemonade can speak",
            "voice": "onyx",
            "stream_format": "audio",
        }

        print(f"[INFO] Sending speech generation request with model {tts_model()}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        content_type = response.headers.get("Content-Type", "")
        self.assertTrue(
            content_type.startswith("audio/l16"),
            f"Streamed audio should be declared as raw PCM, got '{content_type}'",
        )

        # Kokoros streams headerless s16le. A container magic here would mean the
        # bytes and the Content-Type disagree.
        for magic, kind in ((b"RIFF", "WAV"), (b"ID3", "MP3"), (b"OggS", "Ogg")):
            self.assertFalse(
                response.content.startswith(magic),
                f"Streamed body declared as PCM but starts with {kind} magic",
            )

        self.assertGreater(
            len(response.content), 1000, "Streamed clip should be substantial"
        )

        print(f"[OK] Speech generation successful")

    def test_006_tts_speed(self):
        """Test the speech speed multiplier."""
        if not active_supports("tts_speed"):
            self.skipTest("Skipping: speed not supported by the active TTS backend")
        payload = {
            "model": tts_model(),
            "input": "Lemonade can speak",
            "speed": 0.75,
            "response_format": "wav",
        }

        print(f"[INFO] Sending speech generation request with model {tts_model()}")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        self.assertEqual(
            response.status_code,
            200,
            f"Speech generation failed with status {response.status_code}: {response.text}",
        )

        # WAV files start with specific magic bytes
        self.assertTrue(
            response.content[:4] == b"RIFF",
            "Decoded data should be a valid WAV file",
        )

        print(f"[OK] Speech generation successful")

    def test_007_tts_stream_rejects_unstreamable_format(self):
        """A format the backend can only emit buffered must not be streamed."""
        payload = {
            "model": tts_model(),
            "input": "Lemonade can speak",
            "stream_format": "audio",
            "response_format": "mp3",
        }

        print(f"[INFO] Requesting streamed mp3 from {tts_model()} (expected to fail)")

        response = requests.post(
            f"{self.base_url}/audio/speech",
            json=payload,
            timeout=TIMEOUT_MODEL_OPERATION,
        )

        # Kokoros forces raw PCM on its streaming path whatever the request asks
        # for, so serving this as mp3 would hand back undecodable bytes under a
        # Content-Type that does not match them.
        self.assertEqual(
            response.status_code,
            400,
            "Streaming a format the backend cannot encode should be rejected, "
            f"got {response.status_code} with Content-Type "
            f"'{response.headers.get('Content-Type', '')}'",
        )

        print(f"[OK] Unstreamable format rejected")


if __name__ == "__main__":
    run_server_tests(TextToSpeechTests, "TTS TESTS", modality="tts")
