# Security Policy

## Supported versions

Security fixes are provided for the latest release on the `master` branch.
Before 1.0 the API and wire protocol may change between minor releases; always
link consumers against the matching library version.

## Reporting a vulnerability

Please do not open a public issue for security problems. Report them privately
through GitHub's "Report a vulnerability" advisory flow on
https://github.com/zero-pipe/network_bridge/security/advisories, or by email
to the maintainers listed in the repository profile.

Include:

- the affected version or commit,
- the transport(s) and platform involved,
- a minimal reproduction or proof of concept,
- the impact you believe the issue has.

We aim to acknowledge reports within three business days and to publish a fix
or mitigation as soon as practical. Credit is given in the advisory unless you
prefer to remain anonymous.

## Security model

The built-in UDP and TCP transports are plaintext and unauthenticated. The
envelope only validates magic, version, and CRC integrity; it provides no
confidentiality, replay protection, or peer identity.

Do not expose a node directly to an untrusted network. For deployments that
need confidentiality or authentication:

- carry an encrypted channel through a custom transport provider
  (`<nb/transport_provider.h>`), or
- validate credentials in metadata inside `nb_service_set_interceptor` or the
  handler, and use `peer_rate_limit_pps` and `max_streams` for basic resource
  control.

Cyclone DDS security, when enabled in the DDS configuration, is provided by
Cyclone DDS itself and is outside this project's trust boundary.
