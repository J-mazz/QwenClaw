#!/usr/bin/env bash
# okx-oauth.sh — OKX OAuth 2.0 Authorization Code + PKCE flow
# Supports demo and live profiles; stores refresh token in ~/.okx/tokens.json

set -euo pipefail

# Configuration
OAUTH_SERVER="https://app.okx.com"
RESOURCE="https://us.okx.com/api/v1/mcp"
REDIRECT_PORT=7989
REDIRECT_URI="http://127.0.0.1:${REDIRECT_PORT}/callback"
STATE=$(uuidgen 2>/dev/null || echo "qc-$(date +%s)")
TOKENS_FILE="${HOME}/.okx/tokens.json"

# PKCE
VERIFIER=$(openssl rand -base64 32 | tr '+/' '-_' | tr -d '=')
CHALLENGE=$(echo -n "$VERIFIER" | openssl dgst -sha256 -binary | base64 | tr '+/' '-_' | tr -d '=')

usage() {
  cat >&2 <<'EOF'
Usage: okx-oauth.sh [COMMAND] [OPTIONS]

Commands:
  auth                 Start OAuth flow (opens browser, waits for callback)
  refresh              Refresh access token using stored refresh token
  export               Print export OKX_ACCESS_TOKEN=... (uses stored token or auto-refresh)
  clear                Delete stored tokens

Options (for auth):
  --scope SCOPE        Scope(s): demo:read, demo:trade, live:read, live:trade, live:asset_transfer, demo:earn, live:earn
                       Default: demo:read demo:trade
  --no-browser         Don't auto-open browser (print URL instead)

Example:
  okx-oauth.sh auth --scope "demo:trade demo:read"
  eval "$(okx-oauth.sh export)"
EOF
  exit 1
}

log_info() {
  echo "[$(date +'%H:%M:%S')] $*" >&2
}

log_error() {
  echo "[$(date +'%H:%M:%S')] ERROR: $*" >&2
}

# Store tokens to disk
store_tokens() {
  local access_token=$1
  local refresh_token=$2
  local expires_in=${3:-3600}
  local expires_at=$(($(date +%s) + expires_in))

  mkdir -p "$(dirname "$TOKENS_FILE")"
  cat > "$TOKENS_FILE" <<EOF
{
  "access_token": "$access_token",
  "refresh_token": "$refresh_token",
  "expires_at": $expires_at,
  "updated_at": $(date +%s)
}
EOF
  chmod 600 "$TOKENS_FILE"
  log_info "Tokens stored to $TOKENS_FILE"
}

# Load tokens from disk
load_tokens() {
  if [[ ! -f "$TOKENS_FILE" ]]; then
    return 1
  fi
  cat "$TOKENS_FILE"
}

# Check if token is expired
token_expired() {
  local tokens_json=$1
  local expires_at=$(echo "$tokens_json" | jq -r '.expires_at // 0')
  local now=$(date +%s)
  [[ $now -ge $((expires_at - 300)) ]]  # Refresh if <5m left
}

# Dynamic client registration
register_client() {
  log_info "Registering OAuth client..."

  local response=$(curl -sS -X POST \
    "${OAUTH_SERVER}/api/v5/mcp/auth/register" \
    -H "Content-Type: application/json" \
    -d "{
      \"client_name\": \"QuantClaw-$(hostname)\",
      \"redirect_uris\": [\"$REDIRECT_URI\"],
      \"response_types\": [\"code\"],
      \"grant_types\": [\"authorization_code\", \"refresh_token\"],
      \"token_endpoint_auth_method\": \"none\"
    }")

  local client_id=$(echo "$response" | jq -r '.client_id // empty')
  if [[ -z "$client_id" ]]; then
    log_error "Registration failed: $response"
    return 1
  fi

  echo "$client_id"
  log_info "Registered client: $client_id"
}

# Authorization URL
build_auth_url() {
  local client_id=$1
  local scope=${2:-"demo:read demo:trade"}

  echo "${OAUTH_SERVER}/account/oauth?"\
"response_type=code&"\
"client_id=${client_id}&"\
"redirect_uri=$(python3 -c "import urllib.parse; print(urllib.parse.quote('''$REDIRECT_URI'''))")&"\
"scope=$(python3 -c "import urllib.parse; print(urllib.parse.quote('''$scope'''))")&"\
"code_challenge=${CHALLENGE}&"\
"code_challenge_method=S256&"\
"state=${STATE}"
}

