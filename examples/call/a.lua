local skynet = require "skynet"

skynet.start(function()
	skynet.error("Server start")
	b = skynet.newservice("b")
	skynet.error("b", b)
	resp = skynet.call(b, "lua", "onMsg", "msg from a")
	skynet.error("resp", resp)
end)
