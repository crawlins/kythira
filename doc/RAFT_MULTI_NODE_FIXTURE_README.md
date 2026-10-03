# Raft Multi-Node Test Fixture

`tests/raft_multi_node_test_fixture.hpp` runs a cluster of real
`kythira::node` instances in one process, connected by the network
simulator. Each running node has a ticker thread that calls
`check_election_timeout()` and `check_heartbeat_timeout()` every
`tick_interval`, the way an event loop drives a deployed node, so elections,
replication and membership changes happen on their own.

Tasks: raft-consensus 700, 710, 730.

## Usage

```cpp
#include "raft_multi_node_test_fixture.hpp"

using kythira::test::raft_multi_node_fixture;

kythira::test::cluster_config cfg;
cfg.node_count = 5;                      // nodes 1..5, all voters
raft_multi_node_fixture f(cfg);
f.initialize_cluster();                  // creates nodes, starts the observer
f.start_all_nodes();

auto leader = f.wait_for_leader(std::chrono::seconds{5});
auto r = f.submit(raft_multi_node_fixture::put_command("k", "v"),
                  std::chrono::seconds{3});
BOOST_CHECK(r.ok);
BOOST_CHECK(f.wait_for_convergence(std::chrono::seconds{5}));
BOOST_CHECK(f.election_safety_violations().empty());
```

Test files using the Folly backend register the fixture by its unqualified
name, since `BOOST_GLOBAL_FIXTURE` pastes its argument into an identifier:
`using kythira::test::folly_init_fixture; BOOST_GLOBAL_FIXTURE(folly_init_fixture);`.

## What it provides

| Area | Members |
|------|---------|
| Lifecycle | `start_node`, `stop_node` (crash; persistence survives), `restart_node`, `add_node` (a node outside the initial cluster) |
| Network | `partition({{...}, {...}})`, `isolate`, `heal`, `set_node_network` / `clear_node_network` (latency, delivery probability) |
| Leadership | `get_leader(among)`, `wait_for_leader(timeout, among)`, `term_of` |
| Client | `submit` (follows the leader), `submit_to`, `await_result` for any byte future (`add_server`, `remove_server`, ...) |
| Logs | `committed_entries`, `committed_logs_mismatch` (Log Matching across all pairs), `wait_for_convergence`, `cluster_summary` |
| Safety | `election_safety_violations` (terms with two leaders), `observed_leaders`, `max_simultaneous_leaders` |

`get_leader` returns the node that claims leadership with a term at least as
high as every other node it considers, so a deposed leader stranded in a
minority is never mistaken for the current one.

A node joining through `add_server` or `add_learner` should be created with
itself as a learner (`f.add_node(4, {1, 2, 3}, {4})`) so it cannot campaign
before the leader's configuration entry reaches it.

## Tests built on it

- `tests/raft_multi_node_fixture_test.cpp`: bootstrap at 3/5/7 nodes, crash
  and restart, leader crash, latency/loss, log inspection (700, 701, 730)
- `tests/raft_multi_node_partition_test.cpp`: leader isolation, follower
  isolation, minority and even splits (710-713)
- `tests/raft_multi_node_property_test.cpp`: seeded randomized failures (731)
- `tests/raft_membership_management_unit_test.cpp`: add/remove server, self
  removal, learner catch-up, concurrent and partitioned changes (702)
- `tests/membership_change_joint_safety_test.cpp`: joint quorum, truncation
  revert, leader counts during changes (membership-change spec)
