// Bundles the hub web pages into dist/, which the hub serves as static files.
//
// One ESM bundle per page: every src/<page>/main.ts becomes dist/<page>.js,
// code shared by several pages lands in dist/chunks/. src/shared/style.css is
// a second entry point, so the stylesheet (uPlot included) is a single file
// every page links. The .html files sitting next to this script are copied
// verbatim. Run with --watch to rebuild on change and get sourcemaps.

import * as esbuild from "esbuild";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const SRC = path.join(ROOT, "src");
const DIST = path.join(ROOT, "dist");
const watch = process.argv.includes("--watch");

const pages = fs
    .readdirSync(SRC, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
    .filter((name) => fs.existsSync(path.join(SRC, name, "main.ts")));

const entryPoints = Object.fromEntries([
    ...pages.map((name) => [name, path.join(SRC, name, "main.ts")]),
    ["style", path.join(SRC, "shared", "style.css")],
]);

/** Copies the page markup next to the bundles. */
function copyHtml() {
    fs.mkdirSync(DIST, { recursive: true });
    for (const file of fs.readdirSync(ROOT)) {
        if (file.endsWith(".html")) {
            fs.copyFileSync(path.join(ROOT, file), path.join(DIST, file));
        }
    }
}

const options = {
    entryPoints,
    outdir: DIST,
    bundle: true,
    format: "esm",
    target: "es2022",
    splitting: true,
    chunkNames: "chunks/[name]-[hash]",
    sourcemap: watch,
    minify: !watch,
    logLevel: "info",
};

copyHtml();
if (watch) {
    const context = await esbuild.context({
        ...options,
        plugins: [
            {
                name: "copy-html",
                setup(build) {
                    build.onEnd(copyHtml);
                },
            },
        ],
    });
    await context.watch();
} else {
    await esbuild.build(options);
}
