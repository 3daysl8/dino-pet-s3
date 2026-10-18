FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY *.py ui.html countries.json ./
COPY brain-template ./brain-template
# Runs on the host network so it can reach kokoro (8880), llama-swap (8080) and SearXNG (8081) on localhost.
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8090"]
