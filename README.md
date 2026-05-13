# Luminar Light's OpenGOAL Mod Base

This is my personal mod base. I created it because I got tired of having to manually copy all my new changes to every repo I had. By having a mod base, I can more easily maintain all my mods.

As I said, this is my personal mod base. Feel free to use it, but expect limited/no support. This mod base doesn't have any fancy stuff that would make things easier for beginners. It is designed to have as little bloat as possible.

The mod base contains a level called `lltest2`. You may find some examples of the mod base's features implemented in it. Please do not change anything about the level, otherwise you will run into merge conflicts. In the future I might add more and more examples into it, to show people what are currently possible.

## Mod Base's Custom Changes

### Load All SBKs

The c++ side now has enough "space" to allow us to load all SBK files at once. The first time the game wants to load a level sound bank (SBK file), it will load all the SBKs instead. Default functionality of the level soundbank loading is removed/disabled. I emptied the `sound-banks` property of all `level-load-info` objects, because the changes I made make that property redundant/useless.

This change is currently only implemented for TPL. It wouldn't make too much sense for Jak II, because that game has sounds with same name but different content in different SBKs, so having all SBKs loaded could mess with what you hear ingame. There are no such conflicts in TPL, so it is safe there.

---

### Custom Music

I borrowed the custom music system from 'The Forgotten Lands' mod, and made some changes to it. As far as I know, the base of that whole system was initially created by Zed, and then it was greatly expanded by Hat-Kid. Please let me know if this is inaccurate and I will adjust this section.
Later the other modbase found a worthy replacement for SFML, called 'miniaudio', and they let me look at their changes. I implemented these on our side. 

I implemented a way to use custom music for music ambients. To use it, just use the `custom-music` lump instead of the `music` lump in your music ambient. Also expanded the 3D debug text that is drawn when you are displaying music ambients.

