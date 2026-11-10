-- Example script. Every .lua file in a mod's Scripts folder runs when the game starts.
-- The full list of functions is in Mods\SCRIPTING.txt; print() writes to System\SCCTRemaster.log.

print("Example mod loaded")

-- Runs every time a level is ready (mission start, savegame load, script reload).
on("level", function(map)
    local sam = player()
    print("Level " .. map .. " is ready. Sam has " .. tostring(sam.Health) .. " health.")

    -- Count the characters in the level.
    local count = 0
    for _, pawn in ipairs(actors("EPawn")) do
        if pawn ~= sam then count = count + 1 end
    end
    print(count .. " other characters in this level")
end)

-- Runs every frame while playing. Press Num1 to heal Sam.
on("tick", function(dt)
    if pressed("Num1") then
        local sam = player()
        sam.Health = sam.InitialHealth
        print("Healed Sam to " .. sam.Health)
    end
end)
