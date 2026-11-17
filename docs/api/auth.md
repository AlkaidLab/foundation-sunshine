# Web UI authentication API

Status: implementation contract for the Web UI and the desktop panel.
Date: 2026-11-03.

The Configuration UI no longer asks the browser for HTTP Basic credentials.
Remote clients sign in with a JSON request and receive an opaque, server-side
session in a `__Host-` cookie. Scripts may keep using `Authorization: Basic`.

## Trust model

Loopback requests remain the existing trusted local surface. This keeps the tray,
the desktop panel proxy and local automation working without a session. A request
from any other address needs a valid session cookie or valid Basic credentials.

The server never sends `WWW-Authenticate`. An unauthenticated API request gets a
JSON `401`; an unauthenticated page navigation gets a `303` to `/login` with a
same-origin `next` parameter.

## Cookies

| Name | HttpOnly | Purpose |
| --- | --- | --- |
| `__Host-sunshine_session` | yes | Opaque 256-bit session identifier (64 hex characters). The server stores only its SHA-256 verifier. |
| `__Host-sunshine_csrf` | no | Synchronizer token bound to the session. The page reads it and sends the same value in `X-CSRF-Token`. |

Both cookies use `Secure; SameSite=Strict; Path=/` and no `Domain` attribute.
The `__Host-` prefix requires that exact combination, so a sibling host cannot
set or overwrite either cookie.

The session identifier is rotated at every successful login: if the request
already carries a session cookie, that session is revoked before the new one is
created. Logout revokes the current session. A password change revokes every
other session and moves the initiating session to a fresh identifier, so a
token captured before the change is worthless.

## Lifetimes

The defaults follow OWASP ASVS 4.0.3 V3.3.2:

| Session | Idle timeout | Absolute timeout |
| --- | --- | --- |
| ordinary | 30 minutes | 12 hours |
| remember me | 7 days | 30 days |

Every authenticated request refreshes the idle deadline. The absolute deadline
never moves. The values are config options in `sunshine.conf`:

```ini
session_idle_minutes = 30
session_absolute_hours = 12
session_remember_idle_minutes = 10080
session_remember_days = 30
```

Sessions are kept in memory. A restart of the host invalidates every session and
the user signs in again. This is deliberate: no usable session verifier is left
in the configuration directory, and it avoids depending on file ACLs on every
host platform.

## CSRF

`GET`, `HEAD` and `OPTIONS` need no token. For `POST`, `PUT`, `PATCH` and
`DELETE`, a request authenticated by the session cookie must send
`X-CSRF-Token` equal to the `__Host-sunshine_csrf` cookie value. The server also
compares the token with the verifier stored on the session, so this is a
session-bound double submit rather than the naive cookie-only pattern.

The cookie names carry the service port unless it is the default (47989):
`__Host-sunshine_csrf` and `__Host-sunshine_session` on the default port,
`__Host-sunshine_csrf_<port>` and `__Host-sunshine_session_<port>` on any other,
so two hosts on one name do not replace each other. The Web UI is served on the
service port plus one. `GET /api/auth/session` names both (`csrf_cookie`,
`session_cookie`). A page that reads a different name sends no header and the host
answers 403 for every state change (the log line says so).

Basic-authenticated and loopback requests do not use a cookie, so they do not
need the header. The existing Origin/Referer/Sec-Fetch-Site check still applies
to every `/api/` request.

## Endpoints

All request and response bodies are JSON. Send
`Content-Type: application/json` for every POST, PUT, PATCH and DELETE.
Responses use `Cache-Control: no-store`.

### POST /api/auth/login

```json
{
  "username": "alice",
  "password": "correct horse battery staple",
  "remember": false
}
```

`remember` is optional and defaults to `false`. A successful response is `200`:

```json
{
  "status": true,
  "authenticated": true,
  "username": "alice",
  "session_id": "0123456789abcdef0123456789abcdef",
  "created_at": "2026-11-03T10:00:00Z",
  "expires_at": "2026-11-03T10:30:00Z",
  "absolute_expires_at": "2026-11-03T22:00:00Z",
  "remember": false,
  "csrf_token": "64 hex characters"
}
```

The response also sets both cookies. A wrong username and a wrong password
return the same body and status:

```json
{ "ok": false, "error": "Invalid username or password" }
```

HTTP status `401`. After five failures for an address or a username, the server
returns `429 Too Many Requests` with `Retry-After` in seconds. The delay starts
at 30 seconds, doubles per failure and is capped at 15 minutes; one account
cannot exceed 100 failed attempts in a rolling hour.

