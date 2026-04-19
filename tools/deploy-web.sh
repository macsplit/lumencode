#!/bin/bash

# LumenCode Web Deployment Script
# Deploys static files to nuc and updates nginx configuration

NUC_HOST="nuc"
WEB_DIR="./web"
REPO_DIR="./repo"
REMOTE_DIR="/var/www/lumencode"
PORT=33335
DOMAIN="lumencode.app"
PUBLIC_BASE_URL="${PUBLIC_BASE_URL:-http://$NUC_HOST:$PORT}"

generate_flatpak_metadata() {
    cat > "$WEB_DIR/app.lumencode.LumenCode.flatpakref" <<EOF
[Flatpak Ref]
Version=1
Name=app.lumencode.LumenCode
Branch=master
Title=LumenCode
Comment=Structural code explorer for local source trees
Description=Filesystem-aware structural code explorer with parser-assisted symbol browsing and cross-file relation analysis.
Homepage=$PUBLIC_BASE_URL/
Icon=$PUBLIC_BASE_URL/favicon.png
Url=$PUBLIC_BASE_URL/repo/
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
IsRuntime=false
EOF

    cat > "$WEB_DIR/lumencode.flatpakrepo" <<EOF
[Flatpak Repo]
Version=1
Title=LumenCode Flatpak Repository
Comment=Flatpak repository for LumenCode
Description=Self-hosted Flatpak repository for LumenCode updates.
Homepage=$PUBLIC_BASE_URL/
Icon=$PUBLIC_BASE_URL/favicon.png
Url=$PUBLIC_BASE_URL/repo/
EOF
}

echo "🚀 Deploying LumenCode website to $NUC_HOST..."

# 1. Generate Flatpak metadata and sync static files
echo "🧾 Generating Flatpak metadata for $PUBLIC_BASE_URL..."
generate_flatpak_metadata

echo "📦 Syncing files..."
ssh "$NUC_HOST" "mkdir -p $REMOTE_DIR/repo"
rsync -avz --delete "$WEB_DIR/" "$NUC_HOST:$REMOTE_DIR/"
rsync -avz --delete "$REPO_DIR/" "$NUC_HOST:$REMOTE_DIR/repo/"

# 2. Update Nginx configuration
echo "⚙️  Updating Nginx configuration..."
NGINX_CONF="/etc/nginx/sites-available/lumencode.app"

ssh "$NUC_HOST" "sudo tee $NGINX_CONF > /dev/null" <<EOF
server {
    listen $PORT;
    server_name $DOMAIN;

    root $REMOTE_DIR;
    index index.html;

    location / {
        try_files \$uri \$uri/ =404;
    }

    location = /app.lumencode.LumenCode.flatpakref {
        default_type application/vnd.flatpak.ref;
    }

    location = /lumencode.flatpakrepo {
        default_type application/vnd.flatpak.repo;
    }
}
EOF

# 3. Enable site and reload Nginx
echo "🔄 Enabling site and reloading Nginx..."
ssh "$NUC_HOST" "sudo ln -sf $NGINX_CONF /etc/nginx/sites-enabled/ && sudo nginx -t && sudo systemctl reload nginx"

echo "✅ Deployment complete!"
echo "🌐 Site available at http://localhost:$PORT (on nuc) or https://$DOMAIN (once Cloudflare is routed)"
echo "📥 Flatpak install URL: $PUBLIC_BASE_URL/app.lumencode.LumenCode.flatpakref"
