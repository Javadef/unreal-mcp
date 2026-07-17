import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import * as net from "node:net";

// ─────────────────────────────────────────────
// TCP connection to the UE5 editor plugin
// ─────────────────────────────────────────────

// 127.0.0.1 explicitly, not "localhost": the UE plugin binds IPv4 loopback
// only, and "localhost" can resolve to ::1 first depending on the resolver.
const UE_HOST = process.env.UEOC_HOST || "127.0.0.1";
const UE_PORT = parseInt(process.env.UEOC_PORT || "3099", 10);

let socket: net.Socket | null = null;
let responseResolvers = new Map<string, (data: unknown) => void>();
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

function connect(): Promise<net.Socket> {
  return new Promise((resolve, reject) => {
    if (socket && !socket.destroyed) return resolve(socket);

    socket = new net.Socket();
    socket.setEncoding("utf-8");

    let buffer = "";
    let settled = false;

    socket.connect(UE_PORT, UE_HOST, () => {
      console.error(`[UE Bridge] Connected to UE editor on ${UE_HOST}:${UE_PORT}`);
      settled = true;
      resolve(socket!);
    });

    socket.on("data", (chunk: string) => {
      buffer += chunk;
      const lines = buffer.split("\n");
      buffer = lines.pop() || "";

      for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed) continue;
        try {
          const msg = JSON.parse(trimmed);
          if (msg.id && responseResolvers.has(msg.id)) {
            responseResolvers.get(msg.id)!(msg);
            responseResolvers.delete(msg.id);
          }
        } catch {
          console.error("[UE Bridge] Failed to parse response:", trimmed);
        }
      }
    });

    socket.on("error", (err) => {
      console.error("[UE Bridge] Socket error:", err.message);
      socket = null;
      if (!settled) {
        settled = true;
        reject(err);
      }
    });

    socket.on("close", () => {
      console.error("[UE Bridge] Connection closed. Will reconnect...");
      socket = null;
      if (!settled) {
        settled = true;
        reject(new Error("Connection closed before connect"));
      }
      scheduleReconnect();
    });
  });
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect().catch(() => scheduleReconnect());
  }, 2000);
}

function sendToUE(request: Record<string, unknown>): Promise<unknown> {
  return new Promise((resolve, reject) => {
    connect().then((sock) => {
      const id = crypto.randomUUID();
      const msg = JSON.stringify({ id, ...request }) + "\n";

      responseResolvers.set(id, resolve);

      const timeout = setTimeout(() => {
        responseResolvers.delete(id);
        reject(new Error(`Timeout waiting for UE response: ${request.tool}`));
      }, 15000);

      const origResolver = responseResolvers.get(id)!;
      responseResolvers.set(id, (data: unknown) => {
        clearTimeout(timeout);
        origResolver(data);
      });

      sock.write(msg);
    }).catch(reject);
  });
}

// ─────────────────────────────────────────────
// MCP Server
// ─────────────────────────────────────────────

const server = new McpServer({
  name: "opencode-ue-bridge",
  version: "1.0.0",
});

// ─────────────────────────────────────────────
// Tool definitions
// ─────────────────────────────────────────────

