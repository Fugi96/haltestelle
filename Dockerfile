FROM python:3.13-slim

WORKDIR /app

COPY backend/requirements.txt backend/
RUN pip install --no-cache-dir -r backend/requirements.txt

# A new volume takes the owner of /data, so the app can write to it.
RUN useradd --system --no-create-home app && mkdir /data && chown app:app /data

COPY backend/app backend/app
COPY frontend frontend

ENV STATIONS_DB_PATH=/data/stations.sqlite
ENV CONFIG_PATH=/config/config.toml

USER app
WORKDIR /app/backend
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
