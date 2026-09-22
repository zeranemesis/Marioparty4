#include "precompile.hpp"

#include "localization.hpp"

#include <algorithm>
#include <string>

namespace partyboard::ui {

namespace {
// Reuses the modal classes rather than introducing a stylesheet of its own:
// this is a one-off screen and matching the existing dialogs is both less code
// and less to keep in step when the theme changes.
constexpr const char *kWindowClass = "modal";
constexpr const char *kDialogClass = "modal-dialog";
} // namespace

ShaderPrecompile::ShaderPrecompile()
    : WindowSmall(kWindowClass, kDialogClass)
{
    auto *header = append(mDialog, "div");
    header->SetClass("modal-header", true);

    auto *title = append(header, "div");
    title->SetClass("modal-title", true);
    title->SetInnerRML(ui_translate("Compiling shaders"));

    mBody = append(mDialog, "div");
    mBody->SetClass("modal-body", true);
    mBody->SetInnerRML(ui_translate("Preparing..."));
}

void ShaderPrecompile::set_progress(std::size_t created, std::size_t total)
{
    if (mBody == nullptr || total == 0) {
        return;
    }

    const std::size_t percent = std::min<std::size_t>(created * 100 / total, 100);
    // Rebuilding the body every frame for a value that only moves a few hundred
    // times would reflow the document for nothing, and the reflow competes with
    // the compilation this screen exists to wait for.
    if (percent == mLastPercent) {
        return;
    }
    mLastPercent = percent;

    Rml::String body = std::to_string(percent) + "%<br/>" + std::to_string(created) + " / " + std::to_string(total)
        + "<br/><br/>" + ui_translate("This only happens once. The result is kept for future launches.");
    mBody->SetInnerRML(body);
}

} // namespace partyboard::ui
