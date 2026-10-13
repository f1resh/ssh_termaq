#pragma once
#include "core/Types.h"

namespace sc {

/**
 * Blocking UI prompts used by transports (implemented by SessionManager per connection).
 * prompt() blocks the calling (transport) thread until the user answers, the connection is closed,
 * or the prompt is withdrawn — in the latter cases it returns {cancelled=true}.
 * Never call it from the UI thread.
 */
class Prompter {
public:
    virtual ~Prompter() = default;
    virtual PromptResponse prompt(const PromptInput& input) = 0;
};

}  // namespace sc
