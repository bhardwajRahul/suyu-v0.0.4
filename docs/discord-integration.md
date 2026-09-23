# Discord presence and NetPlay integration

## Current implementation

Desktop builds with `USE_DISCORD_PRESENCE=ON` publish the active title through
Suyu's existing Discord application. The Qt frontend keeps its user setting
(`enable_discord_presence`). `suyu-cmd` publishes the title after a successful
load, including when an exported Windows executable or its Steam non-Steam
shortcut starts that executable. All of those export launch methods run the
same `suyu-cmd` code. An active NetPlay room name replaces the activity state
while connected. No address, port, room password, or join token is published.
Room names are visible to people who can see the player's Discord activity
while presence is enabled.

This uses the repository's existing `discord-rpc` dependency. The integration
does not create a separate Discord application for each exported title, so
Discord identifies the activity as Suyu and displays the individual game in
the presence text. Linux and macOS export packaging currently produces source
artifacts, not a playable launcher; a future compiled launcher can use the
same `suyu-cmd` presence path.

## Join from Discord

Discord's current [Social SDK activity documentation](https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1Activity.html)
requires a stable party ID, room capacity with an available slot, and a join
secret before an activity can be joinable. Suyu currently has a direct-connect
room flow, but no launch-time handler that can accept and validate a Discord
join secret. A join button should be added only after that flow exists.

Proposed sequence:

1. Define an opaque, expiring room invite token. Resolve it through the room
   service to a reachable room, and reject expired/full/private rooms. Never put
   a raw room password or private network address in the activity.
2. Add a launch argument or registered protocol handler that receives the
   token, shows the room and requested game, and asks the player to join.
3. Reuse the existing NetPlay direct-connect and local-game lookup paths to
   complete the join. Handle the case where the requested game is absent.
4. Publish party ID, current/max player count, and join secret only for rooms
   that opt into invites. Clear those fields immediately on leave or room close.
5. Register a launch command for installed Suyu. For Steam, distinguish a real
   Steam application ID from Suyu's generated non-Steam shortcut IDs; the
   [Social SDK launch API](https://discord.com/developers/docs/social-sdk/classdiscordpp_1_1Client.html)
   provides separate command and Steam application registration methods.

Discord [recommends the Social SDK](https://support-dev.discord.com/hc/en-us/articles/30125671534359-Migrating-from-the-Legacy-Game-SDK-to-the-Discord-Social-SDK)
for new integrations. Migration should be evaluated before implementing the
join flow because the existing `discord-rpc` library is legacy and the Social
SDK has its own distribution and app setup requirements.

## Achievements

There is no achievement unlock or publish API in the public Social SDK class
index. Discord's [Game Stats Widget](https://discord.com/developer-newsletter/may-2026)
can show game progress, but is currently available to select developers. A
Suyu achievement system would need per-title definitions and reliable,
consented progress signals from the game or emulator. Arbitrary Switch titles
cannot be assigned correct achievements merely by reading their title or save
files. That work should be a separate design and opt-in feature, with Discord
profile publishing considered only if the platform API becomes available.