I made some other minor changes to the custom music system for now, the GOAL side is quite different now but the c++ side is mostly unchanged. For the future I want to make its namings more generic/neutral (so they don't refer to The Forgotten Lands in the name). I also want to implement a way to have music variants/flavors, and I already have a clear vision for it, but I will have to rewrite a lot of code on the c++ side. And while there, I will probably do a general cleanup of that code as well.
Also, for now, the TFL hints system is not reimplemented, since we switched to miniaudio. It is planned though.

---

### Load Boundary Improvements

I have extended the functionality of the load boundaries, to allow restricting them by tasks. You can do this with the new `:tasks` thing I added. If it is not added, things will behave like before. But when it is there, all the listed tasks will need to be completed before the load boundary works. Due to my lack of skill, you have to pass the tasks as strings. But hopefully it can be improved in the future.

See here for an example:

```
(static-load-boundary :flags (player )
  :top 322236.6562  :bot -524288.0000
  :points (  -140670.9062 -353851.7500   6646.9482 -375187.6875   -4327.1040 -433914.7187 )
  :fwd (load village1 firecanyon)
  :tasks ("training-gimmie" "training-door")
  )
```

---

### Navmesh Improvements (Custom Navmesh)

I made changes that allow placing custom navmesh into Jak 1 levels. This will hopefully become useless one day, if proper navmesh support is ever added to OpenGOAL.

The navmesh system in Jak II is more advanced, I haven't managed to figure it out yet.

#### Getting Started

Please keep in mind that you are expected to be familiar with custom levels and GOAL. Still, I tried to make things as understandable as possible.

I would recommend copying an existing navmesh as a start. You can use the inspect method I made. The actor whose navmesh you want to copy must be loaded. Example:
`(inspect (-> (the-as entity-actor (entity-by-name "snow-bunny-55")) nav-mesh))`

You should change the origin and bounds, depending on where you want to place your navmesh.

I usually just remove the nodes, because I do not understand it and things seem fine without it. But keep in mind that every navmesh that is in the game has at least one node.

We do not understand route, but it is needed - otherwise game will crash. If you copy an existing navmesh, the route data is copied correctly. But since we don't understand it, for fully custom navmesh we can never have proper route data. Correct route data is essential if you want to take advantage of gap triangles (where enemies jump).

You can make multiple enemies use the same navmesh. To do this, create the navmesh through code for the first actor, like in the example. And for the other actors, add a lump that tells the game to use another actor's navmesh. Reference is by aid. Example: `"nav-mesh-actor": ["uint32", 40000]`. Tip: You can do the same thing with paths, using the `path-actor` lump.

If the game crashes when you approach a custom navmesh, make sure you added `:custom-hacky? #t` to your custom navmesh definition. If that is there, then check if the actor has a path. It needs a path.

If something is still unclear, please look at the code. I added a lot of comments.

#### Final Words

I am not an expert at decompiling, so my methods were not the most efficient. But with a lot of time, I managed to figure things out. There are probably people who could do this a lot better than me. Hopefully it will happen.

Also, I know my inspect method is not perfect. But it is very tedious to write such a thing, so I just included what we really need. And I think the nodes part could use a cleanup.

I am happy if anyone finds this useful. But I have a request: If you learn more about navmeshes, especially things that would benefit other modders as well, please let me know. And maybe we will add it to this branch.

---

### Miscellaneous Minor Changes

- The 'PS2 Actor Vis' setting is force-disabled in TPL. This is because we have no way to control the actor birth distance, and just using this made things very convinient.
- Added a custom level actor vis forcing to the code, but it is probably currently unused due to the above change.
- I have "fixed" the index of `test-zone` - it should be unique, but it wasn't. This will make it much easier to add and understand new levels, believe me. Always set the index correctly for any new levels you add.
- Use the `BIG_COLLIDE_CACHE_SIZE` by default. This should make the game able to handle much more collision ingame.
- Ambient music marks will now try to display the music and flava data of the marks.
- The code now should refer to the `(game-task max)` everywhere it is supposed to. No more hardcoded numbers. This makes it safe to add new tasks.
- Changed the way `*game-counts*` is handled - it is allocated differently, seemed to work more reliably this way. Also, we no longer care about the game counts file, instead we have the base game values hardcoded, and an easy way to add more. This is much easier to handle. There is a comment to indicate that you can add extra entries there. To avoid merge conflicts, it is probably a good idea to not delete this comment.
- Added two new properties to the `level-load-info` type: `custom-level?` (you should set this to true for your custom levels), and `custom-music` (if your level uses custom music). Keep in mind that both of these are of type `symbol`. The default value of that type is 0. So if you want to check these in conditions, you need to be aware that they won't actually be `#f` in the levels where they weren't defined... they will be 0, which is actually considered "not false" in GOAL, so "true". I am considering defining these properties with value `#f` in the existing levels...
- Commented out some annoying "Failed to find texture at..." console logs, because they were being spammed. I don't think they are really useful nowadays anyway.
- Commented out an assert (`ASSERT(diff < 7);`), which was potentially making it more difficult to build custom levels that had ugly collision geometry. I am not sure how much it helps though, but hopefully it helps somewhat.
- Added a hack that allows us to define the volumes for the water volumes. This hack was invented by Hat-Kid, and I still don't understand why it works, but it does.
- Borrowed a trick from the other mod base (the one by barg and Zed). As you may know, the project dir is found by looking for `jak-project` in the directory path. But if your repository doesn't have that name, it won't be found. They rewrote the function that finds the project directory. But I think it is not working on all platforms yet. I will keep observing their modbase and update this if necessary. What we borrowed for now is working fine on Windows.
- Improved the debug Nav Mesh display. If you enable 'Nav Mesh Extras', you will see the IDs of the vertexes and the triangles.
- When you press L2+UP, the HUD now shows the true maximum obtainable number of orbs, flies, and cells instead of the hardcoded values of 2000, 112, and 101.
- Now you can override which particle a `launcher` should use, using the `part-override` lump (symbol type) - if not specified, or the value you provided does not meet any conditions in the code, then it will continue evaluating the particle choice like vanilla.
- Added a `*force-launcher-active*` global symbol, to allow forcing all `launcher`s to be active regardless of Jak distance or blue eco status. Useful for checking the particles.

---

## Future Plans

- Implement flavor/variant support for custom music.
- Make the TFL hints system work again.
- Clean up GitHub Actions.
- Understand more about the navmesh system (node, route).
- Show how to add new `battle` actors, also show the part that needs to be done through code. I have already touched the battle system in the past. I should clear things up and implement an example.
- Rewrite the warp gate system, to make it more extendable. I imagine a lot of custom level mods will want to use the warp gate. But its code is ugly and not easy to extend.
- Re-allow defining a water-actor for pontoons. It has valid use cases.

## How To Use

While you may think that forking this mod base is preferable, keep in mind that a fork of a public repo can not be private. For this reason, you might want to consider duplicating this repo, and adding this mod base as a remote so you can update from it.

Here is a guide for duplicating the repo: https://docs.github.com/en/repositories/creating-and-managing-repositories/duplicating-a-repository

For convenience, please ensure that your main branch is called `master`. This is important if you want the GitHub workflows to just work easily.

Once your repository exists, you should add this mod base as a remote. This will allow you to stay up to date:

```
git remote add ll-modbase https://github.com/LuminarLight/LL-OpenGOAL-ModBase
```

The command above only needs to be ran once per repository. Afterwards, here is how you can update from the mod base any time:

```
git fetch ll-modbase
git merge ll-modbase/master --allow-unrelated-histories --no-commit
```

The `--no-commit` flag will prevent the command from automatically doing a commit. This will allow you to review all the changes. I think this is very preferable, so please keep using this flag. Also, if you remove the flag, things will still only be instantly commited if there are no conflicts.

### Updating from Vanilla

This is how I update the mod base to stay up to date with OpenGOAL. You don't need to do this (I am supposed to do this to keep the mod base up to date), it is only here so I don't forget (first line only needs to be ran once per repo):

```
git remote add vanilla https://github.com/open-goal/jak-project
git fetch vanilla
git merge vanilla/master --allow-unrelated-histories --no-commit
```

## Final Words

As I said, this is my personal mod base. Support is limited/nonexistent. Still, I imagine there may be people who will find it useful. I am happy if that happens. And you can still reach out to me if you want, but I won't be able to help with basic stuff.

I tried to mention if a change was developed by someone else. If I missed anyone, please let me know and I will correct it. On the other hand, if you use this mod base or parts of it, please be respectful regarding crediting the people behind the features/changes.

*~~Luminar Light*

---

<p align="center">
  <img width="500" height="100%" src="./docs/img/logo-text-colored-new.png">
</p>

<p align="center">
  <a href="https://opengoal.dev/docs/intro" rel="nofollow"><img src="https://img.shields.io/badge/Documentation-Here-informational" alt="Documentation Badge" style="max-width:100%;"></a>
  <a title="Crowdin" target="_blank" href="https://crowdin.com/project/opengoal"><img src="https://badges.crowdin.net/opengoal/localized.svg"></a>
  <a target="_blank" rel="noopener noreferrer" href="https://github.com/open-goal/jak-project/actions/workflows/build-matrix.yaml"><img src="https://github.com/open-goal/jak-project/actions/workflows/build-matrix.yaml/badge.svg" alt="Linux and Windows Build" style="max-width:100%;"></a>
  <a href="https://www.codacy.com/gh/open-goal/jak-project/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=open-goal/jak-project&amp;utm_campaign=Badge_Grade" rel="nofollow"><img src="https://app.codacy.com/project/badge/Grade/29316d04a1644aa390c33be07289f3f5" alt="Codacy Badge" style="max-width:100%;"></a>
  <a href="https://discord.gg/VZbXMHXzWv"><img src="https://img.shields.io/discord/756287461377703987" alt="Discord"></a>
</p>

## Please read first <!-- omit from toc -->

> [!IMPORTANT]
> Our repositories on GitHub are for development of the project and tracking active issues. Most of the information you will find here pertains to setting up the project for development purposes and is not relevant to the end-user.

For a setup guide on how to install and play the game there is the following video that you can check out: https://youtu.be/K84UUMnkJc4

For questions or additional information pertaining to the project, we have a Discord for discussion here: https://discord.gg/VZbXMHXzWv

Additionally, you can find further documentation and answers to **frequently asked questions** on the project's main website: https://opengoal.dev

> [!WARNING]
> **Do not use this decompilation project without the use of your own legally purchased copy of the game.** OpenGOAL does not include any assets from the original games, so you must provide your own legitimately obtained PS2 copy of the game. OpenGOAL supports every retail PAL, NTSC, and NTSC-J build, including Greatest Hits copies. Please note that does NOT include any of the later releases (PS3/PS4/PS5).

- [Project Description](#project-description)
  - [Current Status](#current-status)
  - [Methodology](#methodology)
- [Setting up a Development Environment](#setting-up-a-development-environment)
  - [OS Setup](#os-setup)
  - [Editor Setup](#editor-setup)
  - [Building and Running the Game](#building-and-running-the-game)
    - [Extract Assets](#extract-assets)
    - [Build the Game (Running the Compiler)](#build-the-game-running-the-compiler)
    - [Run the Game](#run-the-game)
      - [Connecting the REPL to the Game](#connecting-the-repl-to-the-game)
      - [Running the Game Without Auto-Booting](#running-the-game-without-auto-booting)
- [Technical Project Overview](#technical-project-overview)

## Project Description

The project's goal is to port the original trilogy (Jak 1 -> Jak 3) to PC. Over 98% of the games were written in GOAL, a custom LISP language developed by Naughty Dog. Our strategy is:
- decompile the original game code into human-readable GOAL code
- develop our own compiler for GOAL and recompile the game code for x86-64
- create a tool to extract game assets into formats that can be easily viewed or modified
- create tools to repack game assets into a format that our port uses.

Our objectives are:
- make the port a "native application" on x86-64, with high performance. It shouldn't be emulated, interpreted, or transpiled.
- Our GOAL compiler's performance should be around the same as unoptimized C.
- try to match things from the original game and development as possible. For example, the original GOAL compiler supported live modification of code while the game is running, so we do the same, even though it's not required for just porting the game.
- support modifications. It should be possible to make edits to the code without everything else breaking.

At the moment we support **x86_64** on Windows, Linux and macOS (via Rosetta translation).  There are no plans to ever make a mobile release.

### Current Status

- Jak 1 has been considered in a polished, complete state for years at this point.
- Jak 2 is considered in beta due to a few issues we are aware of that need fixing, however to the casual user, the game is essentially complete.
- Jak 3 has a good amount of work left to do.

![](./docs/img/promosmall1.png)
![](./docs/img/promosmall2.png)

YouTube playlist showcasing some of the early progress for Jak 1:
https://www.youtube.com/playlist?list=PLWx9T30aAT50cLnCTY1SAbt2TtWQzKfXX

### Methodology

To assist with decompiling, we've built a decompiler that can process GOAL code and unpack game assets. We manually specify function types and locations where we believe the original code had type casts (or where they feel appropriate) until the decompilation succeeds, then we clean up the output of the decompiled code by adding comments and adjusting formatting, then save it in `goal_src/`.

Our decompiler is designed specifically for processing the output of the original GOAL compiler. As a result, when given correct casts, it often produces code that can be directly fed into a compiler and works perfectly. This is continually tested as part of our unit tests.

## Setting up a Development Environment

The remainder of this README is aimed at people interested in building the project from source, typically with the intention of contributing as a developer.

If this does not sound like you and you just want to play the game, refer to the above section [Quick Start](#quick-start)

### OS Setup

- [Windows](/docs/setup/system/windows.md)
- [Linux](/docs/setup/system/linux.md)
- [MacOS](/docs/setup/system/macos.md)
- [Docker](/docs/setup/system/docker.md)

### Editor Setup

You can of course use whatever editor you want, but here is some documentation that should help you get started on some of the editor's we have used and have written about:

- [Visual Studio (Windows)](/docs/setup/dev/vs.md)
- [Visual Studio Code](/docs/setup/dev/vscode.md)
- [Zed](/docs/setup/dev/zed.md)

### Building and Running the Game

Getting a running game involves 4 main steps:

1. Build C++ tools (follow Getting Started steps above for your platform)
2. Extract assets from the game
3. Build the game
4. Run the game

#### Extract Assets

First, we have to setup our environment so we know which game and version we are operating with. For the black label version of Jak 1 we would run the following:

```sh
task set-game-jak1
task set-decomp-ntscv1 # or for example for PAL, `task set-decomp-pal`
```

> Run `task --list` to see the other available options

Next, ensure you extract your ISO file contents into the relevant `iso_data/<game-name>` folder.  In the case of Jak 1 this is `iso_data/jak1`.

Once this is done, open a terminal in the `jak-project` folder and run the following:

```sh
task extract
```

#### Build the Game (Running the Compiler)

The next step is to build the game itself.  To do so, in the same terminal run the following:

```sh
task repl
```

You will be greeted with a prompt like so:

```sh
 _____             _____ _____ _____ __
|     |___ ___ ___|   __|     |  _  |  |
|  |  | . | -_|   |  |  |  |  |     |  |__
|_____|  _|___|_|_|_____|_____|__|__|_____|
      |_|
Welcome to OpenGOAL 0.8!
Run (repl-help) for help with common commands and REPL usage.
Run (lt) to connect to the local target.

g >
```

Run the following to build the game:

```sh
g > (mi)
```

> IMPORTANT NOTE! If you're not using the non-default version of the game, you may hit issues trying to run `(mi)` in this step. An example error might include something like:
>
> `Input file iso_data/jak1/MUS/TWEAKVAL.MUS does not exist.`
>
> This is because the decompiler inputs/outputs using the `gameName` JSON field in the decompiler config. For example if you are using Jak 1 PAL, it will assume `iso_data/jak1_pal` and `decompiler_out/jak1_pal`.  Therefore, you can inform the REPL/compiler of this via the `gameVersionFolder` config field described [here](./goal_src/user/README.md)

#### Run the Game

Finally the game can be launched.  Open a second terminal from the `jak-project` directory and run the following:

```sh
task boot-game
```

The game should boot automatically if everything was done correctly.

##### Connecting the REPL to the Game

Connecting the REPL to the game allows you to inspect and modify code or data while the game is running.

To do so, in the REPL after a successful `(mi)`, run the following:

```sh
g > (lt)
```

If successful, your prompt should change to:

```sh
gc>
```

For example, running the following will print out some basic information about Jak:

```sh
gc> *target*
```

##### Running the Game Without Auto-Booting

You can also start up the game without booting.  To do so run the following in one terminal

```sh
task run-game
```

And then in your REPL run the following (after a successful `(mi)`):

```sh
g > (lt)
[Listener] Socket connected established! (took 0 tries). Waiting for version...
Got version 0.8 OK!
[Debugger] Context: valid = true, s7 = 0x147d24, base = 0x2123000000, tid = 2438049

gc> (lg)
10836466        #xa559f2              0.0000        ("game" "kernel")

gc> (test-play)
(play :use-vis #t :init-game #f) has been called!
0        #x0              0.0000        0

gc>
```

## Technical Project Overview

Some more detail about the various components of the project can be found [here](/docs/project-overview.md)
