"""Auto-save: after a conversation ends, Gemma reads it back and keeps what matters.

Nobody has to say "remember". When a conversation has been quiet for 10 minutes
(or a new one starts), the finished transcript goes to Gemma with only the
memory and family_add tools, plus what Deadlee already knows so it doesn't save
things twice. Same caps and rules as saving during a chat. Local only.
"""
import logging
import threading

import brain
import llm
import prompt
import tools

log = logging.getLogger("pet-bridge")

ALLOWED = {"memory", "family_add"}
MAX_TURNS = 30                     # longest transcript we keep for one conversation

PROMPT = """You look after the memory of Deadlee, a friendly helper that talks with a family \
(Sam, who is 6, his Mum and his Dad). Below is a conversation that just finished, and what \
Deadlee already knows.

Save only things worth remembering next week, using the tools:
- A plan, event or date (a trip, a visit, a party, an appointment, a game): family_add, with the \
date written out (for example "Grandma visits on 24 December 2026"). Work the date out from today.
- Something about a family member (a new favourite thing, what they're into, how school or \
sport went, being sick, a big moment): memory with target = their name.
- A joke, game or nickname Deadlee and the family share: memory with target "memory".

Don't save: questions, weather, small talk, things Deadlee already knows, or anything from \
Deadlee's own answers that the family didn't say. Never save passwords, addresses, phone numbers \
or anything unsafe. Most conversations have nothing worth saving: then just reply "nothing"."""


def fold(transcript: list[dict]) -> list[str]:
    """Run the save pass on one finished conversation. Returns the tools that were used."""
    said = [t for t in transcript if t.get("heard")]
    if not said:
        return []
    snap = brain.snapshot()
    lines = []
    for t in said[-MAX_TURNS:]:
        who = (t.get("speaker") or "Someone").title()
        lines.append(f"{who}: {t['heard']}")
        lines.append(f"Deadlee: {t.get('reply', '')}")
    user = (f"{prompt.now_line()}\n\nWhat Deadlee already knows:\n{snap.knowledge}\n\n"
            f"The conversation:\n" + "\n".join(lines))
    ctx = tools.Context(names=[], allowed=ALLOWED)
    try:
        llm.local(PROMPT, [{"role": "user", "content": user}], ctx, max_tokens=300)
    except llm.LocalError as e:
        log.warning("autosave skipped: %s", e)
        brain.log_event("autosave_error", str(e)[:100])
        return []
    if ctx.used:
        log.info("autosave: %s", ", ".join(ctx.used))
        brain.log_event("autosave", ",".join(ctx.used))
    return ctx.used


def fold_later(transcript: list[dict]) -> None:
    """Background: never hold up a reply."""
    if any(t.get("heard") for t in transcript):
        threading.Thread(target=fold, args=(list(transcript),), name="autosave", daemon=True).start()
