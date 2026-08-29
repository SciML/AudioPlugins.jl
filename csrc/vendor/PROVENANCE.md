# Vendored third-party headers

## clap/ — the CLAP plugin ABI

| | |
|---|---|
| Upstream | https://github.com/free-audio/clap |
| Version | 1.2.10 (`CLAP_VERSION_MAJOR.MINOR.REVISION` in `clap/version.h`) |
| Commit | `a47f6badb49d948fd009998f28309cdab78979c9` |
| Vendored | 2026-08-24, `include/clap` copied verbatim, 68 headers, 392 KB |
| Licence | MIT — see `CLAP-LICENSE` (© 2021 Alexandre BIQUE) |

**Vendored rather than fetched, deliberately.** CLAP is header-only: there is no
library to link, a host `dlopen`s the plugin and talks to it through these
declarations. That makes the whole dependency 392 KB of MIT-licensed text with
no build step, so vendoring buys an offline, deterministic, network-free build
and costs nothing a pinned download would have saved. The headers are also
densely interdependent (`clap/clap.h` pulls in essentially all of them), so
vendoring a useful subset is not an option — it is the tree or a fetch.

Verbatim copy, no local edits. To update: replace the directory from a tagged
upstream checkout, update the table above, and re-run the test suite — the ABI
is versioned and `clap_host.c` checks `clap_version_is_compatible` at load.

## lv2/ — the LV2 plugin specification headers

| | |
|---|---|
| Upstream | https://lv2plug.in (https://gitlab.com/lv2/lv2) |
| Version | 1.18.10 (`lv2-1.18.10.tar.xz`, sha256 `78c51bcf21b54e58bb6329accbb4dae03b2ed79b520f9a01e734bd9de530953f`) |
| Vendored | 2026-08-29, three headers copied verbatim from `include/lv2/`: `core/lv2.h`, `urid/urid.h`, `buf-size/buf-size.h` |
| Licence | ISC — see `LV2-ISC-LICENSE` |

Only the headers the sources here need: `core/lv2.h` for the plugin ABI
(the test plugins and the host), `urid/urid.h` and `buf-size/buf-size.h` for
the host features `lv2_host.c` offers. The full specification — every other
extension header plus the Turtle bundles lilv reads at run time — comes from
`lv2_jll`, and `lv2_host.c` is built against lilv (`Lilv_jll`), which is why
it includes these through the include path (`-I csrc/vendor`) rather than by
relative path: lilv's own headers include `<lv2/core/lv2.h>` too, and both
must resolve to the same copy.
