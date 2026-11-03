# Co-op progress sync

Each player runs their own game with their own save. Progress is shared; bodies are not: you won't see the other player, only what they did.

**Status: partly confirmed in the game (2026-11-01), with two copies on one machine.** Working: connecting, items and heart pieces (given with the test keys below), recovery hearts, chest rupees, popups, small chests opening live, the player marker, and the other player's figure. Health stays private and heart containers aren't shared. Enemy kills are shared live (bats and chuchus). Not yet tried: full-size chests opening live, both players opening a chest at once, maps and keys in a dungeon, and playing over a real network.

To build on Fedora, where FFmpeg's headers and shaderc's library name differ from what the Makefile expects:

```sh
sudo dnf install libavcodec-free-devel libavformat-free-devel libavutil-free-devel libswscale-free-devel \
    libshaderc-devel VulkanMemoryAllocator-devel vulkan-headers vulkan-loader-devel SDL3-devel
CPATH=/usr/include/ffmpeg make -j16 SHADERC=-lshaderc_shared build/skyrecomp
```

## Setup

Everyone needs the same game build (v1.0.0) and the same build of this mod. **Back up your saves first**: connecting merges progress into your save permanently, so use a fresh file or one you're happy to merge.

Open the menu (F1), go to the Co-op tab, and under Online:

- **Host a game**: pick a port (27183 unless you change it) and a password, and press Start hosting. With "Open the port on my router automatically" ticked, the game asks your router (UPnP) to forward the port to this computer, for an hour at a time while you host, and removes it when you stop. It then shows the address friends join with, hidden until you press the eye, with a Copy button.
- **Join a game**: type or paste the host's address (`203.0.113.7`, or `203.0.113.7:27183`), the port and the password, and press Join game.

Stop disconnects and sets the mode back to Off. It's all saved in `skyrecomp.ini` (`coop_mode`, `coop_addr`, `coop_port`, `coop_key`, `coop_upnp`), and a game set to host or join there starts that way. For a test run, `LARECOMP_COOP=host:<port>:<key>` or `LARECOMP_COOP=join:<addr>:<port>:<key>` overrides the file until the menu's settings are applied. To run two copies on one machine, give the second its own `LARECOMP_DATA_ROOT`.

**If friends can't connect**: the menu says why. If your router has UPnP turned off, turn it on or forward the port (TCP) by hand. If it says your router is behind another one (common on mobile internet and some fibre), nobody outside can reach you directly: ask your provider for a public address, or use a VPN such as Tailscale and join with its address. Joining your own public address from inside your home often fails even when friends can connect: use your computer's local address (192.168.x.x) there.

The password is never sent: joining answers the host's random challenge with an HMAC of it. The game data itself isn't encrypted.

When you join, you get a copy of the host's inventory and progress, once: every item, upgrade and story and dungeon flag the host has, and their rupees, ammo, treasures, bugs, heart pieces and heart capacity where those are larger than yours. You keep your own name and where you are, and nothing you had is taken away; what only you had reaches the host as usual. After that, the amounts are each player's own again. Pouch contents (shields, bottles) aren't copied.

For testing, `LARECOMP_COOP_TEST=1` logs every event and adds keys in the focused window: F6 gives the next test item, F7 a heart piece, F5 a heart container (not shared), F8 a map and small key for the current scene. They change the same memory a real pickup does.

## What is shared

| | How |
|---|---|
| Items, upgrades, songs, tablets | Item flags, plus the story flags that mean "owns this item" |
| Heart pieces | Differences; each player's capacity grows when their own count completes a heart |
| Recovery hearts picked up | Heals everyone by a heart |
| Rupees from chests | The first gain within 20 seconds of a chest opening; a full wallet keeps only what fits |
| Maps, boss keys | Dungeon flags |
| Small keys | Differences, so finding and spending both carry over |
| Pouch expansions, extra wallets, crystals, key pieces | Differences |
| Chests, doors, boss-defeated and other scene flags | Grow only, which also stops the same item being collected twice |
| Enemy deaths | Live: one another player kills vanishes in front of you, dropping its loot for you too. Per scene (see below) |

Private: heart containers, damage, stamina, rupees spent or earned elsewhere, ammo, treasures, bugs, pouch contents, and all other story flags.

A popup at the bottom right names what another player's progress gave you: items, maps, keys, heart pieces, chest rupees and recovery hearts. Chests, doors and enemy deaths don't show one, nor does catching up on progress for the first few seconds after loading a save or someone joining.

The other player appears as a second real Link when you're both in the same stage, with their own animations. The renderer draws your Link's model again using their bones (`runtime/hle/coop_pose.c`), about 100 ms behind, so it's hidden by walls but can't interact with anything, and has no shadow. Their sword shows drawn or put away as theirs is, with their lighting. What Link carries is recognised by going where he goes: a piece near him that stays put relative to him while he walks about 2 m is his, along with whatever is loaded beside it, so after loading, swords, shields and items other than the practice sword appear once each player has taken a few steps. Their equipment isn't sent, though: pieces your own Link isn't drawing can't appear on theirs, so if they carry something you don't, it won't show. Until your Link has been drawn once, a simple box figure stands in. A marker shows where they are, with their distance. When they're off-screen, an arrow at the screen edge points their way. It's drawn over the picture, so it shows through walls, and it supports two players: with more, it would jump between them.

