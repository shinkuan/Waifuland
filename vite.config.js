import { defineConfig } from "vite";

export default defineConfig({
  clearScreen: false,
  build: {
    target: "esnext",
  },
  server: {
    port: 1420,
    strictPort: true,
    watch: {
      ignored: ["**/src-tauri/**"],
    },
  },
});
