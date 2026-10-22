// How the connection set-up gives up: an exception that carries the state to
// report and a sentence for the user. Thrown anywhere below the session and
// reported once, where the session catches it.

#pragma once

#include <stdexcept>
#include <string>

#include "casement/protocol.h"

namespace vmware {

class Failure : public std::runtime_error {
public:
    Failure(casement::State state, const std::string& text) : std::runtime_error(text), state(state) {}
    casement::State state;
};

inline Failure failed(const std::string& text) { return Failure(casement::State::failed, text); }

/// Thrown when the user disconnects while the connection is being made:
/// nothing is reported.
class Stopped : public std::exception {
public:
    const char* what() const noexcept override { return "stopped"; }
};

}  // namespace vmware
