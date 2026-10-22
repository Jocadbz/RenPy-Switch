# Original diagnostic fixture for this port. SPDX-License-Identifier: MIT
define config.name = "Ren'Py Switch smoke test"
define config.version = "0.1.0"
define config.screen_width = 1280
define config.screen_height = 720
define config.save_directory = "renpy-switch-smoke"
define config.default_text_cps = 35
define narrator = Character(None)
default selected = "none"

init python:
    config.overlay_screens.append("smoke_controls")
    print("[boot:7] smoke-test script initialized", flush=True)

style default:
    font "DejaVuSans.ttf"
    size 32
    color "#ffffff"
style button_text:
    hover_color "#ffd060"
    selected_color "#70ff90"
style button:
    padding (16, 12)
    background "#24364a"

screen main_menu():
    tag menu
    add Solid("#12202d")
    vbox:
        align (0.5, 0.5)
        spacing 24
        text "Ren'Py Switch native smoke test"
        textbutton "Start" action Start()
        textbutton "Load test save" action FileLoad("smoke", confirm=False)
        textbutton "Quit to hbmenu" action Quit(confirm=False)

screen say(who, what):
    window:
        id "window"
        background "#14293bee"
        xfill True
        yalign 1.0
        ysize 240
        padding (40, 30)
        vbox:
            if who:
                text who id "who"
            text what id "what"

screen choice(items):
    vbox:
        align (0.5, 0.35)
        spacing 20
        for item in items:
            textbutton item.caption action item.action

screen smoke_controls():
    zorder 100
    hbox:
        align (0.5, 0.02)
        spacing 12
        textbutton "Save" action FileSave("smoke", confirm=False)
        textbutton "Load" action FileLoad("smoke", confirm=False)
        textbutton "Rollback" action Rollback()
        textbutton "Quit" action Quit(confirm=False)

screen game_menu():
    modal True
    add Solid("#12202dee")
    vbox:
        align (0.5, 0.5)
        spacing 20
        textbutton "Return" action Return()
        textbutton "Save" action FileSave("smoke", confirm=False)
        textbutton "Load" action FileLoad("smoke", confirm=False)
        textbutton "Quit" action Quit(confirm=False)

label start:
    scene expression Solid("#10283b")
    $ print("[boot:8] dialogue label entered", flush=True)
    "Python, Ren'Py, fonts and the renderer reached interactive dialogue."
    "Advance with the controller or touch. Text test: café — Ελληνικά — Привет."
    menu:
        "Choose a branch."
        "First branch":
            $ selected = "first"
        "Second branch":
            $ selected = "second"
    "You selected the [selected] branch. Save here, quit, relaunch, and load."
    "Rollback should restore the choice. Touch and controller navigation should both work."
    "The core smoke test is complete. Audio, movies and suspend/resume need separate tests."
    return
