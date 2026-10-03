#!/bin/sh
# Generates the backend's password on first start, writes the password file
# and ACL, then starts the broker.
set -eu

SECRET=/secrets/mqtt_password
if [ ! -s "$SECRET" ]; then
    tr -dc 'A-Za-z0-9' < /dev/urandom | head -c 40 > "$SECRET.tmp"
    chmod 644 "$SECRET.tmp"
    mv "$SECRET.tmp" "$SECRET"
fi

CONF=/mosquitto/config
rm -f "$CONF/passwd"
mosquitto_passwd -c -b "$CONF/passwd" app "$(cat "$SECRET")"

cat > "$CONF/acl" <<EOF
topic read #

user app
topic write #
EOF

chown mosquitto:mosquitto "$CONF/passwd" "$CONF/acl"
chmod 600 "$CONF/passwd" "$CONF/acl"

exec mosquitto -c "$CONF/mosquitto.conf"
