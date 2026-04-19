#!/bin/bash

# LumenCode Web Deployment Script
# Deploys static files to nuc and updates nginx configuration

NUC_HOST="nuc"
WEB_DIR="./web"
REMOTE_DIR="/var/www/lumencode"
PORT=33335
DOMAIN="lumencode.app"

echo "🚀 Deploying LumenCode website to $NUC_HOST..."

# 1. Sync static files
echo "📦 Syncing files..."
rsync -avz --delete "$WEB_DIR/" "$NUC_HOST:$REMOTE_DIR/"

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
}
EOF

# 3. Enable site and reload Nginx
echo "🔄 Enabling site and reloading Nginx..."
ssh "$NUC_HOST" "sudo ln -sf $NGINX_CONF /etc/nginx/sites-enabled/ && sudo nginx -t && sudo systemctl reload nginx"

echo "✅ Deployment complete!"
echo "🌐 Site available at http://localhost:$PORT (on nuc) or https://$DOMAIN (once Cloudflare is routed)"
