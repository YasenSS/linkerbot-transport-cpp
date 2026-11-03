#pragma once

#include <optional>
#include <stdexcept>
#include <utility>

namespace linkerbot::test_support {
    // Return an owned value so both temporary read results and named optionals
    // are safe. An absent value becomes a test exception, never a dereference.
    template <typename T> auto require_value(std::optional<T> value) -> T {
        if (!value.has_value()) {
            throw std::logic_error("Expected an engaged optional in test");
        }
        return std::move(*value);
    }
} // namespace linkerbot::test_support
