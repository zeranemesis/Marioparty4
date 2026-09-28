// Credits: TwilitRealm

#pragma once

#include "port/achievements.h"
#include "window.hpp"

#include <vector>

namespace partyboard::ui {

class AchievementsWindow : public Window {
public:
    AchievementsWindow();
    void update() override;

private:
    void updateTotal();
    void buildRetroAchievements();
    std::vector<Achievement> mSnapshot;
    Rml::Element *mTotalEl = nullptr;
    // Showing the RetroAchievements set rather than the local list.
    bool mRetroAchievements = false;
    uint32_t mRaRevision = 0;
};

} // namespace partyboard::ui
