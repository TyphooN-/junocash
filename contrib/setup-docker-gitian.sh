#!/bin/bash
# Script to set up Docker for Gitian builds

set -e

echo "[INFO] Starting Docker daemon..."
systemctl start docker

echo "[INFO] Enabling Docker to start on boot..."
systemctl enable docker

echo "[INFO] Checking Docker status..."
systemctl status docker --no-pager | head -10

echo ""
echo "[INFO] Checking if user '$SUDO_USER' is in docker group..."
if groups "$SUDO_USER" | grep -q '\bdocker\b'; then
    echo "[SUCCESS] User '$SUDO_USER' is already in docker group"
else
    echo "[INFO] Adding user '$SUDO_USER' to docker group..."
    usermod -aG docker "$SUDO_USER"
    echo "[SUCCESS] User '$SUDO_USER' added to docker group"
    echo ""
    echo "┌─────────────────────────────────────────────────────────┐"
    echo "│  IMPORTANT: You need to log out and log back in for    │"
    echo "│  the group changes to take effect.                     │"
    echo "│                                                         │"
    echo "│  Alternative: Run this command to apply immediately:   │"
    echo "│    newgrp docker                                       │"
    echo "└─────────────────────────────────────────────────────────┘"
fi

echo ""
echo "[INFO] Testing Docker with hello-world..."
docker run --rm hello-world

echo ""
echo "[SUCCESS] Docker is ready for Gitian builds!"
