#include <cstdio>
#include <string>
#include <vector>

#include "seat_session.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "seat-session test: %s\n", message);
    return condition;
}

bool run() {
    kopms::SeatSession invalid("", false);
    std::string error;
    if (!expect(!invalid.start({}, &error), "empty seat was accepted")) return false;
    if (!expect(invalid.state() == kopms::SeatSessionState::Failed,
                "invalid seat did not enter failed state")) {
        return false;
    }
    if (!expect(!error.empty(), "invalid seat did not report an error")) return false;

    std::vector<kopms::SeatSessionState> states;
    kopms::SeatSession session("seat0", true);
    if (!expect(session.start(
                    [&states](kopms::SeatSessionState state, const std::string&) {
                        states.push_back(state);
                    },
                    &error),
                "explicit unmanaged seat session did not start")) {
        return false;
    }
    if (!expect(session.active(), "started seat session is not active")) return false;
    if (!expect(session.backend() == kopms::SeatSessionBackend::Logind ||
                    session.backend() == kopms::SeatSessionBackend::Unmanaged,
                "started seat session has no backend")) {
        return false;
    }
    if (!expect(session.dispatch(&error), "seat event dispatch failed")) return false;
    session.stop();
    if (!expect(session.state() == kopms::SeatSessionState::Stopped,
                "stopped seat session has the wrong state")) {
        return false;
    }
    return expect(!states.empty(), "seat state callback was never called");
}

}  // namespace

int main() { return run() ? 0 : 1; }