Each player can also set the name shown over their Link, and in popups, at the top of the Co-op tab. Each player can color their own Link in the Co-op tab of the menu (F1): hair (and eyebrows), eyes, earrings, and each part of the clothes he starts in, from a color wheel or common colors. Both players see them. The renderer makes copies of Link's own textures with each part recolored (told apart by their unlit colors), so shading and detail stay as they were. The classic tunic's parts aren't known yet: its colors are saved but only take effect once its textures have been mapped.

## Known gaps

- **A gripping hand stands in for an open one.** While only one of you holds a sword, the other Link's matching hand is drawn from your own: a fist where theirs is open, or an open hand around their sword.
- **Dark earring colors look greyish.** The earrings' blue is the game's shine on a black texture; a color replaces the texture, and the shine still adds on top.
- **An enemy another player kills just vanishes.** It's asked to delete itself (no death effect), and drops its loot as it does: each player gets their own. Only enemies with a defeat flag are shared; bosses aren't tried. As with the game's own flags, kills are forgotten when you leave a scene.
- **Only bats and chuchus follow the host.** A joining player's bats and chuchus move where the host's do, with their animations (`runtime/hle/coop_enemy.c`): the host sends where each is, which way it faces, its size, its AI state, its animation and (chuchus) whether it has risen, and each takes those just before its own update. Each runs its update, in both games, as if Link stood at whichever player is nearer it (his position and the copies of it enemies aim at, moved for that moment), so awake enemies chase either player, and a joining player's react as the host's did. Their attacks hit only the player really there. A sleeping bat senses only the host's Link (through the game's collision checks, it seems), so the host wakes one when the joining player stands under it, through the bat's own state machine (`changeState` to its waking state). One the joining player hits is theirs from then on: it's hurt and dies in their game, and its kill reaches the host as any does. When a player comes into the host's stage, the host sends its kills there again (the game forgets a stage's kills when you leave it). When the host is somewhere else, the joining player's run on their own. Now and then a bat still sticks on one frame of its animation for a moment. Other enemies still run on their own in each game.
- **Damage isn't shared.** A hit only counts where it lands. (Bats and chuchus die in one hit, so it doesn't show yet.)
- **A boss's heart container may be lost to the other player.** Containers aren't shared, but scene flags are, so if one player beats a boss, the other may no longer be able to fight it or collect theirs. This needs checking at the first boss.
- **Every heart piece is shared, not only those from chests and quests.** Pieces lying in the world use shared scene flags too, so one collected by a player disappears for the other either way.
- **Chest rupees and recovery hearts are recognised by timing and size, not by source.** After opening a chest with no rupees, the next rupees you earn within 20 seconds are shared too. If you keep a chest's text open for longer than that, its rupees aren't. Other 1-heart heals, such as sitting on a chair, count as pickups, and a heart picked up at full health isn't noticed.
- **While disconnected**, whole hearts completed from pieces don't carry over.
- **Pouch items aren't delivered.** Shields, bottles, potions and medals live in pouch slots, so the other player won't receive them.
- **Plot progress isn't shared.** Each player sees cutscenes and advances the story themselves. Some shared items may behave oddly if the plot flag that normally accompanies them isn't set yet.
- **Chests open in front of you, silently.** The lid swings open with no sound or effects. A chest that was dowsable may stay on the dowsing list until the room reloads.
- **While disconnected**, other progress is merged on reconnect by taking the larger counter, so simultaneous offline gains don't add up.

## Design

`runtime/hle/coop_core.c` holds the merge rules and no game knowledge (`tools/test_coop.c`). `runtime/hle/coop_net.c` is non-blocking TCP with the host relaying (`tools/test_coop_net.c`). `runtime/hle/coop.c` supplies addresses and runs once per frame from `runtime/hle/input.c`.

No recompiler patches are involved: changes are found by comparing memory each frame, so the generated game library doesn't need rebuilding.

### How the addresses were established

sshd-rando's symbols are for 1.0.1, so they can't be used directly on 1.0.0.

| Fact | Evidence |
|---|---|
| Flag managers are 0x4000 below the 1.0.1 addresses | The story manager at `main+0x182e1f0` has 1151 reads through exactly the expected vtable slots |
| Story table at `main+0x1649bd0` | Matches all 1088 positions documented in the zeldaret/ss decomp (next best alignment: 607) |
| Item table at `main+0x164a534` | Every known counter fits its known maximum; rupees land in word 61, matching the cheat address |
| Save layout | Read back correctly from a real save (name, capacity, health, area) |

At run time, `sane_layout()` checks the managers mirror the save before syncing, and co-op stays off if they don't. The save address, the managers' scene index and the choice of input tick as the game thread are the main things a first in-game run must confirm.