### POST /api/auth/setup

Available only while no credentials exist. This is the create-account step of
the first-run flow.

```json
{ "username": "alice", "password": "at least 12 characters", "remember": true }
```

The username is 1-128 characters and the password is 12-128 characters. A
successful setup returns the same shape as login and sets the cookies. If
credentials already exist the response is `409 Conflict` with
`{ "ok": false, "error": "Setup already completed" }`.

### POST /api/auth/logout

Revokes the current session and clears both cookies. Requires the CSRF header
when the session cookie authenticated the request. Response `200`:

```json
{ "status": true }
```

### GET /api/auth/session

Public and idempotent; it never returns a challenge. It lets the login page
decide between sign-in and create-account.

Authenticated with a session:

```json
{
  "authenticated": true,
  "username": "alice",
  "session_id": "0123456789abcdef0123456789abcdef",
  "created_at": "2026-11-03T10:00:00Z",
  "last_seen": "2026-11-03T10:05:00Z",
  "expires_at": "2026-11-03T10:35:00Z",
  "absolute_expires_at": "2026-11-03T22:00:00Z",
  "remember": false,
  "csrf_token": "64 hex characters",
  "local": false,
  "setup_required": false
}
```

Trusted loopback without a session:

```json
{ "authenticated": true, "username": "alice", "local": true, "setup_required": false }
```

A request that presents a session cookie gets that session's verdict: a
revoked, expired or rotated identifier reports `"authenticated": false` even
from loopback. The trusted-local answer above applies only when no session
cookie is sent at all.

Otherwise:

```json
{ "authenticated": false, "setup_required": false }
```

`setup_required` is `true` when no credentials have been created yet.

### GET /api/auth/sessions

Lists the sessions of the signed-in user. `client_ip` is masked (IPv4 keeps the
first three octets, IPv6 keeps the first four groups) and `user_agent` is
shortened to at most 99 characters.

```json
{
  "status": true,
  "current": "0123456789abcdef0123456789abcdef",
  "sessions": [
    {
      "id": "0123456789abcdef0123456789abcdef",
      "created_at": "2026-11-03T10:00:00Z",
      "last_seen": "2026-11-03T10:05:00Z",
      "expires_at": "2026-11-03T10:35:00Z",
      "client_ip": "192.0.2.x",
      "user_agent": "Mozilla/5.0 ...",
      "current": true,
      "remember": false
    }
  ]
}
```

`id` is an opaque public handle. It is not the session token and cannot be used
as one.

### DELETE /api/auth/sessions/{id}

Revokes one session by its public `id`. The current session may revoke itself;
the response then clears both cookies. Response:

```json
{ "status": true, "revoked": 1 }
```

### DELETE /api/auth/sessions?all=1

Signs out everywhere, including the current session, and clears the cookies.

```json
{ "status": true, "revoked": 3 }
```

### POST /api/auth/password

```json
{
  "current_password": "correct horse battery staple",
  "new_password": "a new long passphrase",
  "confirm_password": "a new long passphrase"
}
```

`confirm_password` is optional; when present it must match. The current
password is verified in constant time, and the route sits behind the same
failure backoff as login: after five wrong current passwords for an address or
a username it answers `429 Too Many Requests` with `Retry-After`.

A successful change revokes every other session of the user and keeps the
initiating session on a fresh identifier bound to the new credential. The
response sets both cookies again and returns the new session state:

```json
{
  "status": true,
  "session_id": "0123456789abcdef0123456789abcdef",
  "csrf_token": "64 hex characters",
  "expires_at": "2026-11-03T11:00:00Z",
  "absolute_expires_at": "2026-11-03T23:00:00Z"
}
```

The previous session identifier, including its CSRF token, stops working at
that moment. A client authenticated by Basic credentials or loopback trust gets
no cookies; for those clients the change revokes every session.

### POST /api/auth/password/reset

```json
{ "new_password": "a new long passphrase", "confirm_password": "a new long passphrase" }
```

Sets a new password without the current one, for the trusted local surface only:
the request must come from loopback (`403` otherwise), must not carry
`Forwarded`, `X-Forwarded-For` or `X-Real-IP` (`403`, so a reverse proxy on the
same computer cannot pass a remote request off as local), and passes the same
Fetch Metadata and CSRF checks as every state change. It exists for the desktop
panel (主机设置 › 安全与登录), because a person who forgot the password cannot
give the old one. The length rules are those of `/api/auth/password`; `409` when
no account exists yet. A reset revokes every session and clears the login
backoff. Over the network the old password stays required.

