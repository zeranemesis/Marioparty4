#pragma once

#include "window.hpp"

#include <cstddef>

namespace partyboard::ui {

// The "compiling shaders" screen, shown once before the disc boots.
//
// Aurora drops a draw whose pipeline is not ready rather than delaying it, so
// the pipelines have to exist before gameplay starts or the first visit to a
// scene shows missing geometry. Building them is real work -- the driver
// compiles every one -- and a window sitting there doing nothing is
// indistinguishable from a hang, which is why this exists rather than a silent
// wait.
//
// It is the same first-run step other PC games show, for the same reason: a
// compiled pipeline is specific to the GPU and driver version and cannot be
// shipped, so only the list of them travels with the game and each machine
// builds its own once.
class ShaderPrecompile : public WindowSmall {
public:
    ShaderPrecompile();

    void set_progress(std::size_t created, std::size_t total);

private:
    Rml::Element *mBody = nullptr;
    std::size_t mLastPercent = static_cast<std::size_t>(-1);
};

} // namespace partyboard::ui
