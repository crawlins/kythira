// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// rule impl-header (composition root, but not listed for this header)
#include <raft/thing_impl.hpp>
#include <kythira/bad/bad.hpp>
// Listed as a module-adapter: the composition root wires in clean's adapter.
#include <kythira/clean/clean.hpp>
// rule other-module (another module's header with no module-adapter line)
#include <kythira/clean/detail.hpp>
int main() { return 0; }
