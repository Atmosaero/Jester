#pragma once
#include <stdexcept>
#include <string>
#include <utility>

namespace trickster {
// O3DE builds MSVC STL with _HAS_EXCEPTIONS=0. In that mode runtime_error
// retains a message pointer instead of copying its bytes. Keep editor error
// messages alive without changing the SDK's STL ABI or exception settings.
class RuntimeError final : public std::runtime_error {
    std::string message;
public:
    explicit RuntimeError(std::string text) : std::runtime_error("Jester error"),message(std::move(text)) {}
    const char* what() const noexcept override { return message.c_str(); }
};
}
