#!/bin/sh
# (Re)build and start pet-bridge on the server. Secrets, the Whisper model and Deadlee's
# brain folder live in ~/pet-bridge (mounted at /data):
#   pet.token      the toy's token (made here on first run)
#   ui.password    optional: an easy password for the web page (/ui), accepted like the token
#   calendar.url   optional: private ICS link(s) for the family calendar, one per line
#   llama.key      key for llama-swap (a line in ~/llama-secrets/api-keys)
#   anthropic.key  optional: backup brain when the local model is down
#   brain/         SOUL.md, FAMILY.md, MEMORY.md, people/, skills/ (seeded from brain-template)
# Run the tests first: sh run.sh test
set -e
cd "$(dirname "$0")"
[ -f ~/pet-bridge/pet.token ] || (umask 077; python3 -c "import secrets; print(secrets.token_urlsafe(24))" > ~/pet-bridge/pet.token)
docker build -q -t pet-bridge-next .
docker run --rm -v "$PWD/tests":/app/tests pet-bridge-next sh -c "pip install -q pytest 2>/dev/null; python -m pytest -q tests"
[ "$1" = test ] && exit 0
docker tag pet-bridge-next pet-bridge
docker rm -f pet-bridge 2>/dev/null || true
# Run as your user so the brain files stay editable over SSH (older runs were root).
docker run --rm -v ~/pet-bridge:/d pet-bridge chown -R "$(id -u):$(id -g)" /d
docker run -d --name pet-bridge --restart unless-stopped --network host \
  --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v ~/pet-bridge:/data \
  -e PET_VOICE="${PET_VOICE:-bm_daniel}" \
  pet-bridge
