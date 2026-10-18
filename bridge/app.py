"""pet-bridge: Deadlee's voice/chat brain, running on the server (home LAN only).

POST /talk   (header X-Pet-Token)
    body: audio/wav (16 kHz mono 16-bit, from the pet's mic)  -> Whisper -> Gemma -> TTS
       or application/json {"text": "..."}                    -> Gemma -> TTS
    optional header X-Pet-State: JSON with the pet's current state
    returns {"heard", "reply", "audio": "/audio/<id>.mp3", "actions": [...]}
    actions are things for the toy to do: timer, alarm, nightlight, radio (see tools.py).
GET  /audio/<id>.mp3   the spoken reply (unguessable id, kept 10 min; the pet's
                       player can't send headers, so this one is token-free)
POST /talk?mode=wake    a clip the pet heard on its own (plugged in, voice detector):
    answered only if it starts with "hey/hi/hello <name>"; otherwise {"wake": false},
    nothing logged and nothing kept. "Hey Deadlee" alone gets a short "Yes?".
POST /talk?mode=follow  a clip right after a reply (no wake word needed); silence or
    a stray noise gets {"wake": false}.
POST /forget  (header X-Pet-Token)   wipe Deadlee's own notes (FAMILY.md and the parent's parts stay)
GET  /voices, POST /voices/enrol?name= (WAV body), POST /voices/forget[?name=]   (header X-Pet-Token)
     voiceprints for "who's talking" (speaker.py); also "learn my voice, I'm Sam" by voice
GET  /memory  (header X-Pet-Token)   the brain files
GET  /recommendations (header X-Pet-Token)   the nightly "what I couldn't do" digest
GET  /ui      the web page: chat by text, edit the brain files and skills (ui.html; it asks for the token)
POST /talk/new, GET /brain/files, GET|PUT|DELETE /brain/file?path=   (header X-Pet-Token)   used by the page
GET  /calendar, POST /calendar/sync (header X-Pet-Token)   the family calendar as Deadlee sees it; sync it now
GET|POST|DELETE /books (header X-Pet-Token)   the bookshelf Deadlee reads aloud from (books.py)
GET|DELETE /stories (header X-Pet-Token)      stories made up with the child (stories.py)
While a story is being made, /talk replies also carry "story": {title, scene, of, mood, ideas, done}.
GET|DELETE /maths (header X-Pet-Token)        maths game levels and recent games (maths.py)
During a maths game, /talk replies carry "maths": {question, n, of, right, level, done}.
GET  /health

The brain is Gemma on this machine (llama-swap) with the files in /data/brain
(see brain.py). If Gemma is down, Claude Haiku answers with the persona and the
current message only: no notes, family data, history or speaker.
"""
import io
import json
import logging
import os
import re
import secrets
import threading
import time
import wave

import difflib

import httpx
import numpy as np
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse
from faster_whisper import WhisperModel

import autosave
import books
import brain
import calendar_sync
import family
import llm
import maths
import modes
import prompt
import routines
import speaker
import stories
import tools
from concurrent.futures import ThreadPoolExecutor

log = logging.getLogger("pet-bridge")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

DATA = os.environ.get("PET_DATA", "/data")
AUDIO_DIR = os.path.join(DATA, "audio")
os.makedirs(AUDIO_DIR, exist_ok=True)

PET_TOKEN = open(os.path.join(DATA, "pet.token")).read().strip()
TTS_URL = os.environ.get("PET_TTS_URL", "http://127.0.0.1:8880/v1/audio/speech")
TTS_VOICE = os.environ.get("PET_VOICE", "bm_daniel")
TTS_SPEED = float(os.environ.get("PET_VOICE_SPEED", "1.1"))
READING_S = 15 * 60                                             # "keep going" works this long after a page
HISTORY_TURNS = 6                                               # user+assistant pairs kept
HISTORY_IDLE_S = int(os.environ.get("PET_CONVO_IDLE_S", "600"))   # a new conversation after 10 min quiet
AUDIO_KEEP_S = 10 * 60
MAX_AUDIO_BYTES = 16000 * 2 * 20                                # 20 s of 16 kHz mono 16-bit

