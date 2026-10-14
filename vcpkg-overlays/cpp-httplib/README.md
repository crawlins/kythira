# `cpp-httplib` overlay port

The registry's `cpp-httplib` 0.58.0 port (vcpkg commit `e182cb4`, the
baseline `vcpkg-configuration.json` pins for this package), copied unchanged
apart from one extra patch and `port-version` 1.

## Why

The port turns on `CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO`, so every
cpp-httplib client lookup on glibc goes through `getaddrinfo_a()` and
`gai_suspend()` in `detail::getaddrinfo_with_timeout()`. glibc's
`gai_suspend()` can return 0 while the request is still in progress (its
futex wait ends on any wake-up). cpp-httplib took that return as completion
and left the function, abandoning its stack-local `struct gaicb` to glibc's
resolver thread, which then read the request and wrote its result into
whatever frame had replaced it. That thread blocks every signal, so a fault
there kills the process with no handler run: `ca_cluster_node_test` died
that way on clang++-18 arm64 CI legs.

`0001-wait-for-in-progress-getaddrinfo-a.patch` keeps calling
`gai_suspend()` for the rest of the timeout while `gai_error()` reports
`EAI_INPROGRESS`, and falls through to the existing cancel path (which
already waits for the resolver thread) when time runs out. Upstream master
(0.61.0) still has the early return.

## Removing the overlay

Drop this directory and its `overlay-ports` entry once a registry version
of cpp-httplib waits for in-progress requests, after checking that version's
`getaddrinfo_with_timeout()` by hand. When moving to a newer cpp-httplib
before then, re-copy that version's registry port here and rebase the patch.
