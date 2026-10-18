# dino-pet-s3

A pixel-dino virtual pet for the **Waveshare ESP32-S3-Touch-LCD-1.85C** (round 360×360 touch screen), built with ESP-IDF and LVGL.

On its own it's an offline pet with a clock, alarms, timers, SD-card music and a night light. On the home network it can also talk: a small server ("pet-bridge") runs speech-to-text, a local LLM and text-to-speech, and gives the dino (Deadlee) a memory, the family calendar, weather, stories, books read aloud and a maths game.

> **Status: finished and unmaintained.** I'm sharing this build as is. Fork it, copy it, change it. I won't be answering issues or taking pull requests, and the repo is archived.

## Read this first (safety and no warranty)

- **Hobby project, no warranty.** It comes "AS IS" under the Apache License 2.0 (see [LICENSE](LICENSE)). You use it at your own risk.
- **Not a certified children's toy.** It hasn't been tested to any toy, electrical or radio standard. If you build one, you're responsible for making it safe.
- **Lithium battery.** The board runs from a 3.7 V LiPo cell. Use a protected cell, don't leave it charging unattended, and don't use a cell that's swollen or damaged.
- **AI output can be wrong or inappropriate.** The talking feature uses a language model. An adult should set it up, read its prompts (`bridge/brain-template/`, `bridge/prompt.py`) and supervise children using it.
- **Your family's data is your responsibility.** The server stores conversation notes, family details, calendar data and optional voiceprints. Keep it on your home network only. Nothing in this repo sends data anywhere except the services you configure (your local model, and optionally the Anthropic API as a backup).

## What's here

| Folder | What |
|---|---|
| `firmware/` | ESP-IDF project for the toy: pet, clock, music, night light, settings, WiFi, talk |
| `bridge/` | pet-bridge, a FastAPI server in Docker: speech in and out, chat, memory, tools, web page |
| `bridge/brain-template/` | Persona, family file and skill settings copied into a new brain on first run. **Edit these for your family.** |
| `tools/` | Asset and font generators, plus helper scripts for the server |
| `docs/mockup/` | The HTML mock-up the screens were designed from |

## Hardware

| | |
|---|---|
| Board | [Waveshare ESP32-S3-Touch-LCD-1.85C](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C) |
| SoC | ESP32-S3R8, 16 MB flash, 8 MB PSRAM |
| Display | 1.85" 360×360 round, ST77916 (QSPI) |
| Touch | CST816 |
| RTC | PCF85063 |
| Audio | I2S speaker and mic via `esp_codec_dev` |
| IO expander | TCA9554PWR |
| Storage | microSD (SDMMC) |
| Power | 3.7 V Li-ion via MX1.25, USB-C |

Schematics and dimensions are on the [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C) and in the [Waveshare repo](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C).

## Firmware

- ESP-IDF **v5.5.x** (built with 5.5.5; avoid 6.x)
- LVGL **8.3**, matching the Waveshare drivers
- Components: `lvgl/lvgl ~8.3`, `esp_audio_simple_player`, `esp-sr` (wake word), `esp_codec_dev`

From an ESP-IDF shell:

```sh
cd firmware
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p COMx flash monitor
```

If the board won't flash: hold **BOOT**, tap **RESET**, release BOOT, then retry.

The firmware builds and runs fully offline. The Talk screen only turns on if `firmware/main/app/secrets_talk.h` exists (it's gitignored). Make it with `tools/make_talk_secrets.sh`, or write it by hand:

```c
#pragma once
#define PET_BRIDGE_URL "http://<your-server>:8090"
#define PET_TOKEN "<contents of ~/pet-bridge/pet.token on the server>"
```

WiFi networks are added on the toy itself (Settings).

## pet-bridge (the talking part)

It expects these on the same machine, on localhost:

| Service | Port | Used for |
|---|---|---|
| llama-swap (OpenAI-style API) with a chat model, Gemma by default | 8080 | the brain |
| Kokoro TTS | 8880 | the voice |
| SearXNG | 8081 | look-ups |

Whisper (speech-to-text) and speaker recognition run inside the container.

```sh
mkdir -p ~/pet-bridge
cd bridge
sh run.sh test   # build the image and run the tests
sh run.sh        # start it on port 8090
```

Files in `~/pet-bridge` (see the top of `bridge/run.sh`): `pet.token` (made on first run), optional `ui.password` for the web page, optional `calendar.url`, `llama.key`, and optional `anthropic.key` for a backup model. The brain folder is seeded from `brain-template/` on first run; after that the copies in `~/pet-bridge/brain` are yours to edit. Settings like environment variables are in `bridge/llm.py` and `bridge/app.py` (`PET_*`).

The web page is at `http://<your-server>:8090/ui`.

The PowerShell scripts in `tools/` (`deadlee-*.ps1`) are conveniences for editing the brain and calendar from a Windows PC. They have a placeholder address (`user@192.168.1.50`) near the top: change it to your server.

## Credits and licences

Licensed under the [Apache License 2.0](LICENSE). Built on Waveshare's demo code and Espressif's drivers (both Apache-2.0), the Silkscreen font (OFL 1.1) and country data from mledoze/countries (ODbL 1.0). Details in [NOTICE](NOTICE).

"Tamagotchi" is a trademark of Bandai. This project is not affiliated with or endorsed by Bandai, Waveshare or Espressif.
