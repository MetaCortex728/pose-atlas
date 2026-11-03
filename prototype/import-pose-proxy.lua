-- Aseprite import/export bridge. Preserves fixed cells and an editable correction layer.
local root = assert(app.params["root"], "Missing --script-param root=...")
local function path(name) return app.fs.joinPath(root, name) end
local file = assert(io.open(path("pose-contract.json"), "r"))
local contract = json.decode(file:read("*a"))
file:close()
local normals = app.params["normals"] == "true"
local outputName = normals and "normal-proxy.aseprite" or "pose-proxy.aseprite"
assert((contract.schema == 1 and #contract.frames == 5)
  or (contract.schema == 2 and contract.view_count == 8 and #contract.frames == 40)
  or (contract.schema == 3 and contract.view_count == 8 and #contract.frames == 72)
  or (contract.schema == 4 and contract.view_count == 8 and #contract.frames == 104)
  or (contract.schema == 5 and contract.view_count == 8 and #contract.frames == 168)
  or (contract.schema == 6 and contract.view_count == 8 and #contract.frames == 200)
  or (contract.schema == 7 and contract.view_count == 8 and #contract.frames == 456), "Unexpected proxy contract")
assert(not app.fs.isFile(path(outputName)), "Existing editable source will not be overwritten")
local sprite = Sprite(contract.cell[1], contract.cell[2], ColorMode.RGB)
sprite.layers[1].name = normals and "Linear camera normals - no palette or gamma conversion" or "Blender base - unmodified"
for index, item in ipairs(contract.frames) do
  local frame = index == 1 and sprite.frames[1] or sprite:newEmptyFrame()
  frame.duration = item.duration_ms / 1000.0
  local image = Image{fromFile=path((normals and "normals/" or "") .. item.file)}
  assert(image.width == sprite.width and image.height == sprite.height, "Cell size mismatch")
  sprite:newCel(sprite.layers[1], frame, image, Point(0, 0))
end
-- Adding a frame can extend existing tags. Define them only after all frames exist.
for index, item in ipairs(contract.frames) do
  local tag = sprite:newTag(sprite.frames[index], sprite.frames[index])
  tag.name = item.name
end
local corrections = sprite:newLayer()
corrections.name = "Manual corrections - empty, not yet hand-polished"
local slice = sprite:newSlice(Rectangle(0, 0, sprite.width, sprite.height))
slice.name = "foot"
slice.pivot = Point(contract.pivot[1], contract.pivot[2])
sprite.data = "Technical proxy; visual durations do not define combat events."
sprite:saveAs(path(outputName))
print("POSE_ASEPRITE_IMPORT_PASS " .. root)
sprite:close()
