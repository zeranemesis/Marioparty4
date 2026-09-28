// Credits: TwilitRealm

#include "achievements.hpp"

#include "port/achievements.h"
#include "port/retroachievements.h"
#include "fmt/format.h"
#include "nav_types.hpp"
#include "pane.hpp"
#include "localization.hpp"
#include "ui.hpp"

namespace partyboard::ui {
namespace {

    struct CategoryInfo {
        AchievementCategory cat;
        const char *label;
    };

    constexpr CategoryInfo kCategories[] = {
        { AchievementCategory::Challenge, "Challenge" },
        { AchievementCategory::Collection, "Collection" },
        { AchievementCategory::Minigame, "Minigame" },
        { AchievementCategory::Misc, "Misc" },
        { AchievementCategory::Glitched, "Glitched" },
    };

    Rml::String build_achievement_info_rml(const Achievement &a)
    {
        Rml::String s = fmt::format(R"(<div class="achievement-header">)"
                                    R"(<span class="achievement-name{}">{}</span>)"
                                    R"(<span class="achievement-badge{}">{}</span>)"
                                    R"(</div>)"
                                    R"(<p class="achievement-desc">{}</p>)",
            a.unlocked ? " unlocked" : "", a.name, a.unlocked ? " unlocked" : " locked", ui_translate(a.unlocked ? "Unlocked" : "Locked"), a.description);

        if (a.isCounter) {
            float fraction = a.goal > 0 ? float(a.progress) / float(a.goal) : 1.0f;
            s += fmt::format(R"(<progress value="{:.3f}" class="{}"/>)"
                             R"(<span class="achievement-progress">{} / {}</span>)",
                fraction, a.unlocked ? "progress-done" : "progress-ongoing", a.progress, a.goal);
        }

        return s;
    }

    class AchievementRow : public FluentComponent<AchievementRow> {
    public:
        AchievementRow(Rml::Element *parent, const Achievement &a)
            : FluentComponent(createRowRoot(parent))
        {
            auto &btn = add_child<Button>(Button::Props { "×" });
            mClearButton = &btn;
            btn.root()->SetClass("achievement-clear", true);

            btn.on_nav_command([this, key = std::string(a.key)](Rml::Event &, NavCommand cmd) {
                if (cmd == NavCommand::Confirm) {
                    if (mConfirming) {
//                        mDoAud_seStartMenu(kSoundClick); // TODO PC
                        AchievementSystem::get().clearOne(key.c_str());
                        resetConfirm();
                    }
                    else {
                        mConfirming = true;
                        mClearButton->set_text("Clear?");
                    }
                    return true;
                }
                if (cmd == NavCommand::Cancel && mConfirming) {
                    resetConfirm();
                    return true;
                }
                return false;
            });

            Component::listen(btn.root(), Rml::EventId::Blur, [this](Rml::Event &) { resetConfirm(); });

            auto *infoDiv = append(mRoot, "div");
            infoDiv->SetClass("achievement-info", true);
            infoDiv->SetInnerRML(build_achievement_info_rml(a));
        }

        bool focus() override
        {
            return mClearButton->focus();
        }

    private:
        static Rml::Element *createRowRoot(Rml::Element *parent)
        {
            auto *doc = parent->GetOwnerDocument();
            auto elem = doc->CreateElement("div");
            elem->SetClass("achievement-row", true);
            return parent->AppendChild(std::move(elem));
        }

        void resetConfirm()
        {
            mConfirming = false;
            mClearButton->set_text("×");
        }

        Button *mClearButton = nullptr;
        bool mConfirming = false;
    };

