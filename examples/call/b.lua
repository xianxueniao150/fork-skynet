local skynet = require "skynet"

function onMsg(data)
    skynet.error("b recv " .. data)
    return "response from b"
end

skynet.start(function()
    skynet.exit()
    skynet.dispatch("lua", function(session, source, cmd, ...)
        if cmd == "onMsg" then
            local ret = onMsg(...)
            skynet.retpack(ret)
        end
    end)
end)
