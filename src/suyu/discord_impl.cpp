// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <string>

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <discord_rpc.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "network/room_member.h"
#include "network/network.h"
#include "suyu/discord_impl.h"
#include "suyu/uisettings.h"

namespace DiscordRPC {

DiscordImpl::DiscordImpl(Core::System& system_) : system{system_} {
    DiscordEventHandlers handlers{};
    // The number is the client ID for suyu, it's used for images and the
    // application name
    // NOTE: This application is owned by million1156 (million@alyocord.com)
    Discord_Initialize("1221314350216646828", &handlers, 1, nullptr);
}

DiscordImpl::~DiscordImpl() {
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordImpl::Pause() {
    Discord_ClearPresence();
}

std::string DiscordImpl::GetGameString(const std::string& title) {
    // Convert to lowercase
    std::string icon_name = Common::ToLower(title);

    // Replace spaces with dashes
    std::replace(icon_name.begin(), icon_name.end(), ' ', '-');

    // Remove non-alphanumeric characters but keep dashes
    std::erase_if(icon_name, [](char c) { return !std::isalnum(c) && c != '-'; });

    // Remove dashes from the start and end of the string
    icon_name.erase(icon_name.begin(), std::find_if(icon_name.begin(), icon_name.end(),
                                                    [](int ch) { return ch != '-'; }));
    icon_name.erase(
        std::find_if(icon_name.rbegin(), icon_name.rend(), [](int ch) { return ch != '-'; }).base(),
        icon_name.end());

    // Remove double dashes
    icon_name.erase(std::unique(icon_name.begin(), icon_name.end(),
                                [](char a, char b) { return a == '-' && b == '-'; }),
                    icon_name.end());

    return icon_name;
}

void DiscordImpl::UpdateGameStatus(bool use_default) {
    const std::string default_text = "suyu is an emulator for the Nintendo Switch";
    const std::string default_image = "suyu_logo";
    const std::string url = use_default ? default_image : game_url;
    DiscordRichPresence presence{};

    std::string room_state;
    if (const auto member = system.GetRoomNetwork().GetRoomMember().lock();
        member && member->IsConnected()) {
        const auto room_name = member->GetRoomInformation().name;
        room_state = room_name.empty() ? "In a NetPlay room" : "NetPlay: " + room_name;
        // Discord limits activity strings to 128 bytes. Leave room for the
        // prefix and avoid publishing the room's address or password.
        if (room_state.size() > 128) {
            size_t length = 128;
            while (length > 0 &&
                   (static_cast<unsigned char>(room_state[length]) & 0xC0) == 0x80) {
                --length;
            }
            room_state.resize(length);
        }
    }

    presence.largeImageKey = url.c_str();
    presence.largeImageText = game_title.c_str();
    presence.smallImageKey = default_image.c_str();
    presence.smallImageText = default_text.c_str();
    presence.state = room_state.empty() ? game_title.c_str() : room_state.c_str();
    presence.details = "Currently in game";
    presence.startTimestamp = game_start_timestamp;
    Discord_UpdatePresence(&presence);
}

void DiscordImpl::Update() {
    const std::string default_text = "suyu is an emulator for the Nintendo Switch";
    const std::string default_image = "suyu_logo";

    if (system.IsPoweredOn()) {
        std::string loaded_title;
        system.GetAppLoader().ReadTitle(loaded_title);
        if (loaded_title.empty()) {
            loaded_title = "Nintendo Switch game";
        }
        if (loaded_title == game_title) {
            UpdateGameStatus(!game_image_available);
            return;
        }
        game_title = std::move(loaded_title);
        game_image_available = false;
        game_start_timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();

        // Used to format Icon URL for suyu website game compatibility page
        std::string icon_name = GetGameString(game_title);
        game_url = fmt::format("https://suyu.dev/images/game/boxart/{}.png", icon_name);

        QNetworkAccessManager manager;
        QNetworkRequest request;
        request.setUrl(QUrl(QString::fromStdString(game_url)));
        request.setTransferTimeout(3000);
        QNetworkReply* reply = manager.head(request);
        QEventLoop request_event_loop;
        QObject::connect(reply, &QNetworkReply::finished, &request_event_loop, &QEventLoop::quit);
        request_event_loop.exec();
        game_image_available = reply->error() == QNetworkReply::NoError;
        UpdateGameStatus(!game_image_available);
        return;
    }

    game_title.clear();
    game_start_timestamp = 0;
    s64 start_time = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    DiscordRichPresence presence{};
    presence.largeImageKey = default_image.c_str();
    presence.largeImageText = default_text.c_str();
    presence.details = "Currently not in game";
    presence.startTimestamp = start_time;
    Discord_UpdatePresence(&presence);
}
} // namespace DiscordRPC
