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
- `get_material_graph` - Dump material node graph
