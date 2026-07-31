# `ion-c` overlay port

Provides [`amazon-ion/ion-c`](https://github.com/amazon-ion/ion-c), the C
reference implementation of Amazon Ion, to vcpkg. It backs kythira's
`ion_rpc_serializer` (`include/raft/ion_serializer.hpp`) — an alternative
`Types::serializer_type` to `json_rpc_serializer`, using Amazon Ion instead of
JSON as the RPC wire format.

No official vcpkg registry port for `ion-c` exists, so — like
`vcpkg-overlays/stdexec` and `vcpkg-overlays/lakers` — this project ships its
own overlay port. `ion-c` is a plain CMake C project, so the port follows the
CMake-port shape (`vcpkg_from_github` + `vcpkg_cmake_configure` /
`vcpkg_cmake_install` / `vcpkg_cmake_config_fixup`), not the cargo-build shape
`lakers` needs.

## Opt-in only

The port is gated behind the opt-in `"ion"` feature in the root `vcpkg.json`
(mirroring the `"edhoc"` feature that gates `lakers`). A default install does
**not** fetch or build `ion-c`; omitting the feature leaves
`KYTHIRA_ION_SERIALIZER_AVAILABLE` undefined and `ion_rpc_serializer`
unavailable, and the rest of the project configures and builds unaffected
(graceful degradation, mirroring `LIBCOAP_FOUND`).

To build with Ion support:

```sh
vcpkg install ion-c --overlay-ports=vcpkg-overlays
# or add the "ion" feature to your manifest install
cmake -B build -DVCPKG_MANIFEST_FEATURES=ion ...
```

The `ION_SERIALIZER` Kconfig symbol (root `Kconfig`) plus
`KYTHIRA_KCONFIG_STRICT=ON` turns a missing `ion-c` into a hard configure
failure, exactly as `COAP_TRANSPORT` does for `libcoap`.

## Pin / SHA512 regeneration

`portfile.cmake` pins `REF v1.1.3`. The `SHA512` is intentionally set to `0`
and **must be regenerated when the pin is first built or bumped** — the
authoring environment had no outbound access to GitHub archive downloads to
compute it. Regenerate by running the `vcpkg install` command above once: vcpkg
fails the download with the actual hash, which you copy into the `SHA512` field.
This is a one-time step per pin and only affects opt-in Ion builds; it never
affects the default (Ion-less) build.
