// **Feature: coap-transport-security, Requirement 9.5**
// Two peers configured with bootstrap_method == edhoc and matching
// identity/peer credentials complete a handshake and derive a working
// (matching) OSCORE context; a peer-credential mismatch fails the
// handshake and prevents session establishment (Requirement 5.4).
//
// Only meaningful when built with LAKERS_AVAILABLE (this test target links
// lakers::lakers explicitly — see tests/CMakeLists.txt); the vcpkg "edhoc"
// feature must be installed for that target to exist (see
// vcpkg-overlays/lakers/README.md).
#define BOOST_TEST_MODULE coap_edhoc_oscore_bootstrap_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT 30

#include <raft/coap_edhoc.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

using namespace kythira;

namespace {

// Same test vectors as lakers' own lib/src/lib.rs test_handshake / this
// project's vcpkg-overlays/lakers/ffi/tests/handshake.rs, so this test is
// exercising known-good EDHOC material rather than ad hoc bytes.
auto hex_decode(const std::string& hex) -> std::vector<std::byte> {
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("bad hex");
    };
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

auto cred_i() -> std::vector<std::byte> {
    return hex_decode(
        "A2027734322D35302D33312D46462D45462D33372D33322D333908A101A5010202412B2001215820AC75E9EC"
        "E3E50BFC8ED60399889522405C47BF16DF96660A41298CB4307F7EB62258206E5DE611388A4B8A8211334AC7D"
        "37ECB52A387D257E6DB3C2A93DF21FF3AFFC8");
}
auto i_key() -> std::vector<std::byte> {
    return hex_decode("fb13adeb6518cee5f88417660841142e830a81fe334380a953406a1305e8706b");
}
auto r_key() -> std::vector<std::byte> {
    return hex_decode("72cc4761dbd4c78f758931aa589d348d1ef874a7e303ede2f140dcf3e6aa4aac");
}
auto cred_r() -> std::vector<std::byte> {
    return hex_decode(
        "A2026008A101A5010202410A2001215820BBC34960526EA4D32E940CAD2A234148DDC21791A12AFBCBAC93622"
        "046DD44F02258204519E257236B2A0CE2023F0931F1F386CA7AFDA64FCDE0108C224C51EABF6072");
}

// A no-op transport for the LAKERS_AVAILABLE-unset stub test below: the
// stub run_edhoc_handshake() throws before ever touching its transport
// argument, so it doesn't need a working one — just something that type-
// checks as edhoc_transport without pulling in the synchronization
// machinery real handshakes need (which would otherwise sit in every
// build's binary, compiled but permanently unreachable, since the default
// build has no vcpkg "edhoc" feature installed and so never takes the
// #ifdef LAKERS_AVAILABLE branch below).
class null_transport : public edhoc_transport {
public:
    auto send(const std::vector<std::byte>&) -> void override {}
    auto receive() -> std::vector<std::byte> override { return {}; }
};

#ifdef LAKERS_AVAILABLE

// A blocking single-producer/single-consumer byte-message queue: run_edhoc_
// handshake() drives each role from its own thread (each side's handshake
// is one blocking call doing several send()/receive() round trips), so
// receive() has to actually wait for the peer thread's send() rather than
// just checking whether something has arrived yet.
class blocking_queue {
public:
    auto push(std::vector<std::byte> message) -> void {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _queue.push_back(std::move(message));
        }
        _cv.notify_one();
    }

    auto pop() -> std::vector<std::byte> {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [&] { return !_queue.empty(); });
        auto msg = std::move(_queue.front());
        _queue.pop_front();
        return msg;
    }

    // Used only by the mismatched-credential test: if the initiator aborts
    // before sending message_3 (because verify_message_2 already failed),
    // the responder would otherwise block forever waiting for it. Returns
    // nullopt on timeout.
    auto pop_or_timeout(std::chrono::milliseconds timeout)
        -> std::optional<std::vector<std::byte>> {
        std::unique_lock<std::mutex> lock(_mutex);
        if (!_cv.wait_for(lock, timeout, [&] { return !_queue.empty(); })) {
            return std::nullopt;
        }
        auto msg = std::move(_queue.front());
        _queue.pop_front();
        return msg;
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<std::vector<std::byte>> _queue;
};

// Connects two edhoc_transport instances so that Peer A's send() feeds Peer
// B's receive() and vice versa, each driven from its own thread.
class channel_transport : public edhoc_transport {
public:
    channel_transport(blocking_queue& outbox, blocking_queue& inbox)
        : _outbox(outbox), _inbox(inbox) {}

    auto send(const std::vector<std::byte>& message) -> void override { _outbox.push(message); }
    auto receive() -> std::vector<std::byte> override { return _inbox.pop(); }

private:
    blocking_queue& _outbox;
    blocking_queue& _inbox;
};

// Marker used only by the mismatched-credential test: distinguishes "the
// peer never sent anything (because it aborted first)" from a real EDHOC
// protocol failure, so the responder side's thread can tell the two apart.
struct peer_never_responded {};

