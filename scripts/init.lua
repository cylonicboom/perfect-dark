-- Entry point. The engine runs scripts/init.lua automatically on startup, on
-- each stage load, and whenever you type `/lua reload` in the ~ console.
--
-- Keep your own scripting here, or require/dofile other files like the showcase.

local ok, err = pcall(dofile, "scripts/showcase.lua")
if not ok then
  pd.log("init.lua: failed to load showcase: " .. tostring(err))
end
