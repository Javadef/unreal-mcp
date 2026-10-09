# OpenCode Unreal Engine MCP Bridge

MCP server bridging [OpenCode](https://opencode.ai) to Unreal Engine 5 editor via a local TCP connection.

## How it works

- **UE Editor Plugin (C++)**: Runs a TCP server on port 3099. Handles tool requests for project structure, C++ classes, Blueprints, scene actors, assets, build logs, and code generation.
- **MCP Server (TypeScript)**: Bridges OpenCode's MCP protocol over stdio to the UE plugin's TCP socket.

## Setup

1. Enable the OpenCodeBridge plugin in Edit > Plugins.
2. Run `npm install` then `npm run build`.
3. Launch OpenCode from the project root. The MCP server starts automatically.

## Available Tools

- `ping` - Check editor connection
- `get_project_structure` - Project modules, plugins, content dirs
- `get_scene_hierarchy` - All actors in the level
- `execute_console_command` - Run console commands
- `search_assets` - Search the asset registry
- `get_asset_details` - Get metadata for an asset
- `get_class_details` - Get C++ class reflection info
- `get_module_dependencies` - Parse Build.cs dependencies
- `get_plugin_list` - List enabled plugins
- `get_output_log` - Get recent log messages
- `get_build_logs` - Get build log entries
- `get_compilation_status` - Check hot-reload state
- `get_blueprint_list` - List Blueprint assets
- `get_selected_actors` - Get editor selection
- `get_actor_details` - Get actor properties
- `set_actor_property` - Set a property on an actor
- `generate_code` - Write code to project source
- `search_classes` - Search C++ classes
- `get_cpp_hierarchy` - Get class hierarchy
- `get_material_graph` - Material / MaterialFunction node graph: every node's non-default properties and named input pins
- `get_material_parameters` - Effective parameter values of a Material or MaterialInstance
- `get_material_hlsl` - The HLSL Unreal generates for a material, static switches resolved
- `get_static_mesh_data` - LOD counts and screen sizes; optionally a LOD's vertices, UV channels and indices
- `get_texture_info` - Runtime format, size and alpha of a texture, its settings, per-channel source statistics
- `get_level_lighting` - Lights, sky, fog, post-process overrides and renderer cvars of the open level
- `export_asset_text` - Any asset as T3D text
- `live_compile` - Run a Live Coding compile, wait, return the result and the compiler's errors
- `run_python` - Execute Python in the editor

Every tool also takes `outputFile`: the result is written to disk (relative paths go to
`<Project>/Saved/OpenCodeBridge`) and the reply is just the path and size.

## Layout

- `src/` → `dist/index.js` is the only copy of the MCP server. Point every client at it.
- `UEPlugin/` is a directory junction to the live plugin inside the UE project
  (`<Project>/Plugins/OpenCodeBridge`), so there is one copy of the plugin source too: editing
  it here edits what the editor compiles. On a fresh clone it is an ordinary folder — copy it
  into a project's `Plugins/`.
- Adding a tool: write the handler in `OpenCodeToolHandler.cpp`, register it in `RegisterTools()`,
  add one `tool(...)` entry in `src/index.ts`, then `live_compile` and `npm run build`.

## Editor tab

The toolbar's **OpenCode** button opens a dashboard: bridge status and request statistics, the
open level's lighting at a glance, recent calls, and buttons that run the tools above into
`Saved/OpenCodeBridge` (compile, dump lighting / scene, material graph, HLSL, mesh, texture, T3D).
