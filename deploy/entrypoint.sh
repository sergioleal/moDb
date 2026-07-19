#!/bin/sh
# Entrypoint serverless (Fase 13C): config só por env; dados só em /data.
set -eu

DB_PATH="${MODB_DB_PATH:-/data/db.modb}"
HOST="${MODB_HOST:-0.0.0.0}"
PORT="${MODB_PORT:-7400}"

mkdir -p "$(dirname "$DB_PATH")"

if [ ! -f "$DB_PATH" ]; then
  echo "modb-entrypoint: creating database at $DB_PATH"
  modb db create "$DB_PATH"
fi

echo "modb-entrypoint: serving $DB_PATH on ${HOST}:${PORT}"
exec modb serve --from-env