app = FastAPI()
brain.setup()
routines.start()
whisper = WhisperModel(os.environ.get("PET_WHISPER", "small.en"), device="cpu",
                       compute_type="int8", download_root=os.path.join(DATA, "whisper"))

_lock = threading.Lock()
_history: list[dict] = []
_last_talk = 0.0
_snap: brain.Snapshot | None = None
_speaker: str | None = None                  # who this conversation is with (kept across short clips)
_enrol = speaker.Enrolment()
_pool = ThreadPoolExecutor(max_workers=1)
_transcript: list[dict] = []                 # this conversation, for autosave when it ends
_reading = 0.0                               # when a book page was last read aloud


def _flush_transcript() -> None:
    """Hand the finished conversation to autosave (call with _lock held)."""
    if _transcript:
        autosave.fold_later(_transcript)
        _transcript.clear()


def _idle_watch() -> None:
    """A conversation that just stops (nobody talks for 10 min) gets saved too."""
    while True:
        time.sleep(60)
        with _lock:
            if _transcript and time.time() - _last_talk > HISTORY_IDLE_S:
                _flush_transcript()


threading.Thread(target=_idle_watch, name="autosave-watch", daemon=True).start()


def ui_password() -> str:
    """An easy password for the web page (ui.password in the data folder; no file = none).
    Read each time, so changing or deleting the file needs no restart."""
    try:
        return open(os.path.join(DATA, "ui.password"), encoding="utf-8").read().strip().lower()
    except OSError:
        return ""


def check_token(token: str | None) -> None:
    given = (token or "").strip().encode()
    pw = ui_password().encode()                 # typed on a phone, so capitals don't matter
    if given and (secrets.compare_digest(given, PET_TOKEN.encode()) or
                  (pw and secrets.compare_digest(given.lower(), pw))):
        return
    raise HTTPException(status_code=401, detail="bad token")


def pet_name(state: dict) -> str:
    name = re.sub(r"[^A-Za-z '\-]", "", str(state.get("name", ""))).strip()[:16]
    return name or "Deadlee"


GREETINGS = {"hey", "hi", "hello", "ok", "okay", "oi", "hiya", "yo"}


def match_wake(text: str, name: str) -> str | None:
    """If text starts with a greeting + the dino's name (or the name alone, said clearly),
    return what came after it (may be ""). Otherwise None."""
    words = re.findall(r"[a-z0-9']+", text.lower())
    target = re.sub(r"[^a-z]", "", name.lower())
    if not words or not target:
        return None

    def sounds_like(i: int) -> int:
        """How many words from i make up the name (0 = not the name)."""
        best, best_n = 0.0, 0
        for n in (1, 2):                                   # "Dead Lee" can come out as two words
            if i + n <= len(words):
                r = difflib.SequenceMatcher(None, "".join(words[i:i + n]), target).ratio()
                if r > best:
                    best, best_n = r, n
        return best_n if best >= 0.6 else 0

    for g in range(min(2, len(words))):                   # greeting in the first two words
        if words[g] in GREETINGS:
            n = sounds_like(g + 1)
            if n:
                return " ".join(words[g + 1 + n:])
    if difflib.SequenceMatcher(None, words[0], target).ratio() >= 0.8:   # "Deadlee, ..." on its own
        return " ".join(words[1:])
    return None


def decode_wav(wav_bytes: bytes) -> np.ndarray:
    # Decode the WAV ourselves (no ffmpeg/PyAV): Whisper and the voiceprints take 16 kHz float samples.
    try:
        with wave.open(io.BytesIO(wav_bytes)) as w:
            if w.getnchannels() != 1 or w.getsampwidth() != 2:
                raise HTTPException(status_code=400, detail="need mono 16-bit wav")
            rate = w.getframerate()
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    except wave.Error:
        raise HTTPException(status_code=400, detail="not a wav file")
    samples = pcm.astype(np.float32) / 32768.0
    peak = float(np.abs(samples).max()) if len(samples) else 0.0
    if 0.01 < peak < 0.5:                       # quiet voice (not just room hiss): bring it up
        samples *= 0.9 / peak
    if rate != 16000 and len(samples):
        samples = np.interp(np.arange(0, len(samples), rate / 16000), np.arange(len(samples)),
                            samples).astype(np.float32)
    return samples


