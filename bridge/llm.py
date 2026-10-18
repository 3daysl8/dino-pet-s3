"""The brain: Gemma on the server (llama-swap) with tools, Claude Haiku as a backup.

Privacy rule, enforced here: only local() ever sees the brain files, the
conversation history, tool results or who is speaking. claude_fallback() is
given the persona, the fixed rules, this one message and the pet's needs. Nothing else.
"""
import json
import logging
import os
import re
import time

import anthropic
import httpx

import brain
import prompt
import tools

log = logging.getLogger("pet-bridge")

DATA = os.environ.get("PET_DATA", "/data")
LOCAL_URL = os.environ.get("PET_LOCAL_URL", "http://127.0.0.1:8080/v1/chat/completions")
CHAT_MODEL = os.environ.get("PET_CHAT_MODEL", "gemma-4-26b-a4b")
CLAUDE_MODEL = "claude-haiku-4-5"
LOCAL_TIMEOUT = float(os.environ.get("PET_LOCAL_TIMEOUT", "20"))
MAX_TOOL_ROUNDS = 3
MAX_TOKENS = 220


def _key(name: str) -> str:
    try:
        return open(os.path.join(DATA, name)).read().strip()
    except OSError:
        return ""


_llama_key = _key("llama.key")
_claude_key = _key("anthropic.key")
_claude = anthropic.Anthropic(api_key=_claude_key, timeout=20.0, max_retries=1) if _claude_key else None

stats = {"local": 0, "fallback": 0, "local_errors": 0}


class LocalError(Exception):
    pass


def _post(body: dict) -> dict:
    headers = {"Authorization": f"Bearer {_llama_key}"} if _llama_key else {}
    try:
        r = httpx.post(LOCAL_URL, json=body, headers=headers, timeout=LOCAL_TIMEOUT)
        r.raise_for_status()
        return r.json()["choices"][0]["message"]
    except (httpx.HTTPError, KeyError, ValueError) as e:
        raise LocalError(f"{type(e).__name__}: {e}") from e


# Requests that can't be done without a tool. If the model answers one of these
# without calling anything, ask again with tool_choice=required (small models
# sometimes just *say* "alarm set").
NEEDS_TOOL = re.compile(
    r"\b(timer|alarm|wake me|night ?light|lamp|radio|remember|favou?rite|i like|i love|i'?m into|"
    r"i am into|turn (?:the )?lights? (?:on|off)|weather|rain(?:ing)?|forecast|public holidays?|"
    r"day off|long weekend|spell|read (?:me|us|it|to me|to us|some|more|the next)|keep reading|"
    r"what books|jokes?|(?:make|invent|finish|continue)\b.{0,20}\bstory|capital (?:city )?of|how many people live|what (?:language|money) do they|go back|page \d+|chapter|skip (?:a|the|this|that) page|(?:that|the last) page again|"
    r"from the (?:start|beginning)|start (?:it|the book|the story) (?:again|over))\b", re.I)


CANT = re.compile(r"\bI (?:can'?t|cannot|can not|am not able to|don'?t know how to) "
                  r"(?:do|help|read|check|see|play|set|tell|look|find|open|send|get|turn)", re.I)


def local(system: str, messages: list[dict], ctx: tools.Context, use_tools: bool = True,
          max_tokens: int = MAX_TOKENS, require_tool: bool = False) -> str:
    """Chat with Gemma; runs tool calls (at most MAX_TOOL_ROUNDS) and returns the final text."""
    msgs = [{"role": "system", "content": system}, *messages]
    body = {"model": CHAT_MODEL, "max_tokens": max_tokens,
            "chat_template_kwargs": {"enable_thinking": False}}        # thinking costs seconds, no gain here
    for round_ in range(MAX_TOOL_ROUNDS + 1):
        req = dict(body, messages=msgs)
        if use_tools and round_ < MAX_TOOL_ROUNDS:
            req["tools"] = [t for t in tools.TOOLS
                            if ctx.allowed is None or t["function"]["name"] in ctx.allowed]
            if require_tool and round_ == 0:
                req["tool_choice"] = "required"
        msg = _post(req)
        calls = msg.get("tool_calls") or []
        if not calls:
            return (msg.get("content") or "").strip()
        msgs.append({"role": "assistant", "content": msg.get("content") or "", "tool_calls": calls})
        for c in calls:
            fn = c.get("function", {})
            t0 = time.time()
            result = tools.run(fn.get("name", ""), fn.get("arguments", "{}"), ctx)
            log.info("tool %s %.1fs -> %s", fn.get("name"), time.time() - t0, result[:80])
            msgs.append({"role": "tool", "tool_call_id": c.get("id", ""), "content": result})
    return ""


