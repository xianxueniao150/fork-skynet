local args = {}
for word in string.gmatch(..., "%S+") do
	table.insert(args, word)
end

SERVICE_NAME = args[1] --设置全局变量SERVICE_NAME，因此在Lua层可以用SERVICE_NAME获取当前服务的名称

local main, pattern

local err = {}
--获取需启动的服务的Lua脚本(比如simpledb.lua)
for pat in string.gmatch(LUA_SERVICE, "([^;]+);*") do
	local filename = string.gsub(pat, "?", SERVICE_NAME)
	local f, msg = loadfile(filename)
	if not f then
		table.insert(err, msg)
	else
		pattern = pat
		main = f
		break
	end
end

if not main then
	error(table.concat(err, "\n"))
end

package.path, LUA_PATH = LUA_PATH --赋值完之后LUA_PATH会置为nil
package.cpath, LUA_CPATH = LUA_CPATH

local service_path = string.match(pattern, "(.*/)[^/?]+$")

if service_path then
	service_path = string.gsub(service_path, "?", args[1])
	package.path = service_path .. "?.lua;" .. package.path
	SERVICE_PATH = service_path
else
	local p = string.match(pattern, "(.*/).+$")
	SERVICE_PATH = p
end

--如果skynet启动配置里设置了LUA_PRELOAD，加载并运行它
if LUA_PRELOAD then
	local f = assert(loadfile(LUA_PRELOAD))
	f(table.unpack(args))
	LUA_PRELOAD = nil
end

_G.require = (require "skynet.require").require
main(select(2, table.unpack(args))) --运行对应的lua脚本，除第一个参数以外的所有参数（第一个参数是服务的名称）