def transcribe(samples: np.ndarray, hint: str | None = None) -> str:
    segments, _ = whisper.transcribe(samples, language="en", beam_size=1, vad_filter=True,
                                     initial_prompt=hint)
    return " ".join(s.text.strip() for s in segments).strip()


def voiceprint(samples: np.ndarray):
    try:
        return speaker.embed(samples)
    except Exception as e:  # noqa: BLE001 - recognition is a nice-to-have; never fail a reply
        log.error("speaker embedding failed: %s", e)
        return None


def speak(text: str, speed: float = TTS_SPEED) -> str | None:
    try:
        r = httpx.post(TTS_URL, json={"model": "kokoro", "input": text, "voice": TTS_VOICE,
                                      "response_format": "mp3", "speed": speed}, timeout=30.0)
        r.raise_for_status()
    except Exception as e:  # noqa: BLE001
        log.error("TTS failed: %s", e)
        return None
    name = secrets.token_urlsafe(12) + ".mp3"
    with open(os.path.join(AUDIO_DIR, name), "wb") as f:
        f.write(r.content)
    return f"/audio/{name}"


def prune_audio() -> None:
    cutoff = time.time() - AUDIO_KEEP_S
    for n in os.listdir(AUDIO_DIR):
        p = os.path.join(AUDIO_DIR, n)
        if os.path.getmtime(p) < cutoff:
            os.remove(p)


@app.get("/health")
def health():
    return {"ok": True, "llm": "local", "chat_model": llm.CHAT_MODEL, "local_up": llm.local_up(),
            "voice": TTS_VOICE, **llm.stats}


def answer(heard: str, state: dict, who: str | None = speaker.KEEP) -> tuple[str, tools.Context]:
    """One turn of conversation. who: a name, None (someone not enrolled) or speaker.KEEP
    (unclear: stay with the conversation's speaker). Returns the reply and the tool context
    (actions for the toy, a voice to learn, a turn to speak instead of the reply, tools used)."""
    global _last_talk, _snap, _speaker
    with _lock:
        if time.time() - _last_talk > HISTORY_IDLE_S:
            _flush_transcript()                 # the last conversation is over: keep what mattered
            _history.clear()
            _snap = None
        if _snap is None:                       # a new conversation (maybe begun with a book): fresh snapshot
            _speaker = None if who == speaker.KEEP else who
            _snap = brain.snapshot(_speaker)
        elif who != speaker.KEEP and who != _speaker:   # someone else now: personalise for them
            _speaker = who
            _snap = brain.snapshot(who)
        _last_talk = time.time()
        ctx = tools.Context(speaker=_speaker, names=family.names(_snap.family))
        text, private = llm.reply(_snap, list(_history), heard, state, ctx)
        reply = prompt.tidy(text)
        if ctx.turn:                            # a book page, story scene or maths turn: the bubble
            reply = ctx.turn["reply"]           # gets its short line, its own words are spoken (see talk)
        if len(reply.split()) > prompt.MAX_WORDS + 15:
            brain.log_event("long_reply", f"{len(reply.split())} words")
        if private:                             # backup replies never join the history
            _history.extend([{"role": "user", "content": heard}, {"role": "assistant", "content": reply}])
            del _history[: max(0, len(_history) - 2 * HISTORY_TURNS)]
        _transcript.append({"speaker": _speaker, "heard": heard, "reply": reply})
        del _transcript[: max(0, len(_transcript) - autosave.MAX_TURNS)]
        if re.search(r"\b(i'm not sure|i don't know|not sure)\b", reply, re.I):
            brain.log_event("unsure", ",".join(ctx.used))
    return reply, ctx


def do_ritual(kind: str, state: dict, t0: float) -> dict:
    with _lock:
        snap = brain.snapshot(_speaker)
    try:
        reply = prompt.tidy(routines.ritual(kind, snap, state, _speaker))
    except llm.LocalError as e:
        log.error("ritual %s: %s", kind, e)
        reply = "Good morning!" if kind == "morning" else "Goodnight, sleep tight!"
    audio = speak(reply)
    log.info("ritual %s %.1fs reply=%r", kind, time.time() - t0, reply)
    return {"wake": True, "heard": "", "reply": reply, "audio": audio, "actions": []}