server.tool("ping",
  "Check if the UE editor is connected and responsive.",
  {},
  async () => {
    const result = await sendToUE({ tool: "ping", args: {} });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_project_structure",
  "Get UE project structure: modules, plugins, content directories, config files.",
  {},
  async () => {
    const result = await sendToUE({ tool: "get_project_structure", args: {} });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_scene_hierarchy",
  "Get all actors in the current editor world with their hierarchy, class, and transform.",
  {},
  async () => {
    const result = await sendToUE({ tool: "get_scene_hierarchy", args: {} });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("execute_console_command",
  "Execute a UE console command in the editor.",
  { command: z.string().describe("Console command to execute") },
  async ({ command }) => {
    const result = await sendToUE({ tool: "execute_console_command", args: { command } });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("search_assets",
  "Search UE project assets by name, type, or path.",
  {
    query: z.string().optional().describe("Search query to match against asset names"),
    assetType: z.string().optional().describe("Filter by asset type (e.g. StaticMesh, Material, Texture2D)"),
    pathPrefix: z.string().optional().describe("Filter by content path prefix (e.g. /Game/Environment)"),
    limit: z.number().optional().describe("Max results (default 50)"),
    offset: z.number().optional().describe("Pagination offset (default 0)"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "search_assets", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_asset_details",
  "Get detailed metadata for a single UE asset.",
  { assetPath: z.string().describe("Full asset path (e.g. /Game/Meshes/SM_Chair)") },
  async ({ assetPath }) => {
    const result = await sendToUE({ tool: "get_asset_details", args: { assetPath } });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_class_details",
  "Get detailed reflection info for a C++ class: properties, functions, and metadata.",
  { className: z.string().describe("C++ class name") },
  async ({ className }) => {
    const result = await sendToUE({ tool: "get_class_details", args: { className } });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_module_dependencies",
  "Get C++ module dependencies from Build.cs files.",
  { moduleName: z.string().optional().describe("Filter by module name") },
  async ({ moduleName }) => {
    const result = await sendToUE({ tool: "get_module_dependencies", args: { moduleName } });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_plugin_list",
  "List all plugins in the project with metadata.",
  {},
  async () => {
    const result = await sendToUE({ tool: "get_plugin_list", args: {} });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_output_log",
  "Get recent output log messages with optional category filter.",
  {
    limit: z.number().optional().describe("Max entries (default 100)"),
    category: z.string().optional().describe("Filter by log category"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "get_output_log", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_build_logs",
  "Get recent build/compilation log entries from the editor.",
  {
    limit: z.number().optional().describe("Max entries (default 50)"),
    category: z.string().optional().describe("Filter by log category"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "get_build_logs", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_compilation_status",
  "Check if the editor is currently compiling/hot-reloading.",
  {},
  async () => {
    const result = await sendToUE({ tool: "get_compilation_status", args: {} });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_blueprint_list",
  "List all Blueprint assets in the project.",
  {
    limit: z.number().optional().describe("Max results (default 100)"),
    pathPrefix: z.string().optional().describe("Filter by content path prefix"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "get_blueprint_list", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_selected_actors",
  "Get all currently selected actors in the editor.",
  {},
  async () => {
    const result = await sendToUE({ tool: "get_selected_actors", args: {} });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_actor_details",
  "Get detailed properties of a specific actor including components and tags.",
  { name: z.string().describe("Actor name or label") },
  async ({ name }) => {
    const result = await sendToUE({ tool: "get_actor_details", args: { name } });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("set_actor_property",
  "Set a property value on an actor or its component.",
  {
    actorName: z.string().describe("Actor name"),
    propertyName: z.string().describe("Property name to set"),
    value: z.string().describe("New value as string"),
    componentName: z.string().optional().describe("Optional component name"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "set_actor_property", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("generate_code",
  "Generate C++ or Blueprint code and write to a project source file.",
  {
    filePath: z.string().describe("Relative file path in project Source/"),
    content: z.string().describe("Code content to write"),
    description: z.string().optional().describe("Description of the generated code"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "generate_code", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("search_classes",
  "Search C++ classes by name pattern.",
  {
    query: z.string().describe("Class name search pattern"),
    limit: z.number().optional().describe("Max results (default 50)"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "search_classes", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_cpp_hierarchy",
  "Get the C++ class hierarchy for a given base class.",
  {
    baseClass: z.string().describe("Base class name (e.g. AActor, UObject)"),
    maxDepth: z.number().optional().describe("Max depth (default 5)"),
  },
  async (args) => {
    const result = await sendToUE({ tool: "get_cpp_hierarchy", args });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

server.tool("get_material_graph",
  "Dump the full material expression node graph for a Material or MaterialFunction. Shows every expression node with its connections, parameter values, constants, and function calls.",
  { assetPath: z.string().describe("Full asset path to Material or MaterialFunction") },
  async ({ assetPath }) => {
    const result = await sendToUE({ tool: "get_material_graph", args: { assetPath } });
    return { content: [{ type: "text", text: JSON.stringify(result, null, 2) }] };
  },
);

// ─────────────────────────────────────────────
// Start
// ─────────────────────────────────────────────

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("[UE Bridge] MCP server started. Waiting for UE editor connection...");

  // Pre-connect to UE
  connect().catch(() => {
    console.error("[UE Bridge] UE editor not available yet. Will connect when ready.");
  });
}

main().catch((err) => {
  console.error("[UE Bridge] Fatal error:", err);
  process.exit(1);
});
