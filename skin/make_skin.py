"""Builds Chord Stage's skins/ folder: the ui.json layout from the SDK's tools/gen_ui.py, plus a
big Now / Next display across the top half of the PLAY page, which ui.json cannot express.

The PLAY page in ui.json leaves its first two rows empty; this script puts there:
    NOW   the chord you just played        (parameter "Now",  large)
    NEXT  the chord to play next            (parameter "Next", very large, accent colour)
    STEP  where you are in the progression  (parameter "Step")
Each is a Label of type "Value" bound to its parameter, so the MPC redraws it whenever the
plugin reports the parameter changed.

Run after changing ui.json, after one normal build (it needs build/params.json):

    python3 skin/make_skin.py          (from the project folder)
    python3 ../mpc-plugin-builder-sdk/tools/build.py .

build.py then copies skins/ verbatim instead of generating the UI.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.environ.get("MPC_SDK") or os.path.join(os.path.dirname(PROJECT), "mpc-plugin-builder-sdk")

FONT = "Titillium Web"
WHITE, MUTED, ACCENT = "ffffffff", "ff8a8a8e", "ff9b63ff"   # ACCENT = the violet UI kit


def label(name, kind, bounds, height, colour, style="Regular", justification="left verticallyCentred",
          param_handle=None, text=None, case="Original"):
    data = {"version": 1,
            "textStyle": {"version": 1, "font": {"version": 1, "name": FONT, "style": style, "height": float(height)},
                          "colour": colour, "justification": justification, "case": case},
            "type": kind, "handleName": "Data"}
    if text is not None:
        data["text"] = text
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": "Label", "data": data},
            "handle remapping": {"version": 1, "map": [{"key": "Data", "value": param_handle}] if param_handle else []},
            "bounds": {"version": 1, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show",
                       "whenVisible": "Always", "boundsType": "Absolute", "bounds": bounds}}


def main():
    params_path = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params_path):
        sys.exit("build/params.json is missing: run tools/build.py once first")
    params = json.load(open(params_path))["params"]
    handle = {p["name"]: "Parameter %d" % p["index"] for p in params}
    for need in ("Now", "Next", "Step"):
        if need not in handle:
            sys.exit("parameter %s not found in build/params.json" % need)

    tmp = tempfile.mkdtemp()
    try:
        subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params_path,
                               "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp,
                               "--kit-cache", os.path.join(PROJECT, "build")])
        tui_path = os.path.join(tmp, "TUI.json")
        tui = json.load(open(tui_path))
        play = next(t for t in tui["pageData"]["tabs"] if t["tabName"] == "PLAY")
        comps = play["componentDefinition"]["componentsData"]
        # Rows 1-2 of the grid (y 45..303) are free: NOW + STEP on the left, NEXT big on the right
        comps += [
            # Headings are "Name" labels (the parameter's name in capitals): fixed "Text" labels do
            # not draw on the MPC (seen on a Live III, OS 3.10.1)
            label("now heading", "Name", "40 40 380 34", 26, MUTED, param_handle=handle["Now"], case="Upper Case"),
            # The MPC draws label text at about 2/3 of font.height (Live III, OS 3.10.1), so sizes are set high
            label("now value", "Value", "40 74 400 120", 110, WHITE, "SemiBold", param_handle=handle["Now"]),
            label("step heading", "Name", "40 200 380 34", 26, MUTED, param_handle=handle["Step"], case="Upper Case"),
            label("step value", "Value", "40 232 400 64", 44, WHITE, param_handle=handle["Step"]),
            label("next heading", "Name", "450 40 810 34", 26, ACCENT, "SemiBold", "horizontallyCentred verticallyCentred", param_handle=handle["Next"], case="Upper Case"),
            label("next value", "Value", "450 70 810 230", 200, ACCENT, "Bold", "horizontallyCentred verticallyCentred",
                  param_handle=handle["Next"]),   # about 130 px on screen: "A#maj9#11/C" still fits
        ]
        json.dump(tui, open(tui_path, "w"), indent=4)
        out = os.path.join(PROJECT, "skins")
        if os.path.isdir(out):
            shutil.rmtree(out)
        shutil.copytree(tmp, out)
        print("wrote", out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
