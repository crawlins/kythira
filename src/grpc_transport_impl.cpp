/// @file grpc_transport_impl.cpp
/// @brief Translation-unit anchor for the header-only gRPC transport.
///
/// `grpc_client<Types>`/`grpc_server<Types>` are fully defined in
/// `grpc_transport_impl.hpp`; this file gives the `raft_grpc_transport` library
/// a compiled object alongside the generated `raft.pb.cc`/`raft.grpc.pb.cc`,
/// and explicitly instantiates both classes for the example
/// `grpc_kythira_transport_types` bundle so their member bodies are type-checked
/// and emitted once here rather than in every consumer.

#include <raft/grpc_transport_impl.hpp>

namespace kythira {

template class grpc_client<grpc_kythira_transport_types>;
template class grpc_server<grpc_kythira_transport_types>;

}  // namespace kythira