def local_json(system: str, user: str, schema: dict, max_tokens: int = 400, temperature: float = 0.8) -> dict:
    """One answer from Gemma held to a JSON schema (the story maker's scenes). Raises LocalError
    if the model is down, slow (LOCAL_TIMEOUT) or the answer still isn't JSON. No tools, no history."""
    t0 = time.time()
    msg = _post({"model": CHAT_MODEL, "max_tokens": max_tokens, "temperature": temperature,
                 "chat_template_kwargs": {"enable_thinking": False},
                 "response_format": {"type": "json_schema",
                                     "json_schema": {"name": "answer", "strict": True, "schema": schema}},
                 "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]})
    text = msg.get("content") or ""
    log.info("local_json %.1fs, %d chars in, %d chars out", time.time() - t0, len(system) + len(user), len(text))
    try:
        out = json.loads(text)
    except ValueError as e:
        raise LocalError("answer was not JSON") from e
    if not isinstance(out, dict):
        raise LocalError("answer was not a JSON object")
    return out


def claude_fallback(soul: str, heard: str, state: dict) -> str:
    """Backup brain. Gets no notes, family files, history, tools or speaker name."""
    if not _claude:
        return prompt.LINES["no_brain"]
    system = prompt.build_system(soul, private=False)
    content = f"{heard}\n\n({prompt.state_text(state, private=False)})"
    try:
        r = _claude.messages.create(model=CLAUDE_MODEL, max_tokens=MAX_TOKENS, system=system,
                                    messages=[{"role": "user", "content": content}])
    except anthropic.AuthenticationError:
        log.error("Anthropic key rejected")
        return prompt.LINES["key"]
    except anthropic.RateLimitError:
        return prompt.LINES["busy"]
    except (anthropic.APIConnectionError, anthropic.APITimeoutError):
        return prompt.LINES["offline"]
    except anthropic.APIStatusError as e:
        log.error("Anthropic error %s: %s", e.status_code, e.message)
        return prompt.LINES["no_brain"]
    if r.stop_reason == "refusal":
        return prompt.LINES["refusal"]
    return next((b.text for b in r.content if b.type == "text"), "").strip()


def reply(snap: brain.Snapshot, history: list[dict], heard: str, state: dict,
          ctx: tools.Context) -> tuple[str, bool]:
    """Answer one message. Returns (text, private) where private=False means the backup answered."""
    system = prompt.build_system(snap.soul, private=True, knowledge=snap.knowledge, skills=snap.skills)
    msgs = history + [{"role": "user", "content": f"{heard}\n\n({prompt.state_text(state, ctx.speaker)})"}]
    try:
        text = local(system, msgs, ctx)
        if not ctx.used and NEEDS_TOOL.search(heard):
            brain.log_event("tool_skipped", heard[:60])
            log.info("no tool used for a tool request; asking again with tool_choice=required")
            text = local(system, msgs, ctx, require_tool=True) or text
        stats["local"] += 1
        if text and CANT.search(text) and "cant_help" not in ctx.used:
            brain.log_gap(heard, "said it couldn't", "", ctx.names)     # the model forgot cant_help
        if text:
            return text, True
        brain.log_event("empty_reply")
    except LocalError as e:
        stats["local_errors"] += 1
        log.error("local LLM failed: %s", e)
        brain.log_event("local_error", str(e)[:120])
        if ctx.actions:                         # a tool already ran: don't repeat the request elsewhere
            return prompt.LINES["no_brain"], True
    stats["fallback"] += 1
    brain.log_event("fallback")
    return claude_fallback(snap.soul, heard, state), False


def local_up() -> bool:
    try:
        r = httpx.get(LOCAL_URL.replace("/v1/chat/completions", "/running"), timeout=2.0)
        return any(m.get("model") == CHAT_MODEL and m.get("state") == "ready"
                   for m in r.json().get("running", []))
    except (httpx.HTTPError, ValueError):
        return False