    Rml::String build_ra_info_rml(const ra::AchievementInfo &a)
    {
        // Title and description come from the server: escaped, or a '<' or '&'
        // in one would be read as markup.
        Rml::String s = fmt::format(R"(<div class="achievement-header">)"
                                    R"(<span class="achievement-name{}">{}</span>)"
                                    R"(<span class="achievement-badge{}">{} - {} pts</span>)"
                                    R"(</div>)"
                                    R"(<p class="achievement-desc">{}</p>)",
            a.unlocked ? " unlocked" : "", escape(a.title), a.unlocked ? " unlocked" : " locked",
            ui_translate(a.unlocked ? "Unlocked" : "Locked"), a.points, escape(a.description));
        // Every row gets a bar. Only achievements the set measures have steps in
        // between ("3/9"); the others are all-or-nothing in RetroAchievements,
        // so their bar is empty until the unlock and full after it.
        float fraction = 0.0f;
        Rml::String label;
        if (a.unlocked) {
            fraction = 1.0f;
            label = ui_translate("Unlocked");
        } else if (!a.progress.empty()) {
            fraction = a.progressPercent / 100.0f;
            label = fmt::format("{} ({:.0f}%)", a.progress, a.progressPercent);
        } else {
            label = "0%";
        }
        s += fmt::format(R"(<progress value="{:.3f}" class="{}"/>)"
                         R"(<span class="achievement-progress">{}</span>)",
            fraction, a.unlocked ? "progress-done" : "progress-ongoing", label);
        return s;
    }

    // A RetroAchievements row: read-only, the server owns the unlock.
    class RaAchievementRow : public FluentComponent<RaAchievementRow> {
    public:
        RaAchievementRow(Rml::Element *parent, const ra::AchievementInfo &a)
            : FluentComponent(createRowRoot(parent))
        {
            // The badge, or an empty square of the same size while it downloads,
            // so the text of every row starts at the same place.
            if (!a.image.empty()) {
                auto *icon = append(mRoot, "img");
                icon->SetAttribute("src", a.image);
                icon->SetClass("achievement-icon", true);
            }
            else {
                auto *icon = append(mRoot, "div");
                icon->SetClass("achievement-icon", true);
                icon->SetClass("pending", true);
            }
            auto *infoDiv = append(mRoot, "div");
            infoDiv->SetClass("achievement-info", true);
            infoDiv->SetInnerRML(build_ra_info_rml(a));
        }

