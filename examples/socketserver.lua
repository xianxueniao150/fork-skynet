local skynet = require "skynet"
local socket = require "skynet.socket"

--简单echo服务
function echo(cID, addr)
    socket.start(cID)
    while true do
        local str = socket.read(cID)
        if str then
            skynet.error("recv " .. str)
            socket.write(cID, string.upper(str))
        else
            socket.close(cID)
            skynet.error(addr .. " disconnect")
            return
        end
    end
end

function accept(cID, addr)
    skynet.error(addr .. " accepted")
    skynet.fork(echo, cID, addr) --来一个链接，就开一个新的协程来处理客户端数据
end

--服务入口
skynet.start(function()
    local addr = "0.0.0.0:8001"
    skynet.error("listen " .. addr)
    local lID = socket.listen(addr)
    assert(lID)
    socket.start(lID, accept)
end)
