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
      // Tell the editor which copy of this server is talking to it; its
      // dashboard shows the path and hands it out as the MCP config.
      socket!.write(JSON.stringify({
        id: crypto.randomUUID(), tool: "hello", args: { server: process.argv[1] ?? "" },
      }) + "\n");
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

function sendToUE(request: Record<string, unknown>, timeoutMs = 15000): Promise<unknown> {
  return new Promise((resolve, reject) => {
    connect().then((sock) => {
      const id = crypto.randomUUID();
      const msg = JSON.stringify({ id, ...request }) + "\n";

      responseResolvers.set(id, resolve);

      const timeout = setTimeout(() => {
        responseResolvers.delete(id);
        reject(new Error(`Timeout waiting for UE response: ${request.tool}`));
      }, timeoutMs);

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

// Every tool forwards its arguments to the plugin tool of the same name and
// returns the reply as compact JSON — indentation only costs tokens.
//
// Every tool also takes `outputFile`: the plugin then writes the result to disk
// and replies with just the path and size, so a large result (a mesh, a
// material's HLSL, a whole level) never has to pass through the conversation.
const outputFile = z.string().optional().describe(
  "Write the result to this file instead of returning it; the reply is then just the path and size. Relative paths go to <Project>/Saved/OpenCodeBridge. Use for anything large.");

function tool(name: string, description: string, shape: z.ZodRawShape = {}, timeoutMs?: number) {
  server.tool(name, description, { ...shape, outputFile }, async (args) => {
    const result = await sendToUE({ tool: name, args }, timeoutMs);
    return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
  });
}

const assetPath = (what: string) => z.string().describe(`Full asset path to ${what}`);

// --- Editor / project ---------------------------------------------------------

tool("ping",
  "Check if the UE editor is connected and responsive. Also reports when the plugin was last compiled.");

tool("get_project_structure",
  "Get UE project structure: modules, plugins, content directories, config files.");

tool("execute_console_command",
  "Execute a UE console command in the editor.",
  { command: z.string().describe("Console command to execute") },
  120000);

tool("get_plugin_list",
  "List all plugins in the project with metadata.");

tool("get_module_dependencies",
  "Get C++ module dependencies from Build.cs files.",
  { moduleName: z.string().optional().describe("Filter by module name") });

tool("get_output_log",
  "Get recent output log messages with optional category filter.",
  {
    limit: z.number().optional().describe("Max entries (default 100)"),
    category: z.string().optional().describe("Filter by log category"),
  });

tool("get_build_logs",
  "Get recent build/compilation log entries from the editor.",
  {
    limit: z.number().optional().describe("Max entries (default 50)"),
    category: z.string().optional().describe("Filter by log category"),
  });

tool("get_compilation_status",
  "Check if the editor is currently compiling/hot-reloading.");

tool("live_compile",
  "Run a Live Coding compile in the editor and wait for it. Returns the result (Success / NoChanges / Failure / ...), the Live Coding console's log for this compile (linker errors appear there) and, when compilation failed, the compiler's own error lines — which Live Coding otherwise shows only in its console window. The editor is unresponsive while it runs.",
  {},
  600000);

tool("run_python",
  "Execute Python in the UE editor's embedded interpreter (the `unreal` module is available). The generic escape hatch: query or modify anything the editor Python API covers — assets, actors, properties, imports — without a bespoke tool. Returns captured log output (print() lands here as info entries). Requires the 'Python Editor Script Plugin' to be enabled.",
  {
    code: z.string().describe("Python source to execute. Multi-line is fine in exec mode. Use print() or unreal.log() to emit results."),
    mode: z.enum(["exec", "eval"]).optional().describe("exec (default): run statements. eval: evaluate a single expression and return its repr() as `result`."),
  },
  // Python can legitimately run long (asset scans, batch edits).
  120000);

tool("generate_code",
  "Generate C++ or Blueprint code and write to a project source file.",
  {
    filePath: z.string().describe("Relative file path in project Source/"),
    content: z.string().describe("Code content to write"),
    description: z.string().optional().describe("Description of the generated code"),
  });

// --- Level --------------------------------------------------------------------

tool("get_scene_hierarchy",
  "Get all actors in the current editor world with their hierarchy, class, and transform.");

tool("get_selected_actors",
  "Get all currently selected actors in the editor.");

tool("get_actor_details",
  "Get detailed properties of a specific actor including components and tags.",
  { name: z.string().describe("Actor name or label") });

tool("set_actor_property",
  "Set a property value on an actor or its component.",
  {
    actorName: z.string().describe("Actor name"),
    propertyName: z.string().describe("Property name to set"),
    value: z.string().describe("New value as string"),
    componentName: z.string().optional().describe("Optional component name"),
  });

tool("get_level_lighting",
  "Everything that decides how the open level is lit and graded, in one reply: directional and sky lights, height fog, sky atmosphere, local lights, each post-process volume's overridden settings, and the renderer cvars that matter (GI and reflection method, exposure, anti-aliasing, grass density).",
  {},
  30000);

// --- Assets -------------------------------------------------------------------

tool("search_assets",
  "Search UE project assets by name, type, or path.",
  {
    query: z.string().optional().describe("Search query to match against asset names"),
    assetType: z.string().optional().describe("Filter by UClass name (e.g. StaticMesh, Texture2D, MaterialInterface = materials + instances). Resolved across all modules; unresolved names fall back to class-name substring."),
    includeInstances: z.boolean().optional().describe("With assetType 'Material', also return MaterialInstances (default false)"),
    pathPrefix: z.string().optional().describe("Filter by content path prefix (e.g. /Game/Environment). If omitted, all mounted content roots except /Engine are searched."),
    limit: z.number().optional().describe("Max results (default 50)"),
    offset: z.number().optional().describe("Pagination offset (default 0)"),
  });

tool("get_asset_details",
  "Get detailed metadata for a single UE asset.",
  { assetPath: z.string().describe("Full asset path (e.g. /Game/Meshes/SM_Chair)") });

tool("export_asset_text",
  "Export any asset as T3D text: every sub-object with every non-default property. For what no dedicated tool covers (material instances, landscape grass types, data assets). Usually large — use outputFile.",
  { assetPath: assetPath("any asset") },
  60000);

tool("get_static_mesh_data",
  "Static mesh geometry. By default: bounds, material slots and, per LOD, vertex / triangle / UV-channel counts, sections and the LOD's screen size. With includeData: that LOD's render vertices (positions, every UV channel, optionally normals) and triangle indices as flat arrays — large, so pair it with outputFile.",
  {
    assetPath: assetPath("a StaticMesh"),
    lod: z.number().optional().describe("LOD whose geometry to dump with includeData (default 0)"),
    includeData: z.boolean().optional().describe("Also return the LOD's vertices and indices (default false)"),
    includeNormals: z.boolean().optional().describe("With includeData, also return vertex normals (default false)"),
  },
  60000);

tool("get_texture_info",
  "What the GPU actually samples for a texture — runtime size, mip count, pixel format, whether alpha survives — plus the import settings that decide it and per-channel min/max/mean of the source image. Look here before assuming a channel is constant or a format is compressed.",
  {
    assetPath: assetPath("a Texture"),
    channelStats: z.boolean().optional().describe("Compute per-channel min/max/mean of the source image (default true)"),
  },
  60000);

tool("get_blueprint_list",
  "List all Blueprint assets in the project.",
  {
    limit: z.number().optional().describe("Max results (default 100)"),
    pathPrefix: z.string().optional().describe("Filter by content path prefix"),
  });

// --- Materials ----------------------------------------------------------------

tool("get_material_graph",
  "Dump the node graph of a Material or MaterialFunction: every expression with its non-default properties (component masks, UV index, constants, parameter names and defaults, textures, transform spaces, reroute declarations) and every connected input pin by name, including function-call inputs and material-attribute pins. A Material also reports its blend mode, shading models and root pins. Only the asset's own nodes are listed; the functions it calls are named in `functions` and dumped with a call of their own. For a MaterialInstance use get_material_parameters.",
  {
    assetPath: assetPath("a Material or MaterialFunction"),
    format: z.enum(["compact", "json"]).optional().describe("compact (default): one line per node, 'Name | Prop=Value | Pin<-Source.Output // desc'. json: one object per node, with editor positions."),
  });

tool("get_material_parameters",
  "Get the effective scalar, vector, texture and static-switch parameter values of a Material or MaterialInstance (resolved through the parent chain), with an `overridden` flag for values set on the asset itself.",
  { assetPath: assetPath("a Material or MaterialInstance") });

tool("get_material_hlsl",
  "The HLSL Unreal generates for a Material or MaterialInstance, with static switches already resolved for that asset — the exact shader, where get_material_graph is only the recipe. section 'generated' (default): just the translator-written functions (world position offset, customized UVs, pixel inputs). 'full': the whole material template, ~250k characters — use outputFile.",
  {
    assetPath: assetPath("a Material or MaterialInstance"),
    section: z.enum(["generated", "full"]).optional().describe("generated (default) or full"),
  },
  120000);

// --- C++ ----------------------------------------------------------------------

tool("get_class_details",
  "Get detailed reflection info for a C++ class: properties, functions, and metadata.",
  { className: z.string().describe("C++ class name") });

tool("search_classes",
  "Search C++ classes by name pattern.",
  {
    query: z.string().describe("Class name search pattern"),
    limit: z.number().optional().describe("Max results (default 50)"),
  });

tool("get_cpp_hierarchy",
  "Get the C++ class hierarchy for a given base class.",
  {
    baseClass: z.string().describe("Base class name (e.g. AActor, UObject)"),
    maxDepth: z.number().optional().describe("Max depth (default 5)"),
  });

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