@app.post("/talk")
async def talk(request: Request, x_pet_token: str | None = Header(None),
               x_pet_state: str | None = Header(None)):
    check_token(x_pet_token)
    t0 = time.time()
    mode = request.query_params.get("mode", "")
    body = await request.body()
    if len(body) > MAX_AUDIO_BYTES + 1024:
        raise HTTPException(status_code=413, detail="too long")
    try:
        state = json.loads(x_pet_state) if x_pet_state else {}
    except json.JSONDecodeError:
        state = {}
    emb = None
    is_audio = not request.headers.get("content-type", "").startswith("application/json")
    if not is_audio:
        req = json.loads(body or b"{}")
        kind = str(req.get("routine", ""))
        if kind in routines.RITUALS:              # the toy's good-morning / goodnight moment
            return JSONResponse(do_ritual(kind, state, t0))
        heard = str(req.get("text", "")).strip()[:300]
    else:
        samples = decode_wav(body)
        fp = _pool.submit(voiceprint, samples)        # who's talking, alongside Whisper
        heard = transcribe(samples, f"Hey {pet_name(state)}." if mode == "wake" else None)
        emb = fp.result()
    t_stt = time.time()

    if mode == "wake":                     # overheard: only answer if it was meant for Deadlee
        rest = match_wake(heard, pet_name(state))
        if rest is None:
            log.info("wake: no (%.1fs)", time.time() - t0)   # never log what was said
            return JSONResponse({"wake": False})
        log.info("wake: yes")
        if not rest:
            return JSONResponse({"wake": True, "heard": heard, "reply": prompt.LINES["wake"],
                                 "audio": speak(prompt.LINES["wake"]), "actions": []})
        heard = rest
    elif mode == "follow" and len(re.sub(r"[^a-z]", "", heard.lower())) < 2:
        return JSONResponse({"wake": False})   # silence or a stray noise after a reply

    global _reading, _last_talk, _snap
    actions: list[dict] = []
    turn, used = None, []
    who, score = speaker.who_said(emb) if is_audio else (speaker.KEEP, 0.0)
    if not is_audio and req.get("speaker"):        # the web page says who is typing
        who = brain.resolve_person(str(req["speaker"])) or speaker.KEEP
    if is_audio:
        log.info("speaker: %s (%.2f)", who or "unknown", score)
    if _enrol.active(time.time()):                 # learning a voice: this clip is a sample, not a question
        if re.search(r"\b(stop|cancel|never mind)\b", heard, re.I):
            _enrol.name = None
            reply = "Okay, we'll do it another time."
        elif not is_audio:
            reply = "Use the mic so I can hear your voice."
        else:
            reply = _enrol.feed(emb)               # None (too short) asks again
    elif not heard:
        reply = prompt.LINES["didnt_catch"]
    else:
        by = (who if isinstance(who, str) and who != speaker.KEEP else _speaker) or ""
        # The bookshelf, the story maker and the maths game are routed in code (modes.py).
        try:
            turn = modes.handle(heard, by, reading=time.time() - _reading < READING_S)
        except Exception as e:  # noqa: BLE001 - a bad book or story file must never break a reply
            log.exception("mode routing failed")
            brain.log_event("mode_error", type(e).__name__)
        if turn:
            reply = turn["reply"]
            with _lock:
                if time.time() - _last_talk > HISTORY_IDLE_S:   # a new conversation starts here, so
                    _flush_transcript()                         # the next chat reply re-reads the brain
                    _history.clear()
                    _snap = None
                _last_talk = time.time()
                # ...and the chat model hears about it, or "stop the book" gets "what book?"
                _history.extend([{"role": "user", "content": heard}, {"role": "assistant", "content": reply}])
                del _history[: max(0, len(_history) - 2 * HISTORY_TURNS)]
        else:
            reply, ctx = answer(heard, state, who)
            actions, used, turn = ctx.actions, ctx.used, ctx.turn
            if ctx.enrol:
                reply = f"Okay {ctx.enrol.title()}! {_enrol.start(ctx.enrol, time.time())}"
    t_llm = time.time()
    turn = turn or {}
    page = turn.get("page")
    if heard:                                      # a press with nothing said leaves "keep going" / "stop" working
        _reading = time.time() if page and not page["last"] else 0.0     # after "The end." nothing is left to keep going with
    if turn:                                       # a page, scene or maths turn: its own words, at its own pace
        audio = speak(turn["speak"], turn.get("speed", TTS_SPEED))
    else:
        audio = speak(reply, tools.speed_for(used, TTS_SPEED))     # slower for jokes and look-ups, if set
    prune_audio()
    log.info("talk %.1fs (stt %.1f, brain %.1f, tts %.1f) heard=%r reply=%r actions=%s",
             time.time() - t0, t_stt - t0, t_llm - t_stt, time.time() - t_llm, heard, reply,
             [a["type"] for a in actions])
    out = {"wake": True, "heard": heard, "reply": reply, "audio": audio, "actions": actions}
    if turn.get("story"):
        out["story"] = turn["story"]               # title, scene n of m, mood, the two ideas, done
    if turn.get("maths"):
        out["maths"] = turn["maths"]               # the sum to show, question n of m, right so far, level, done
    return JSONResponse(out)


