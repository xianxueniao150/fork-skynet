local skynet = require "skynet"

local coin = 0

function onMsg(data)
    skynet.error("agent recv " .. data .. "eeeee")
    --消息处理
    if data == "work\r\n" then
        coin = coin + 2
        return coin .. "\r\n"
    end

    return "err cmd\r\n"
end

skynet.start(function()
    skynet.dispatch("lua", function(session, source, cmd, ...)
        if cmd == "onMsg" then
            local ret = onMsg(...)
            skynet.retpack(ret)
        end
    end)
end)
