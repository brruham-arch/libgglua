gg.log("main.lua dimuat")
local b = gg.base("libgame.so")
gg.log("libgame base = " .. (b and string.format("0x%x", b) or "nil"))

function onComment(e) gg.log("komen " .. tostring(e.username) .. ": " .. tostring(e.comment)) end
function onGift(e) gg.log("gift " .. tostring(e.username) .. " " .. tostring(e.giftName) .. " x" .. tostring(e.repeatCount)) end
function onLike(e) gg.log("like " .. tostring(e.username) .. " +" .. tostring(e.value)) end
function onJoin(e) gg.log("join " .. tostring(e.username)) end
