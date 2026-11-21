# Security Policy

## Supported Versions

| Version | Supported |
| ------- | --------- |
| 1.0.x   | Yes       |

## Reporting a Vulnerability

Please do not open a public issue for an unfixed vulnerability.

Report it privately through GitHub:

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability** (Security Advisories).

If private reporting is not available on the repository, open an issue that only
states you have a security report and ask the maintainer to open a private
channel. Do not include reproduction details in public.

Please include:

- Affected version and platform.
- Reproduction steps.
- Observed and expected behavior.
- Impact assessment (data exposure, privilege escalation, denial of service).
- Relevant logs with secrets removed.

## What to Expect

- Acknowledgement within 7 days.
- A fix or mitigation plan for confirmed issues.
- Credit in the advisory once a fix ships, unless you prefer to stay anonymous.

## Scope

In scope:

- The C++ backend, HTTP API and WebSocket server.
- The Web UI.
- Build and packaging scripts.
- Credential storage (Hugging Face token handling).

Out of scope:

- Vulnerabilities in third-party components — report those upstream.
- Issues that require pre-existing local administrator or root access.
- Self-XSS or social-engineering scenarios.

## Handling Principles

- ModelForge binds its HTTP server to loopback only; exposing it to a network is
  not a supported configuration.
- Secrets are never logged. If you find a path that leaks a token, treat it as a
  security issue.
