// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

// Starts the SDK socket HTTP server. Ollama routes: /api/tags, /api/generate,
// /api/chat. OpenAI-compatible routes: /v1/models, /v1/chat/completions (SSE).
// Optional /app0/api_key.txt enables bearer authentication on all routes.
// Connections are bounded and completed worker threads are joined, not detached.
// Safe to call once during startup; failure leaves the console app running.
bool prospero_http_server_start(unsigned short port);

// Copies the most recent status/diagnostic line the server thread recorded
// (socket bind results, last request path, etc.) into the caller's buffer.
// Thread-safe; safe to call from the UI thread on each tick.
void prospero_http_server_last_status(char *text, std::size_t capacity);

// Copies the console's current LAN URL for the hosted web interface.
bool prospero_http_server_mobile_url(char *text, std::size_t capacity);

// Returns 1 when a bearer key is required, 0 when authentication is disabled,
// and -1 when api_key.txt is invalid and requests cannot authenticate.
int prospero_http_server_auth_status();

// Ensure shared model storage is available, automatically starting the bundled
// title-scoped helper through the console-local ELF loader when needed.
