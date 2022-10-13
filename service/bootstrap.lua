local skynet = require "skynet"
local harbor = require "skynet.harbor"
local service = require "skynet.service"
require "skynet.manager" -- import skynet.launch, ...

skynet.start(function()
	local standalone = skynet.getenv "standalone"

	local launcher = assert(skynet.launch("snlua", "launcher"))
	skynet.name(".launcher", launcher)

	local harbor_id = tonumber(skynet.getenv "harbor" or 0)
	if harbor_id == 0 then --如果 harbor 为 0 ，skynet 工作在单节点模式下。此时 master 和 address 以及 standalone 都不必设置。
		assert(standalone == nil)
		standalone = true
		skynet.setenv("standalone", "true")

		local ok, slave = pcall(skynet.newservice, "cdummy") --单节点模式下，是不需要通过内置的 harbor 机制做节点间通讯的。但为了兼容（因为你还是有可能注册全局名字），需要启动一个叫做 cdummy 的服务，它负责拦截对外广播的全局名字变更。

		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)

	else
		if standalone then
			if not pcall(skynet.newservice, "cmaster") then
				skynet.abort()
			end
		end

		local ok, slave = pcall(skynet.newservice, "cslave")
		if not ok then
			skynet.abort()
		end
		skynet.name(".cslave", slave)
	end

	if standalone then
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end
	skynet.newservice "service_mgr"

	local enablessl = skynet.getenv "enablessl"
	if enablessl then
		service.new("ltls_holder", function()
			local c = require "ltls.init.c"
			c.constructor()
		end)
	end

	pcall(skynet.newservice, skynet.getenv "start" or "main")
	skynet.exit()
end)
