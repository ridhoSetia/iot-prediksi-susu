FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

# Install dependencies
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy source code and assets
COPY . .

# Expose FastAPI port
EXPOSE 7000

# Start server with hot-reload enabled
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "7000", "--reload", "--reload-dir", "/app"]
