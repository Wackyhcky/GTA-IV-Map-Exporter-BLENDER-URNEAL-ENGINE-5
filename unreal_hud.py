"""Runs inside Unreal Editor: imports the GTA IV HUD written by gtaiv_exporter.hud (textures +
hud.json) into /Game/GTAIV/HUD, builds the radar materials and DA_GTAIV_Hud, and makes the
GM_GTAIV game mode use the GTA IV HUD.

Job (GTA4_UE_JOB env var, JSON): {"hud_dir": ...}
"""

import json
import os
import traceback

import unreal

ROOT = "/Game/GTAIV/HUD"
TEX = ROOT + "/Textures"
DATA = ROOT + "/DA_GTAIV_Hud"
FRONTEND_DATA = "/Game/GTAIV/Frontend/DA_GTAIV_Frontend"
GAME_MODE = "/Game/GTAIV/Characters/GM_GTAIV"
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()

RADAR_HLSL = r"""
float2 p = UV - 0.5;
float2 w = Basis.zw * p.x - Basis.xy * p.y;
float2 uv = Center.xy + float2(w.x * Scale.x, w.y * Scale.y);
float3 water = float3(0.124, 0.205, 0.296);
float3 c = water;
if (uv.x >= 0 && uv.y >= 0 && uv.x <= 1 && uv.y <= 1)
    c = Texture2DSample(MapTex, MapTexSampler, uv).rgb;
float r = length(p) * 2;
return float4(c, 1 - smoothstep(0.985, 1.0, r));
"""

RING_HLSL = r"""
float2 p = UV - 0.5;
float r = length(p) * 2;
float band = smoothstep(0.880, 0.890, r) * (1 - smoothstep(0.962, 0.972, r));
float t = acos(clamp(p.y / max(length(p), 1e-5), -1, 1)) / 3.14159265;
float left = p.x < 0 ? 1 : 0;
float h = left * (t <= Health ? 1 : 0);
float a = (1 - left) * (t <= Armour ? 1 : 0);
float3 c = h > 0 ? HealthColour.rgb : ArmourColour.rgb;
return float4(c, band * max(h, a));
"""


def say(msg):
    unreal.log("@@" + msg)


def import_textures(src_dir, textures):
    tasks = []
    for t in textures:
        task = unreal.AssetImportTask()
        task.filename = os.path.join(src_dir, "textures", t["asset"] + ".png")
        task.destination_path = TEX
        task.automated = True
        task.replace_existing = True
        task.save = False
        tasks.append(task)
    AT.import_asset_tasks(tasks)
    out = {}
    for t in textures:
        tex = unreal.load_asset("%s/%s" % (TEX, t["asset"]))
        if tex is None:
            continue
        if t["name"] == "radar_map":
            tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)  # keeps mips: radar zooms out
            tex.set_editor_property("never_stream", True)
        else:
            tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
            tex.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
            tex.set_editor_property("never_stream", True)
        out[t["name"]] = tex
    EAL.save_directory(TEX, only_if_is_dirty=False)
    say("LOG Imported %d HUD textures" % len(out))
    return out