# Exchange code for token
exchange_code() {
  local client_id=$1
  local code=$2

  log_info "Exchanging authorization code for token..."

  local response=$(curl -sS -X POST \
    "${OAUTH_SERVER}/api/v5/mcp/auth/token" \
    -H "Content-Type: application/json" \
    -d "{
      \"grant_type\": \"authorization_code\",
      \"client_id\": \"$client_id\",
      \"code\": \"$code\",
      \"redirect_uri\": \"$REDIRECT_URI\",
      \"code_verifier\": \"$VERIFIER\"
    }")

  local access_token=$(echo "$response" | jq -r '.access_token // empty')
  local refresh_token=$(echo "$response" | jq -r '.refresh_token // empty')
  local expires_in=$(echo "$response" | jq -r '.expires_in // 3600')

  if [[ -z "$access_token" ]]; then
    log_error "Token exchange failed: $response"
    return 1
  fi

  store_tokens "$access_token" "$refresh_token" "$expires_in"
  echo "$access_token"
}

# Refresh token
refresh_access_token() {
  local tokens_json=$1
  local refresh_token=$(echo "$tokens_json" | jq -r '.refresh_token // empty')

  if [[ -z "$refresh_token" ]]; then
    log_error "No refresh token available"
    return 1
  fi

  log_info "Refreshing access token..."

  local response=$(curl -sS -X POST \
    "${OAUTH_SERVER}/api/v5/mcp/auth/token" \
    -H "Content-Type: application/json" \
    -d "{
      \"grant_type\": \"refresh_token\",
      \"refresh_token\": \"$refresh_token\"
    }")

  local access_token=$(echo "$response" | jq -r '.access_token // empty')
  local expires_in=$(echo "$response" | jq -r '.expires_in // 3600')

  if [[ -z "$access_token" ]]; then
    log_error "Token refresh failed: $response"
    return 1
  fi

  store_tokens "$access_token" "$refresh_token" "$expires_in"
  echo "$access_token"
}

# Start OAuth flow
cmd_auth() {
  local scope="demo:read demo:trade"
  local open_browser=true

  while [[ $# -gt 0 ]]; do
    case $1 in
      --scope) scope="$2"; shift 2 ;;
      --no-browser) open_browser=false; shift ;;
      *) log_error "Unknown option: $1"; usage ;;
    esac
  done

  # Register client
  local client_id=$(register_client) || exit 1

  # Build auth URL
  local auth_url=$(build_auth_url "$client_id" "$scope")

  log_info "Opening authorization URL..."
  if $open_browser; then
    if command -v xdg-open &>/dev/null; then
      xdg-open "$auth_url" 2>/dev/null || true
    elif command -v open &>/dev/null; then
      open "$auth_url" 2>/dev/null || true
    else
      log_info "Cannot auto-open browser. Visit this URL:"
    fi
  fi

  echo "$auth_url"

  # Start callback listener
  log_info "Waiting for callback on $REDIRECT_URI..."

  local callback_response
  {
    callback_response=$(nc -l 127.0.0.1 "$REDIRECT_PORT" -w 1 2>/dev/null | head -1) || true
  } &
  local nc_pid=$!

  # Extract code from callback (redirect?code=...)
  local code=$(echo "$callback_response" | grep -oP 'code=\K[^&\s]+' | head -1) || true

  if [[ -z "$code" ]]; then
    # Fallback: prompt user
    log_info "Enter the authorization code from the redirect URL:"
    read -r code
  fi

  # Kill nc
  wait $nc_pid 2>/dev/null || true

  # Exchange code
  local access_token=$(exchange_code "$client_id" "$code") || exit 1

  log_info "OAuth flow complete. Access token stored."
  echo "export OKX_ACCESS_TOKEN=\"$access_token\""
}

# Refresh stored token
cmd_refresh() {
  local tokens_json=$(load_tokens) || {
    log_error "No stored tokens found. Run: okx-oauth.sh auth"
    exit 1
  }

  local access_token=$(refresh_access_token "$tokens_json") || exit 1
  echo "export OKX_ACCESS_TOKEN=\"$access_token\""
}

# Export current token (with auto-refresh if expired)
cmd_export() {
  local tokens_json=$(load_tokens) || {
    log_error "No stored tokens found. Run: okx-oauth.sh auth"
    exit 1
  }

  if token_expired "$tokens_json"; then
    tokens_json=$(load_tokens)  # Reload after refresh
    local access_token=$(echo "$tokens_json" | jq -r '.access_token')
  else
    local access_token=$(echo "$tokens_json" | jq -r '.access_token')
  fi

  echo "export OKX_ACCESS_TOKEN=\"$access_token\""
}

# Clear stored tokens
cmd_clear() {
  rm -f "$TOKENS_FILE"
  log_info "Tokens cleared."
}

# Main
if [[ $# -eq 0 ]]; then
  usage
fi

case "${1:-}" in
  auth) shift; cmd_auth "$@" ;;
  refresh) cmd_refresh ;;
  export) cmd_export ;;
  clear) cmd_clear ;;
  --help|-h) usage ;;
  *) log_error "Unknown command: $1"; usage ;;
esac
