// Generates doc/std/*.md from [LittleBinding] comments in the C sources.
// Usage: node scripts/generate-std-docs.mjs [--root ..] [-o doc/std]
import { readFileSync, writeFileSync, readdirSync, statSync, mkdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
let root = path.resolve(here, "..");
let output = path.resolve(root, "doc", "std");

for (let i = 0; i < args.length; i++) {
  if (args[i] === "--root") root = path.resolve(args[++i]);
  else if (args[i] === "-o") output = path.resolve(args[++i]);
}

const GLOBAL_FILES = new Map([
  ["pcall", "core"],
  ["unpack", "core"],
  ["import", "core"],
  ["Promise", "async"],
  ["setTimeout", "async"],
  ["setInterval", "async"],
  ["clearTimeout", "async"],
  ["clearInterval", "async"],
]);

const MEMBER_FILES = new Map([
  ["mainloop.run", "async"],
  ["mainloop.poll", "async"],
  ["mainloop.stop", "async"],
  ["task.run", "task"],
]);

/* Generated files that have a hand-written conceptual guide. */
const GUIDES = new Map([
  ["task", { file: "tasks.md", label: "task execution model, shared state and limitations" }],
  ["async", { file: "async.md", label: "promises, timers and the mainloop" }],
]);

function sourceFiles(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    if (name === "vendor" || name === "build" || name === ".git" || name === ".task") continue;
    const full = path.join(dir, name);
    const stat = statSync(full);
    if (stat.isDirectory()) out.push(...sourceFiles(full));
    else if (name.endsWith(".c")) out.push(full);
  }
  return out.sort();
}

function targetFile(qualified) {
  if (MEMBER_FILES.has(qualified)) return MEMBER_FILES.get(qualified);
  const dot = qualified.indexOf(".");
  if (dot > 0) return qualified.slice(0, dot);
  return GLOBAL_FILES.get(qualified) || "globals";
}

const documents = new Map(); // file -> entries[]
const seen = new Set();

for (const file of sourceFiles(root)) {
  const text = readFileSync(file, "utf8");
  const blocks = /\/\*\* \[LittleBinding\] ([^\s]+)\r?\n([\s\S]*?)\*\//g;
  let match;

  while ((match = blocks.exec(text)) !== null) {
    const qualified = match[1];
    if (seen.has(qualified)) continue;
    seen.add(qualified);

    const description = [];
    const params = [];
    let signature = "";

    for (const raw of match[2].split(/\r?\n/)) {
      const line = raw.replace(/^\s*\*\s?/, "").trimEnd().trim();
      if (!line) continue;

      if (line.startsWith("@signature ")) {
        signature = line.slice("@signature ".length).trim();
        continue;
      }
      if (line.startsWith("@param ")) {
        const rest = line.slice("@param ".length).trim();
        const space = rest.indexOf(" ");
        params.push(space < 0 ? { name: rest, text: "" } : { name: rest.slice(0, space), text: rest.slice(space + 1).trim() });
        continue;
      }
      description.push(line);
    }

    if (!signature) signature = params.length ? `(${params.map((param) => param.name).join(", ")})` : "()";

    const target = targetFile(qualified);
    if (!documents.has(target)) documents.set(target, []);
    documents.get(target).push({ qualified, signature, description: description.join(" "), params });
  }
}

mkdirSync(output, { recursive: true });

for (const [target, entries] of documents) {
  const lines = [`# ${target}`, ""];
  const guide = GUIDES.get(target);
  if (guide) {
    lines.push(`> See the [${guide.label}](../guides/${guide.file}) guide.`, "");
  }
  for (const entry of entries) {
    lines.push(`\`${entry.qualified}${entry.signature}\`${entry.description ? ` ${entry.description}` : ""}`);
    lines.push("");
    for (const param of entry.params) {
      if (!param.text) continue;
      lines.push(`- \`${param.name}\`: ${param.text}`);
    }
    if (entry.params.some((param) => param.text)) lines.push("");
  }

  writeFileSync(path.join(output, `${target}.md`), lines.join("\n"));
}

const names = [...documents.keys()].sort();
console.log(`wrote ${names.length} documents: ${names.join(", ")}`);
