# Luau (Roblox fork, build 0.747)

Vendored Luau for the core. This is the Luau VM/compiler the 0.747 client uses
(the `RobloxExtraSpace` layout and `luaL_sandboxthread` are part of it), so
`luau_load`/`lua_pcall` from this copy can operate on the client's `lua_State`.

Do not upgrade independently of the client build: `lua_State`, `Closure`, and
`Proto` layouts must match the running client.

License: MIT, Copyright (c) 2019 Roblox Corporation and 1994-2008 Lua.org,
PUC-Rio. See `LICENSE.txt`.
