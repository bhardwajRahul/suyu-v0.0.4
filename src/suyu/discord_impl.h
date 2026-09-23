// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

#include "suyu/discord.h"

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update() override;

private:
    std::string GetGameString(const std::string& title);
    void UpdateGameStatus(bool use_default);

    std::string game_url{};
    std::string game_title{};
    bool game_image_available = false;
    std::int64_t game_start_timestamp = 0;

    Core::System& system;
};

} // namespace DiscordRPC
