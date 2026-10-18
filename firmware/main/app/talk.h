// Talk: chat with the dino's brain (pet-bridge on the server, home WiFi only).
// The network work runs on its own task; the UI polls talk_status().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { TALK_IDLE = 0, TALK_WAIT_WIFI, TALK_THINKING, TALK_DONE, TALK_ERROR } talk_status_t;

bool talk_configured(void);                     // built with the bridge address + token?
bool talk_send_text(const char *text, const char *state_json);   // false if already busy
// Deadlee says good morning / goodnight ("morning" or "bedtime"): turns WiFi on for it,
// plays the line and opens Talk to show it. False if busy or Talk isn't set up.
bool talk_routine(const char *kind, const char *state_json);
// Hold-to-talk: start recording; on release, send it (false if too short or busy).
bool talk_record_start(void);
bool talk_record_send(const char *state_json);
float talk_record_seconds(void);
// Wipe the dino's long-term memory on the server (runs in the background).
bool talk_forget(void);
int talk_forget_result(void);         // 0 = nothing new, 1 = wiped, -1 = failed (read once)

// "Hey <name>" while plugged in: the voice detector sends each phrase to the server,
// which answers only if it was meant for the dino. Call talk_poll() once a second
// (UI thread) with the current pet state; true from talk_wants_view() = open Talk.
void talk_poll(const char *state_json);
bool talk_wants_view(void);
bool talk_listening(void);

// Tap to talk: listen until the speaker stops, send, then keep listening for a
// reply without another tap. Ends after a quiet spell or talk_session_end().
typedef enum { LISTEN_OFF = 0, LISTEN_READY, LISTEN_HEARING } talk_listen_t;
bool talk_session_start(void);
void talk_session_end(void);
bool talk_session_active(void);
talk_listen_t talk_listen_state(void);    // for the screen: waiting for you / hearing you
talk_status_t talk_status(void);

// Things the bridge asked the toy to do with a reply ("actions" in the /talk JSON).
// Run on the UI thread by actions.c.
typedef enum { TALK_ACT_TIMER = 1, TALK_ACT_ALARM, TALK_ACT_NIGHTLIGHT, TALK_ACT_RADIO } talk_act_type_t;
typedef struct {
    talk_act_type_t type;
    int32_t a, b, c, d;   // timer: a=seconds | alarm: a=h b=m c=days | nightlight: a=on b=colour c=level d=timer (-1 = keep) | radio: a=play
    char text[32];        // timer label / radio station
} talk_action_t;
bool talk_take_action(talk_action_t *out);     // oldest queued action, if any
// After TALK_DONE / TALK_ERROR: copy out what was heard and the reply (or the
// error text), and go back to TALK_IDLE.
void talk_take(char *heard, size_t heard_n, char *reply, size_t reply_n);
