#pragma once
#include <asterion/kernel/plugin.hpp>
#include <asterion/foundation/serialization.hpp>
namespace asterion {
// Ordered, durable commits; host serializes calls. Failure may have committed:
// implementation must refuse further writes until reopened and recovered.
class JournalPort : public Plugin {
public:
    virtual std::vector<Json> read() const = 0;
    virtual void append(const Json& record) = 0;
};
}