@app.post("/forget")
def forget(x_pet_token: str | None = Header(None)):
    global _snap
    check_token(x_pet_token)
    brain.forget()
    with _lock:
        _history.clear()
        _transcript.clear()                     # nothing from before the wipe gets saved later
        _snap = None
    log.info("memory wiped")
    return {"ok": True}


@app.get("/memory")
def memory(x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    return brain.dump()


# ---------------------------------------------------------------- the web page

@app.get("/ui")
def ui():
    """Chat with Deadlee and edit the brain from a browser. The page holds no secrets:
    it asks for the pet token and sends it with every call."""
    return FileResponse(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui.html"),
                        media_type="text/html", headers={"Cache-Control": "no-store"})


@app.post("/talk/new")
def talk_new(x_pet_token: str | None = Header(None)):
    """End the conversation now, so the next message reads the brain files afresh."""
    global _snap
    check_token(x_pet_token)
    with _lock:
        _flush_transcript()
        _history.clear()
        _snap = None
    return {"ok": True}


@app.get("/brain/files")
def brain_files(x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    return {"files": brain.list_files(), "people": brain.people()}


@app.get("/brain/file")
def brain_file(path: str, x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    text = brain.read_file(path)
    if text is None:
        raise HTTPException(status_code=404, detail="not a brain file")
    return {"path": path, "text": text, "cap": brain.file_cap(path)}


@app.put("/brain/file")
async def brain_file_save(path: str, request: Request, x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    result = brain.write_file(path, str((await request.json()).get("text", "")))
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    log.info("brain file saved from the web page: %s", path)
    out = {"ok": True, "result": result}
    if path == "FAMILY.md":
        out["check"] = family.check(brain.family_text())
    return out


@app.get("/books")
def books_list(x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    return {"books": books.library()}


@app.post("/books")
async def books_add(request: Request, x_pet_token: str | None = Header(None)):
    """Add a book: {"gutenberg": 14838, "start": "Once upon a time"} fetches a free one from
    Project Gutenberg; {"title", "author", "text"} adds your own."""
    check_token(x_pet_token)
    req = await request.json()
    try:
        if req.get("gutenberg"):
            result = books.add_gutenberg(int(req["gutenberg"]), str(req.get("start") or ""))
        else:
            result = books.add(str(req.get("title") or ""), str(req.get("author") or ""), str(req.get("text") or ""))
    except (httpx.HTTPError, ValueError) as e:
        result = f"error: couldn't get that book ({type(e).__name__})"
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    return {"ok": True, "result": result, "books": books.library()}


@app.post("/books/bookmark")
async def books_bookmark(request: Request, x_pet_token: str | None = Header(None)):
    """Set the page a book will carry on from: {"name": ..., "page": 12}."""
    check_token(x_pet_token)
    req = await request.json()
    try:
        result = books.set_bookmark(str(req.get("name") or ""), int(req.get("page") or 0))
    except (TypeError, ValueError):
        result = "error: page must be a number"
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    return {"ok": True, "books": books.library()}


@app.delete("/books")
def books_remove(name: str, x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    result = books.remove(name)
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    return {"ok": True, "books": books.library()}


@app.get("/maths")
def maths_progress(x_pet_token: str | None = Header(None)):
    """Each child's maths level and recent games (counts only)."""
    check_token(x_pet_token)
    return {"ok": True, "progress": maths.progress(), "settings": maths.settings(), "levels": maths.LEVELS}


@app.delete("/maths")
def maths_reset(name: str, x_pet_token: str | None = Header(None)):
    """Forget a child's maths level and games; the next game starts from the starting level."""
    check_token(x_pet_token)
    result = maths.reset(name)
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    return {"ok": True, "progress": maths.progress(), "settings": maths.settings(), "levels": maths.LEVELS}


@app.get("/stories")
def stories_list(x_pet_token: str | None = Header(None)):
    """The stories made with Deadlee, with their text, for a parent to read."""
    check_token(x_pet_token)
    return _stories_page()


def _stories_page() -> dict:
    return {"ok": True, "stories": stories.library(), "settings": stories.settings(), "friends": stories.friends()}


@app.delete("/stories/friend")
def stories_friend_remove(name: str, x_pet_token: str | None = Header(None)):
    """Stop a story friend from coming back in new stories (old stories keep them)."""
    check_token(x_pet_token)
    result = stories.remove_friend(name)
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    return _stories_page()


@app.delete("/stories")
def stories_remove(id: str, x_pet_token: str | None = Header(None)):
    """Delete a story (and its copy on the bookshelf)."""
    check_token(x_pet_token)
    result = stories.remove(id)
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    return _stories_page()


@app.get("/calendar")
def calendar_view(x_pet_token: str | None = Header(None)):
    """What Deadlee currently knows from the family calendar."""
    check_token(x_pet_token)
    return {**calendar_sync.status(), "view": calendar_sync.view()}


@app.post("/calendar/sync")
def calendar_sync_now(x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    result = calendar_sync.sync()
    return {**calendar_sync.status(), **result, "view": calendar_sync.view()}


@app.delete("/brain/file")
def brain_file_delete(path: str, x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    result = brain.delete_file(path)
    if result.startswith("error"):
        raise HTTPException(status_code=400, detail=result[7:])
    log.info("brain file deleted from the web page: %s", path)
    return {"ok": True}


@app.get("/recommendations")
def recommendations(x_pet_token: str | None = Header(None)):
    check_token(x_pet_token)
    try:
        return PlainTextResponse(open(brain.path("RECOMMENDATIONS.md"), encoding="utf-8").read())
    except OSError:
        return PlainTextResponse("No recommendations yet (the nightly digest hasn't run).")


@app.get("/voices")
def voices_list(x_pet_token: str | None = Header(None)):
    """Who Deadlee knows by voice (names and clip counts only)."""
    check_token(x_pet_token)
    out = {}
    for name in speaker.voices():
        out[name] = int(np.load(os.path.join(brain.path("voices"), name + ".npy")).shape[0])
    return {"voices": out, "threshold": speaker.THRESHOLD}


@app.post("/voices/enrol")
async def voices_enrol(request: Request, name: str, x_pet_token: str | None = Header(None)):
    """Add one WAV clip to a family member's voiceprint (5 or more is best)."""
    check_token(x_pet_token)
    who = brain.resolve_person(name)
    if not who:
        raise HTTPException(status_code=400, detail="not in the family list")
    emb = voiceprint(decode_wav(await request.body()))
    if emb is None:
        raise HTTPException(status_code=400, detail="clip too short (need about 2 seconds of speech)")
    return {"name": who, "clips": speaker.add_clip(who, emb)}


@app.post("/voices/forget")
def voices_forget(name: str | None = None, x_pet_token: str | None = Header(None)):
    """Delete one voiceprint (?name=) or all of them."""
    check_token(x_pet_token)
    return {"forgotten": speaker.forget(brain.resolve_person(name) if name else None)}


@app.get("/audio/{name}")
def audio(name: str):
    if not re.fullmatch(r"[A-Za-z0-9_-]{16}\.mp3", name):
        raise HTTPException(status_code=404)
    path = os.path.join(AUDIO_DIR, name)
    if not os.path.exists(path):
        raise HTTPException(status_code=404)
    return FileResponse(path, media_type="audio/mpeg")
