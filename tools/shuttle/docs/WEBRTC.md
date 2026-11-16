# WebRTC privacy (native, profile-level)

Chromium's own `WebRtcIpHandlingPolicy` only. Nothing is spoofed: `getStats`, SDP, codec capabilities and ICE
candidates are Chromium's real output, nothing is injected (no JS, no CDP, no Playwright), and no IP is faked.

## Behavior

The policy is set in the renderer preferences of each tab (`ApplyAgentRendererPrefs`, `chrome/browser/shuttle/
proxy_auth.cc`; decision in `webrtc_policy.cc`):

| Tab | Policy |
|---|---|
| Agent tab, agent has a proxy (also an unreadable stored one: fails closed) | `disable_non_proxied_udp`, per-URL rules cleared |
| Any tab (agent or not) of a profile whose own `proxy` pref is `fixed_servers` or `pac_script` | `disable_non_proxied_udp`, per-URL rules cleared |
| Anything else | the profile's setting (`webrtc.ip_handling_policy`, `--force-webrtc-ip-handling-policy`, policy `WebRtcIpHandlingPolicy`); never relaxed or overridden |
| Worker of a profile with an explicit proxy | `disable_non_proxied_udp` |

`disable_non_proxied_udp` is the strictest policy Chromium has: ICE uses no route that bypasses the proxy, so no host or
server-reflexive candidate carries the real local or public address. The result is the same for all tabs of the profile
(no per-tab randomness). Changing an agent's proxy, or the profile's `proxy` pref, re-syncs the open tabs (`ProxyTabSync`);
an already-connected peer connection keeps its state until it is re-created.

## Limits

- **No UDP through HTTP/HTTPS proxies.** With the strict policy, WebRTC media and data channels need a UDP-capable
  route through the proxy (SOCKS5 UDP ASSOCIATE). Through an HTTP/HTTPS proxy they fail to connect (no leak, but
  WebRTC calls do not work), unless a TURN relay is reachable over TCP/TLS.
- **TURN.** A TURN server named by a page is only reached through the proxy under the strict policy. TURN over TCP/TLS
  works through an HTTP proxy; TURN over UDP needs a UDP-capable proxy. The relayed address is the TURN server's, not the
  proxy's. In a profile without any proxy there is no strict policy: a TURN or STUN server sees the real address.
- **System and auto-detected proxies are not detected.** Modes `system` and `auto_detect` of the `proxy` pref do not
  trigger the strict policy (the profile cannot know whether a proxy applies). Use the profile's own setting
  (`--force-webrtc-ip-handling-policy=disable_non_proxied_udp` or the policy) for those.
- **Workers.** An agent's own proxy does not reach its shared/service workers (their agent cannot be attributed);
  only a profile-level explicit proxy does. Known gap of the per-agent proxy.
- **Browser-level traffic** (outside tabs) is not covered.
- No MCP setting: it follows the proxy. There is no way to loosen it for a proxied tab.

## Verification

`scripts/smoke-proxy.mjs` step 9 (WebRTC, SKIP when undecidable offline) covers an agent proxy. The profile-proxy case
is not covered by a smoke script yet.
