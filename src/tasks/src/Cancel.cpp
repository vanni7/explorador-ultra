// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/tasks/Cancel.h"

#include <thread>

namespace eu::tasks {

namespace {

bool askBusy(std::mutex &mutex, const std::function<bool()> &deviceBusy)
{
    const std::lock_guard<std::mutex> lock(mutex);
    return deviceBusy && deviceBusy();
}

} // namespace

void CancelToken::yieldToInteractive(std::chrono::milliseconds maxWait) const
{
    using Clock = std::chrono::steady_clock;
    State &state = *m_state;
    const Clock::time_point start = Clock::now();
    if (start < state.freeUntil) {
        return;   // acaba de ceder mucho rato: ahora le toca avanzar
    }
    while (!isCancelled() && askBusy(state.mutex, state.deviceBusy)) {
        if (Clock::now() - start >= maxWait) {
            state.freeUntil = Clock::now() + std::chrono::seconds(1);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace eu::tasks