class timeout_channel_transport : public edhoc_transport {
public:
    timeout_channel_transport(blocking_queue& outbox, blocking_queue& inbox,
                              std::chrono::milliseconds timeout)
        : _outbox(outbox), _inbox(inbox), _timeout(timeout) {}

    auto send(const std::vector<std::byte>& message) -> void override { _outbox.push(message); }
    auto receive() -> std::vector<std::byte> override {
        auto msg = _inbox.pop_or_timeout(_timeout);
        if (!msg) throw peer_never_responded{};
        return *msg;
    }

private:
    blocking_queue& _outbox;
    blocking_queue& _inbox;
    std::chrono::milliseconds _timeout;
};

#endif  // LAKERS_AVAILABLE

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_edhoc_oscore_bootstrap_tests)

#ifdef LAKERS_AVAILABLE

BOOST_AUTO_TEST_CASE(matching_credentials_derive_mirrored_oscore_context,
                     *boost::unit_test::timeout(20)) {
    blocking_queue i_to_r;
    blocking_queue r_to_i;
    channel_transport initiator_transport(i_to_r, r_to_i);
    channel_transport responder_transport(r_to_i, i_to_r);

    edhoc_params initiator_params;
    initiator_params.is_initiator = true;
    initiator_params.identity_credential = cred_i();
    initiator_params.identity_private_key = i_key();
    initiator_params.peer_credential = cred_r();

    edhoc_params responder_params;
    responder_params.is_initiator = false;
    responder_params.identity_credential = cred_r();
    responder_params.identity_private_key = r_key();
    responder_params.peer_credential = cred_i();

    // Initiator drives message_1 first; must run before the responder can
    // have anything to receive(), so this can't be parallelized without a
    // real thread per side — sequential is sufficient to prove the crypto.
    std::exception_ptr responder_error;
    oscore_credentials responder_result;
    std::thread responder_thread([&] {
        try {
            responder_result = run_edhoc_handshake(responder_params, responder_transport);
        } catch (...) {
            responder_error = std::current_exception();
        }
    });

    auto initiator_result = run_edhoc_handshake(initiator_params, initiator_transport);
    responder_thread.join();
    if (responder_error) std::rethrow_exception(responder_error);

    BOOST_CHECK(initiator_result.master_secret == responder_result.master_secret);
    BOOST_CHECK(initiator_result.master_salt == responder_result.master_salt);
    // Mirrored: initiator's sender_id must equal responder's recipient_id
    // and vice versa (see run_edhoc_handshake()'s comment on kIdLabelA/B).
    BOOST_CHECK(initiator_result.sender_id == responder_result.recipient_id);
    BOOST_CHECK(initiator_result.recipient_id == responder_result.sender_id);
    BOOST_CHECK(!initiator_result.master_secret.empty());
}

BOOST_AUTO_TEST_CASE(mismatched_peer_credential_fails_handshake, *boost::unit_test::timeout(20)) {
    blocking_queue i_to_r;
    blocking_queue r_to_i;
    channel_transport initiator_transport(i_to_r, r_to_i);
    // The initiator fails at verify_message_2 — after receiving message_2
    // but before ever sending message_3 — so the responder's final
    // receive() (waiting for message_3) would otherwise block forever;
    // bound it instead and treat the timeout as the expected outcome.
    timeout_channel_transport responder_transport(r_to_i, i_to_r, std::chrono::milliseconds(2000));

    edhoc_params initiator_params;
    initiator_params.is_initiator = true;
    initiator_params.identity_credential = cred_i();
    initiator_params.identity_private_key = i_key();
    initiator_params.peer_credential = cred_i();  // wrong: should be cred_r()

    edhoc_params responder_params;
    responder_params.is_initiator = false;
    responder_params.identity_credential = cred_r();
    responder_params.identity_private_key = r_key();
    responder_params.peer_credential = cred_i();

    std::thread responder_thread([&] {
        try {
            run_edhoc_handshake(responder_params, responder_transport);
        } catch (const coap_credential_bootstrap_error&) {
            // Expected: the responder's own verify_message_3 could also be
            // the one to detect the mismatch, depending on scheduling.
        } catch (const peer_never_responded&) {
            // Expected: the initiator threw before ever sending message_3.
        }
    });

    BOOST_CHECK_THROW(run_edhoc_handshake(initiator_params, initiator_transport),
                      coap_credential_bootstrap_error);
    responder_thread.join();
}

#else

BOOST_AUTO_TEST_CASE(edhoc_unavailable_fails_descriptively, *boost::unit_test::timeout(10)) {
    null_transport transport;
    edhoc_params params;
    params.is_initiator = true;
    BOOST_CHECK_THROW(run_edhoc_handshake(params, transport), coap_credential_bootstrap_error);
}

#endif  // LAKERS_AVAILABLE

BOOST_AUTO_TEST_SUITE_END()
