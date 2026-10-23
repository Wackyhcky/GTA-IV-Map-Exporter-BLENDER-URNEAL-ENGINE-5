"""Runs inside Unreal Editor: imports the GTA IV front end written by gtaiv_exporter.frontend
(textures + frontend.json) into /Game/GTAIV/Frontend and builds the GTAIV_Startup level.

Job (GTA4_UE_JOB env var, JSON): {"frontend_dir": ..., "start_level": "/Game/..."}
"""

import json
import os
import traceback

import unreal

ROOT = "/Game/GTAIV/Frontend"
TEX = ROOT + "/Textures"
AUDIO = ROOT + "/Audio"
DATA = ROOT + "/DA_GTAIV_Frontend"
LEVEL = ROOT + "/GTAIV_Startup"
EAL = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()


def say(msg):
    unreal.log("@@" + msg)


def import_textures(src_dir, textures):
    tasks = []
    for t in textures:
        t_ = unreal.AssetImportTask()
        t_.filename = os.path.join(src_dir, "textures", t["asset"] + ".png")
        t_.destination_path = TEX
        t_.automated = True
        t_.replace_existing = True
        t_.save = False
        tasks.append(t_)
    AT.import_asset_tasks(tasks)
    out = {}
    for t in textures:
        tex = unreal.load_asset("%s/%s" % (TEX, t["asset"]))
        if tex is None:
            continue
        # drawn 1:1 on screen: no mips, UI compression, never streamed out
        tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
        tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        tex.set_editor_property("never_stream", True)
        out[t["name"]] = tex
    EAL.save_directory(TEX, only_if_is_dirty=False)
    say("LOG Imported %d front-end textures" % len(out))
    return out


def import_music(src_dir, music):
    """WAVs from loading.rpf -> SoundWaves; the loading and menu music loop."""
    tasks = []
    for name in set(music.values()):
        t_ = unreal.AssetImportTask()
        t_.filename = os.path.join(src_dir, "audio", name + ".wav")
        t_.destination_path = AUDIO
        t_.destination_name = "S_FE_" + name
        t_.automated = True
        t_.replace_existing = True
        t_.save = False
        tasks.append(t_)
    if tasks:
        AT.import_asset_tasks(tasks)
    out = {}
    for role, name in music.items():
        snd = unreal.load_asset("%s/S_FE_%s" % (AUDIO, name))
        if snd is None:
            continue
        snd.set_editor_property("looping", role in ("loading", "menu"))
        snd.set_editor_property("virtualization_mode", unreal.VirtualizationMode.PLAY_WHEN_SILENT)
        out[role] = snd
    if out:
        EAL.save_directory(AUDIO, only_if_is_dirty=False)
        say("LOG Imported %d music tracks" % len(out))
    return out


def option(o, displays):
    return unreal.GTAIVFeOption(label=o["label"], action=o["action"], value=o["value"], scaler=o["scaler"],
                                display=o["display"], display_values=displays.get(o["display"], []))


def title_bgs(tex):
    m = unreal.Map(str, unreal.Texture2D)
    for k, t in tex.items():
        if k.startswith("title_") and t:
            m[k[len("title_"):]] = t
    return m


def build_data(data, tex, start_level, music=None):
    if EAL.does_asset_exist(DATA):
        da = unreal.load_asset(DATA)
    else:
        f = unreal.DataAssetFactory()
        f.set_editor_property("data_asset_class", unreal.GTAIVFrontendData)
        da = AT.create_asset("DA_GTAIV_Frontend", ROOT, unreal.GTAIVFrontendData, f)
    screens = []
    for s in data["screens"]:
        objs = []
        for o in s["objects"]:
            objs.append(unreal.GTAIVFeObject(
                texture=tex.get(o["texture"]), start_time=o.get("start", 0), end_time=o.get("end", s["duration"]),
                move_time=o.get("move_time", 20), scale_time=o.get("scale_time", 20),
                pos=unreal.Vector2D(*o.get("pos", [0.5, 0.5])), move=unreal.Vector2D(*o.get("move", [0, 0])),
                size=unreal.Vector2D(*o.get("size", [1, 1])), scale=unreal.Vector2D(*o.get("scale", [0, 0]))))
        screens.append(unreal.GTAIVFeScreen(flag=s["flag"], duration=s["duration"], fade=s["fade"], objects=objs))
    displays = data["displays"]
    menus = [unreal.GTAIVFeMenu(id=k, header=m["header"], options=[option(o, displays) for o in m["options"]])
             for k, m in data["menus"].items()]
    title = [unreal.GTAIVFeOption(label=i["label"], action=i["action"], value=i["value"])
             for i in data["title"]["items"]]
    layout = unreal.Map(str, unreal.Vector2D)
    for k, v in data["layout"].items():
        layout[k] = unreal.Vector2D(v[0], v[1])
    f = data["font"]
    da.set_editor_properties({
        "screens": screens, "legal_text1": data["strings"]["LEGAL_PC"], "legal_text2": data["strings"]["LEGAL_PC_2"],
        "title_header": data["title"]["header"], "title_items": title, "menus": menus, "layout": layout,
        "cursor": tex.get("mousecursor"), "start_level": start_level,
        "font_texture": tex.get("font1"), "font_map": f["map"], "font_prop": f["prop"],
        "font_main_end": f["main"][1], "font_common_start": f["common"][0], "font_whitespace": f["whitespace"],
        "title_backgrounds": title_bgs(tex), "title_bar": data.get("title_bar", ["Start", "Options", "Social Club", "Back"]),
        "font3_texture": tex.get("font3"), "font3_map": data.get("font3", {}).get("map", []),
        "font3_prop": data.get("font3", {}).get("prop", []),
        "font3_start": data.get("font3", {}).get("sub2", [92, 164])[0], "font3_end": data.get("font3", {}).get("sub2", [92, 164])[1],
        "loading_text": data.get("loading_text", "Loading -"), "wasted_text": data.get("wasted_text", "Wasted"),
        "music": (music or {}).get("loading"), "start_music": (music or {}).get("start"),
        "menu_music": (music or {}).get("menu"),
    })
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    say("LOG Front-end data: %d screens, %d menus" % (len(screens), len(menus)))
    return da


def build_level():
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if EAL.does_asset_exist(LEVEL):
        les.load_level(LEVEL)
    else:
        les.new_level(LEVEL)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.GTAIVFrontendGameMode.static_class())
    les.save_current_level()
    say("LOG Built level %s (game mode GTAIVFrontendGameMode)" % LEVEL)


def main():
    job = json.load(open(os.environ["GTA4_UE_JOB"], encoding="utf-8"))
    src = job["frontend_dir"]
    data = json.load(open(os.path.join(src, "frontend.json"), encoding="utf-8"))
    tex = import_textures(src, data["textures"])
    music = import_music(src, data.get("music") or {})
    build_data(data, tex, job.get("start_level", ""), music)
    build_level()
    say("DONE 1")


try:
    main()
except Exception as e:
    unreal.log_error(traceback.format_exc())
    say("FAILED %s" % e)