def ui_material(name, code, scalars=(), vectors=(), texture=None):
    path = "%s/%s" % (ROOT, name)
    if EAL.does_asset_exist(path):
        EAL.delete_asset(path)
    mat = AT.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_UI)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    custom = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("code", code)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    names = ["UV"] + list(scalars) + list(vectors) + ([texture[0]] if texture else [])
    inputs = []
    for n in names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    uv = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -700, -200)
    MEL.connect_material_expressions(uv, "", custom, "UV")
    y = 0
    for s in scalars:
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, y)
        e.set_editor_property("parameter_name", s)
        MEL.connect_material_expressions(e, "", custom, s)
        y += 100
    for v in vectors:
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -700, y)
        e.set_editor_property("parameter_name", v)
        MEL.connect_material_expressions(e, "RGBA", custom, v)  # all four components
        y += 150
    if texture:
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, -700, y)
        e.set_editor_property("parameter_name", texture[0])
        if texture[1]:
            e.set_editor_property("texture", texture[1])
        MEL.connect_material_expressions(e, "", custom, texture[0])
    rgb = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -100, -50)
    rgb.set_editor_properties({"r": True, "g": True, "b": True, "a": False})
    alpha = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -100, 80)
    alpha.set_editor_properties({"r": False, "g": False, "b": False, "a": True})
    MEL.connect_material_expressions(custom, "", rgb, "")
    MEL.connect_material_expressions(custom, "", alpha, "")
    MEL.connect_material_property(rgb, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    MEL.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
    MEL.recompile_material(mat)
    EAL.save_loaded_asset(mat)
    return mat


def build_data(data, tex, radar_mat, ring_mat):
    if EAL.does_asset_exist(DATA):
        da = unreal.load_asset(DATA)
    else:
        f = unreal.DataAssetFactory()
        f.set_editor_property("data_asset_class", unreal.GTAIVHudData)
        da = AT.create_asset("DA_GTAIV_Hud", ROOT, unreal.GTAIVHudData, f)
    colours = unreal.Map(str, unreal.LinearColor)
    for k, (r, g, b) in data["colours"].items():
        colours[k] = unreal.LinearColor(r / 255.0, g / 255.0, b / 255.0, 1.0)
    layout = unreal.Map(str, unreal.GTAIVHudElement)
    for k, e in data["layout"].items():
        c = data["colours"].get(e["colour"], [255, 255, 255])
        layout[k] = unreal.GTAIVHudElement(pos=unreal.Vector2D(*e["pos"][:2]), size=unreal.Vector2D(*e["size"][:2]),
                                           colour=unreal.LinearColor(c[0] / 255.0, c[1] / 255.0, c[2] / 255.0,
                                                                     e["alpha"] / 255.0))
    textures = unreal.Map(str, unreal.Texture2D)
    for k, t in tex.items():
        if k != "radar_map":
            textures[k] = t
    zones = [unreal.GTAIVZone(name=z["name"], min=unreal.Vector2D(*z["min"]), max=unreal.Vector2D(*z["max"]))
             for z in data["zones"]]
    r = data["radar"]
    da.set_editor_properties({
        "layout": layout, "colours": colours, "zones": zones, "textures": textures,
        "radar_map": tex.get("radar_map"), "radar_cols": r["cols"], "radar_rows": r["rows"], "radar_tile": r["tile"],
        "radar_origin": unreal.Vector2D(*r["origin"]), "radar_material": radar_mat, "ring_material": ring_mat,
        "font": unreal.load_asset(FRONTEND_DATA) if EAL.does_asset_exist(FRONTEND_DATA) else None,
    })
    EAL.save_loaded_asset(da, only_if_is_dirty=False)
    say("LOG HUD data: %d layout entries, %d zones, %d textures" % (len(data["layout"]), len(zones), len(textures)))


def hook_game_mode():
    """GM_GTAIV (GASP game mode with GTA IV peds) shows the GTA IV HUD; the front end's
    Start New Game opens its level with that game mode."""
    if EAL.does_asset_exist(GAME_MODE):
        bp = unreal.load_asset(GAME_MODE)
        cdo = unreal.get_default_object(bp.generated_class())
        cdo.set_editor_property("hud_class", unreal.GTAIVPlayerHUD.static_class())
        EAL.save_loaded_asset(bp, only_if_is_dirty=False)
        say("LOG GM_GTAIV now uses the GTA IV HUD")
        if EAL.does_asset_exist(FRONTEND_DATA):
            fe = unreal.load_asset(FRONTEND_DATA)
            level = fe.get_editor_property("start_level").split("?")[0]
            if level:
                fe.set_editor_property("start_level", "%s?game=%s.GM_GTAIV_C" % (level, GAME_MODE))
                EAL.save_loaded_asset(fe, only_if_is_dirty=False)
                say("LOG Start New Game: %s with GM_GTAIV" % level)
    else:
        say("LOG GM_GTAIV not found (export the characters first); use GTAIVPlayerHUD as your HUD class")


def main():
    job = json.load(open(os.environ["GTA4_UE_JOB"], encoding="utf-8"))
    src = job["hud_dir"]
    data = json.load(open(os.path.join(src, "hud.json"), encoding="utf-8"))
    tex = import_textures(src, data["textures"])
    radar = ui_material("M_GTAIV_Radar", RADAR_HLSL, vectors=("Center", "Basis", "Scale"),
                        texture=("MapTex", tex.get("radar_map")))
    ring = ui_material("M_GTAIV_RadarRing", RING_HLSL, scalars=("Health", "Armour"),
                       vectors=("HealthColour", "ArmourColour"))
    build_data(data, tex, radar, ring)
    hook_game_mode()
    say("DONE 1")


try:
    main()
except Exception as e:
    unreal.log_error(traceback.format_exc())
    say("FAILED %s" % e)
