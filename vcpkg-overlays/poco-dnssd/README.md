# poco-dnssd overlay port

Builds [Poco DNSSD](https://github.com/pocoproject/poco-dnssd) and its Avahi
backend as two static libraries, `PocoDNSSD` and `PocoDNSSDAvahi`, against
vcpkg's `poco[net]`. kythira's `poco_peer_discovery` and `poco_discovery_node`
use them (`CONFIG_POCO_DISCOVERY`, `KYTHIRA_HAS_POCO_DNSSD`).

Enable it with the manifest feature:

```sh
sudo apt-get install libavahi-client-dev   # vcpkg has no avahi port
vcpkg install --x-feature=poco-dnssd
```

Consumers use `find_package(poco-dnssd CONFIG)` and link
`poco-dnssd::PocoDNSSDAvahi`, which brings in `PocoDNSSD`, Poco and the
Avahi client library. The package reports not-found when pkg-config cannot
find `avahi-client`.

Running anything that constructs a `poco_peer_discovery` needs a running
`avahi-daemon` (and the system D-Bus it talks to); without one, Avahi client
creation fails and Poco throws `DNSSDException`.

Upstream has no CMake build, so `CMakeLists.txt` and
`poco-dnssd-config.cmake.in` here are vendored and copied into the source
tree by `portfile.cmake`. Only the Avahi backend is built; Bonjour would need
Apple's mDNSResponder SDK. Upstream is untagged and has not changed since
2016, so the port pins its only head commit.
