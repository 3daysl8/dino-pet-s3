"""Who's talking: speaker recognition so Deadlee knows Sam (and the rest of the family).

Each person's voiceprint is a few speaker embeddings (WeSpeaker ResNet34 via
sherpa-onnx, CPU, ~26 MB) saved as brain/voices/<name>.npy, one row per
enrolment clip. A clip is matched by cosine similarity to each person's average.

Voiceprints are biometric: they stay on the server (not in the PC backup, never
sent anywhere), and the enrolment audio itself is not kept.
"""
import logging
import os
import threading

import httpx
import numpy as np

import brain

log = logging.getLogger("pet-bridge")

MODEL_URL = ("https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
             "wespeaker_en_voxceleb_resnet34_LM.onnx")
# Tune on real clips from the toy. Synthetic test voices: same voice 0.92, other voices 0.47-0.57.
THRESHOLD = float(os.environ.get("PET_SPEAKER_THRESHOLD", "0.65"))
MARGIN = float(os.environ.get("PET_SPEAKER_MARGIN", "0.08"))          # best must beat second by this
# Below this: someone else. Between this and THRESHOLD: unclear, stay with the conversation's
# speaker. Kept narrow: calling someone by the wrong name is worse than not using a name.
CLEARLY_OTHER = float(os.environ.get("PET_SPEAKER_OTHER", "0.55"))
MIN_SECONDS = 1.0                                                     # shorter clips aren't judged
ENROL_CLIPS = 5

_extractor = None
_lock = threading.Lock()


def _model_path() -> str:
    return os.path.join(brain.DATA, "models", "wespeaker_en_voxceleb_resnet34_LM.onnx")


def _load():
    global _extractor
    if _extractor is not None:
        return _extractor
    with _lock:
        if _extractor is None:
            import sherpa_onnx
            path = _model_path()
            if not os.path.exists(path):
                os.makedirs(os.path.dirname(path), exist_ok=True)
                log.info("downloading the speaker model")
                with httpx.stream("GET", MODEL_URL, follow_redirects=True, timeout=120) as r:
                    r.raise_for_status()
                    with open(path + ".part", "wb") as f:
                        for chunk in r.iter_bytes():
                            f.write(chunk)
                os.replace(path + ".part", path)
            cfg = sherpa_onnx.SpeakerEmbeddingExtractorConfig(model=path, num_threads=2)
            _extractor = sherpa_onnx.SpeakerEmbeddingExtractor(cfg)
    return _extractor


def embed(samples: np.ndarray) -> np.ndarray | None:
    """16 kHz mono float32 samples -> unit-length embedding (None if too short)."""
    if len(samples) < 16000 * MIN_SECONDS:
        return None
    ex = _load()
    stream = ex.create_stream()
    stream.accept_waveform(16000, samples.astype(np.float32))
    stream.input_finished()
    if not ex.is_ready(stream):
        return None
    v = np.array(ex.compute(stream), dtype=np.float32)
    n = float(np.linalg.norm(v))
    return v / n if n else None


# ---------------------------------------------------------------- voiceprints

def _voice_file(name: str) -> str:
    return brain.path("voices", name + ".npy")


def voices() -> dict[str, np.ndarray]:
    """name -> average unit embedding."""
    out = {}
    try:
        files = os.listdir(brain.path("voices"))
    except OSError:
        return out
    for f in files:
        if f.endswith(".npy"):
            rows = np.load(os.path.join(brain.path("voices"), f))
            if rows.ndim == 2 and len(rows):
                m = rows.mean(axis=0)
                out[f[:-4]] = m / np.linalg.norm(m)
    return out


def add_clip(name: str, emb: np.ndarray) -> int:
    """Add one enrolment embedding; returns how many clips that person now has."""
    os.makedirs(brain.path("voices"), exist_ok=True)
    p = _voice_file(name)
    rows = np.load(p) if os.path.exists(p) else np.zeros((0, len(emb)), dtype=np.float32)
    rows = np.vstack([rows, emb[None, :]])[-20:]          # keep the latest 20 clips
    np.save(p, rows)
    return len(rows)


def forget(name: str | None = None) -> list[str]:
    gone = []
    for n in list(voices()) if name is None else [name]:
        try:
            os.remove(_voice_file(n))
            gone.append(n)
        except OSError:
            pass
    return gone


KEEP = "keep"            # carry the conversation's speaker over (clip too short or unclear)


def who_said(emb: np.ndarray | None, prints: dict[str, np.ndarray] | None = None) -> tuple[str | None, float]:
    """A name, None (clearly not anyone enrolled, or nobody enrolled) or KEEP."""
    if emb is None:
        return KEEP, 0.0
    prints = voices() if prints is None else prints
    if not prints:
        return None, 0.0
    name, score = identify(emb, prints)
    if name:
        return name, score
    return (None if score < CLEARLY_OTHER else KEEP), score


def identify(emb: np.ndarray | None, prints: dict[str, np.ndarray] | None = None) -> tuple[str | None, float]:
    """Best match if it's above THRESHOLD and clearly ahead of the next person."""
    if emb is None:
        return None, 0.0
    prints = voices() if prints is None else prints
    if not prints:
        return None, 0.0
    scores = sorted(((float(np.dot(emb, v)), n) for n, v in prints.items()), reverse=True)
    best, name = scores[0]
    second = scores[1][0] if len(scores) > 1 else -1.0
    if best >= THRESHOLD and best - second >= MARGIN:
        return name, best
    return None, best


# ---------------------------------------------------------------- enrolment by voice

ENROL_LINES = [
    "Let's learn your voice. First, tell me your favourite animal.",
    "Great! Now count from one to five.",
    "Nice. What did you have for breakfast?",
    "Two more. Say: the quick brown fox jumps over the lazy dog.",
    "Last one! Tell me something that makes you laugh.",
]


class Enrolment:
    """'Deadlee, learn my voice, I'm Sam' -> the next ENROL_CLIPS recordings are voice samples."""

    def __init__(self):
        self.name: str | None = None
        self.got = 0
        self.started = 0.0

    def active(self, now: float) -> bool:
        if self.name and now - self.started > 5 * 60:      # walked away: give up
            log.info("voice enrolment for %s timed out", self.name)
            self.name = None
        return self.name is not None

    def start(self, name: str, now: float) -> str:
        self.name, self.got, self.started = name, 0, now
        return ENROL_LINES[0]

    def feed(self, emb: np.ndarray | None) -> str:
        """One clip; returns what Deadlee says next."""
        if emb is None:
            return "That was a bit short. Say it again, a bit longer?"
        self.got += 1
        add_clip(self.name, emb)
        if self.got < ENROL_CLIPS:
            return ENROL_LINES[self.got]
        name, self.name = self.name, None
        log.info("voice enrolled: %s", name)
        return f"Got it, {name.title()}! Now I'll know when it's you talking."
