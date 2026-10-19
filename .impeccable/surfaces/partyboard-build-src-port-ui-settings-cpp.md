---
version: 1
slug: "partyboard-build-src-port-ui-settings-cpp"
primary_target: "PartyBoard-build/src/port/ui/settings.cpp"
related_targets: ["PartyBoard-build/res/rml/window.rcss"]
---

# Settings menu (F1 menu: Settings tabs, Players, Play Online)

Mode: Operate. Code-led (Operate surface). Redesign of the existing look; behaviour, controls and approved wording are kept.

Audience and job: the setup owner at a desk, before friends play: find a setting, read it at a glance, change it. Sometimes a player on the couch with a gamepad. Must read on a laptop screen and a TV.

Constraints: RmlUi (RCSS subset), the game's FOT-NewRodin Pro face and Material Symbols Rounded icons; controller, mouse and keyboard parity; the master/detail pane model and the top tab bar stay (controller semantics); the game renders behind the menu.

User decisions (2026-10-18): "clean modern launcher" like Switch / Steam Big Picture settings; minimal text: short labels, one short help line only for the focused setting. Final approved course change (2026-10-18, after the first build): keep the new layout and feel, but keep the ORIGINAL visual look (blue glass panel, cyan frame/focus/selection, cream text, FOT-NewRodin Pro, uppercase tabs and section headings). The earlier ink-navy / star-gold direction is withdrawn. Visual tokens and rules now live in /DESIGN.md.

## Direction contract (final, as shipped at 69aa64b)
THESIS: A console-settings layout in the game's own blue glass: a short list of plain sentence-case rows with their values, and one inspector for the row you're on. It refuses the old crowding (boxed rows, uppercase row labels, paragraph help), not the old look.
OWN-WORLD: The original PartyBoard menu: translucent blue glass panel rgba(37,59,156,90%) with a 2dp #009dda frame, blurred game behind; cream text #E0DBC8 stepped by opacity; cyan #00b4fa for focus ring, active tab and selection; FOT-NewRodin Pro; uppercase bold tabs and section headings; 18dp window corners, 12dp control corners.
STORY: The visitor opens the menu, sees the tab and a short list of rows such as "Window Mode  Windowed". They move down it, the inspector shows the choices plus one short line of help, and they pick one. Done in seconds.
FIRST VIEWPORT: Uppercase tabs across a 64dp band, the active one underlined in cyan; close button top right. The left list (flex 1.25) has small uppercase section headings, then unboxed 52dp sentence-case rows, label left and value right; list actions read as rows; main online actions (Host a game, Find games, Join, Leave lobby) keep a boxed "primary" button look. The right inspector (flex 1) shows the choices as boxed buttons, the current one cyan-filled, then one muted help line. The bottom 80dp of the screen stays clear for the "Menu open" notice.
FORM: Canon settings sheet (standing exit taken by the user), position n/a; seed key: none (canon, user-chosen).
SIGNATURE: The focused row lights as a cyan-ringed, cyan-washed band; the inspector follows it instantly.
FINISH: unreviewed and undocumented is unfinished; DESIGN.md recorded from the shipped build (69aa64b).
