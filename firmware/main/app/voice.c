#include "voice.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "settings.h"

typedef struct { uint8_t ev; int8_t id; } msg_t;
static QueueHandle_t s_q;

bool voice_usb_connected(void)
{
    // True when a computer is on the USB port. A plain phone charger does not
    // count, because the board has no separate VBUS sense we can read.
    return usb_serial_jtag_is_connected();
}

bool voice_enabled(void)
{
    switch (g_settings.voice) {
    case VOICE_ALWAYS:   return true;
    case VOICE_USB_ONLY: return voice_usb_connected();
    default:             return false;
    }
}

void voice_post(voice_ev_t ev, int command_id)
{
    if (!s_q) s_q = xQueueCreate(4, sizeof(msg_t));
    msg_t m = {(uint8_t)ev, (int8_t)command_id};
    xQueueSend(s_q, &m, 0);
}

voice_ev_t voice_poll(int *command_id)
{
    msg_t m;
    if (!s_q || xQueueReceive(s_q, &m, 0) != pdTRUE) return VOICE_EV_NONE;
    if (command_id) *command_id = m.id;
    return (voice_ev_t)m.ev;
}