    private:
        static Rml::Element *createRowRoot(Rml::Element *parent)
        {
            auto *doc = parent->GetOwnerDocument();
            auto elem = doc->CreateElement("div");
            elem->SetClass("achievement-row", true);
            return parent->AppendChild(std::move(elem));
        }
    };

} // namespace

void AchievementsWindow::buildRetroAchievements()
{
    mRetroAchievements = true;
    mRaRevision = ra::revision();
    // An opaque, higher-contrast variant of the window (res/rml/window.rcss):
    // the list is read over a busy game scene.
    mRoot->SetClass("achievements", true);
    {
        auto elem = mDocument->CreateElement("div");
        elem->SetClass("achievement-total", true);
        mTotalEl = mRoot->AppendChild(std::move(elem));
        updateTotal();
    }
    for (const bool unlockedTab : { false, true }) {
        add_tab(unlockedTab ? "Unlocked" : "Locked", [this, unlockedTab](Rml::Element *content) {
            const auto list = ra::achievements();
            auto &pane = add_child<Pane>(content, Pane::Type::Controlled);
            int unlocked = 0;
            uint32_t points = 0, totalPoints = 0;
            for (const auto &a : list) {
                unlocked += a.unlocked;
                totalPoints += a.points;
                points += a.unlocked ? a.points : 0;
            }
            const float fraction = list.empty() ? 0.0f : float(unlocked) / float(list.size());
            pane.add_rml(fmt::format(R"(<div class="ra-summary">)"
                                     R"(<span class="ra-summary-user">RetroAchievements - {}</span>)"
                                     R"(<span class="ra-summary-count">{} / {} {} - {} / {} pts</span>)"
                                     R"(</div>)"
                                     R"(<progress class="ra-total" value="{:.3f}"/>)",
                ra::username(), unlocked, list.size(), ui_translate("unlocked"), points, totalPoints, fraction));
            for (const auto &a : list) {
                if (a.unlocked == unlockedTab) {
                    pane.add_child<RaAchievementRow>(a);
                }
            }
            pane.finalize();
        });
    }
}

AchievementsWindow::AchievementsWindow()
{
    if (ra::state() == ra::State::Playing) {
        buildRetroAchievements();
        return;
    }
    const auto all = AchievementSystem::get().getAchievements();
    if (all.empty()) {
        // Nothing local to show: say where the achievements come from.
        add_tab("RetroAchievements", [this](Rml::Element *content) {
            auto &pane = add_child<Pane>(content, Pane::Type::Controlled);
            pane.add_section("RetroAchievements");
            pane.add_text(ra::statusText());
            pane.add_text("Log in from Settings > RetroAchievements.");
            pane.finalize();
        });
        return;
    }

    {
        auto elem = mDocument->CreateElement("div");
        elem->SetClass("achievement-total", true);
        mTotalEl = mRoot->AppendChild(std::move(elem));
        updateTotal();
    }

    for (const auto &catInfo : kCategories) {
        int catTotal = 0;
        for (const auto &a : all) {
            if (a.category == catInfo.cat) {
                ++catTotal;
            }
        }
        if (catTotal == 0) {
            continue;
        }

        add_tab(catInfo.label, [this, cat = catInfo.cat](Rml::Element *content) {
            const auto achievements = AchievementSystem::get().getAchievements();

            int total = 0, unlocked = 0;
            for (const auto &a : achievements) {
                if (a.category == cat) {
                    ++total;
                    if (a.unlocked) {
                        ++unlocked;
                    }
                }
            }

            auto &pane = add_child<Pane>(content, Pane::Type::Controlled);

            pane.add_section(fmt::format("{} / {} {}", unlocked, total, ui_translate("unlocked")));

            for (const auto &a : achievements) {
                if (a.category != cat) {
                    continue;
                }
                pane.add_child<AchievementRow>(a);
            }

            pane.add_section("Actions");

            auto &clearAllBtn = pane.add_button("Clear All Achievements");
            auto *clearAllPtr = &clearAllBtn;
            auto confirmingAll = std::make_shared<bool>(false);

            clearAllBtn.on_nav_command([clearAllPtr, confirmingAll](Rml::Event &, NavCommand cmd) {
                if (cmd == NavCommand::Confirm) {
                    if (*confirmingAll) {
//                        mDoAud_seStartMenu(kSoundClick); // TODO PC
                        AchievementSystem::get().clearAll();
                        *confirmingAll = false;
                        clearAllPtr->set_text("Clear All Achievements");
                    }
                    else {
                        *confirmingAll = true;
                        clearAllPtr->set_text("Are you sure?");
                    }
                    return true;
                }
                if (cmd == NavCommand::Cancel && *confirmingAll) {
                    *confirmingAll = false;
                    clearAllPtr->set_text("Clear All Achievements");
                    return true;
                }
                return false;
            });
            clearAllBtn.listen(Rml::EventId::Blur, [clearAllPtr, confirmingAll](Rml::Event &) {
                *confirmingAll = false;
                clearAllPtr->set_text("Clear All Achievements");
            });

            pane.finalize();
        });
    }
}

void AchievementsWindow::update()
{
    if (mRetroAchievements) {
        if (ra::revision() != mRaRevision) {
            mRaRevision = ra::revision();
            refresh_active_tab();
            updateTotal();
        }
        Window::update();
        return;
    }
    const auto current = AchievementSystem::get().getAchievements();
    bool dirty = current.size() != mSnapshot.size();
    if (!dirty) {
        for (size_t i = 0; i < current.size(); ++i) {
            if (current[i].progress != mSnapshot[i].progress || current[i].unlocked != mSnapshot[i].unlocked) {
                dirty = true;
                break;
            }
        }
    }
    if (dirty) {
        mSnapshot = current;
        refresh_active_tab();
        updateTotal();
    }
    Window::update();
}

void AchievementsWindow::updateTotal()
{
    if (mTotalEl == nullptr) {
        return;
    }
    if (mRetroAchievements) {
        const auto list = ra::achievements();
        int unlocked = 0;
        for (const auto &a : list) {
            unlocked += a.unlocked;
        }
        const int pct = list.empty() ? 0 : static_cast<int>(unlocked * 100 / list.size());
        mTotalEl->SetInnerRML(fmt::format("{}%", pct));
        return;
    }
    const auto all = AchievementSystem::get().getAchievements();
    int total = static_cast<int>(all.size());
    int unlocked = 0;
    for (const auto &a : all) {
        if (a.unlocked) {
            ++unlocked;
        }
    }
    const int pct = total > 0 ? (unlocked * 100 / total) : 0;
    mTotalEl->SetInnerRML(fmt::format("{}%", pct));
}

} // namespace partyboard::ui
