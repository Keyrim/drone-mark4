// Bundles the extension into dist/extension.cjs, the CommonJS entry VS Code
// loads (the vscode module is provided by the host and stays external). Run
// with --watch to rebuild on change and get sourcemaps.

import * as esbuild from "esbuild";

const watch = process.argv.includes("--watch");

const options = {
    entryPoints: ["src/extension.ts"],
    outfile: "dist/extension.cjs",
    bundle: true,
    format: "cjs",
    platform: "node",
    target: "node20",
    // vscode is provided by the host; the two ws speedups are optional
    // native modules ws only requires inside a try/catch.
    external: ["vscode", "bufferutil", "utf-8-validate"],
    sourcemap: watch,
    minify: !watch,
    logLevel: "info",
};

if (watch) {
    const context = await esbuild.context(options);
    await context.watch();
} else {
    await esbuild.build(options);
}