## Browser navigation

When a page navigation has no session, the server answers `303 See Other` with:

```
Location: /login?next=%2Fconfig%3Ftab%3Dnetwork
```

The login page must accept `next` only when it starts with a single `/` and does
not start with `//` or contain a backslash. The server applies the same rule
when it builds the redirect. Never put a session token in a URL.

`GET /login` serves the built `login.html` when present and otherwise a minimal
server-side sign-in page. The built page should read `GET /api/auth/session`,
show the create-account fields when `setup_required` is true, POST to
`/api/auth/setup`, and otherwise POST to `/api/auth/login`.

`GET /login` with a valid session cookie never shows the sign-in form: the
server answers `303 See Other` with the validated `next` target, or `/` when
`next` is absent, unsafe or names `/login` itself. An authenticated bookmark of
the sign-in page therefore lands on the UI, and the redirect cannot loop.

## Legacy endpoints and the desktop panel

`POST /api/password` and `GET|POST /api/logout` remain for the current first-run
page and older UI code. Their behaviour is unchanged except that logout now
always answers `200` and never sends a Basic challenge, and a successful
`/api/password` change revokes all sessions.

The desktop panel reaches the host through its loopback proxy, which is the
trusted local surface, so it keeps working without changes. A remote API client
can keep sending `Authorization: Basic`. If the panel is ever exposed beyond
loopback, it should move to this session model: read `setup_required`, create
the account once, then call `/api/auth/login` and keep the two cookies. The
`X-CSRF-Token` header is required on its state-changing calls.

## Error shape

Errors from these routes use either the existing helper shape:

```json
{ "ok": false, "error": "message" }
```

or the login-specific shape:

```json
{ "status": false, "error": "message", "code": "unauthenticated" }
```

| Status | Meaning |
| --- | --- |
| 400 | Malformed JSON, missing fields, invalid length |
| 401 | Bad credentials or no authenticated session |
| 403 | Cross-site request or missing/invalid CSRF token |
| 409 | Setup attempted after credentials exist |
| 413 | JSON body larger than 8 KiB |
| 429 | Login or password-change backoff active; honour `Retry-After` |

## Security decisions and remaining assumptions

- The existing credential store is SHA-256 with a 16-character salt. The new
  routes keep it and compare the digest with `CRYPTO_memcmp`. The digest is
  computed for unknown users too, so the two failure paths do the same work.
  Moving the store to Argon2id or PBKDF2 is a separate migration; it would
  change the on-disk credential format and is not part of this card.
- Session identifiers and CSRF tokens come from `RAND_bytes` through
  `crypto::rand(32)`. Only SHA-256 verifiers are retained.
- The store is in memory and bounded to 128 sessions; the oldest session is
  evicted when the bound is reached.
- Loopback remains fully trusted. A browser on the host therefore does not see
  the login page unless it reaches the host through a non-loopback address.
- The login page and the shared Security & Login page are built by the UI
  engineer. This card supplies the API, the cookies and a fallback page only.
- The desktop panel path was inspected, not run against this host. It keeps
  working through loopback trust; whether its WebView accepts a `Secure`
  `__Host-` cookie over the loopback HTTP proxy is unverified here.
- The Windows build and the Windows cookie behaviour are unverified on this
  run; the VM was off for the whole check.
- The session list shows only the three fields the panel needs; no raw IP or
  full user agent is persisted or returned.

## Sources

Recorded in `docs/authority/source-index.tsv`:

- `ds5-owasp-asvs4-v2-20261103` - ASVS 4.0.3 V2 Authentication.
- `ds5-owasp-asvs4-v3-20261103` - ASVS 4.0.3 V3 Session Management.
- `ds5-owasp-session-cheatsheet-20261103` - OWASP Session Management Cheat Sheet.
- `ds5-owasp-csrf-cheatsheet-20261103` - OWASP CSRF Prevention Cheat Sheet.
- `ds5-rfc6265-http-state-20261103` - RFC 6265 HTTP State Management.
- `ds5-rfc9110-http-20261031` - RFC 9110 HTTP Semantics (401, 403, 303).
- `ds5-openssl-crypto-memcmp-20261103` - OpenSSL `CRYPTO_memcmp`.
