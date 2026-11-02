import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {defineConfig} from 'vite';

const root = fileURLToPath(new URL('.', import.meta.url));

// API 与健康检查只能由同源 HTTPS 代理交给 ANOF，不能被静态回退吞掉。
function backendBoundary() {
    const install = (server) => {
        server.middlewares.use((request, response, next) => {
            let path;
            try {
                path = decodeURIComponent(new URL(request.url ?? '/', 'http://localhost').pathname);
            } catch {
                response.writeHead(400).end();
                return;
            }
            if (/^\/(?:api|health)(?:\/|$)/u.test(path)) {
                response.writeHead(404, {
                    'Content-Type': 'text/plain; charset=utf-8',
                    'Cache-Control': 'no-store',
                    'X-Content-Type-Options': 'nosniff',
                }).end('Not found');
                return;
            }
            next();
        });
    };
    return {
        name: 'anof-backend-boundary',
        configureServer: install,
        configurePreviewServer: install,
    };
}

export default defineConfig({
    root,
    base: '/',
    publicDir: false,
    envDir: false,
    appType: 'mpa',
    plugins: [backendBoundary(), {
        name: 'runtime-notices', generateBundle() {
            this.emitFile({
                type: 'asset',
                fileName: 'THIRD_PARTY_NOTICES.txt',
                source: readFileSync(new URL('./THIRD_PARTY_NOTICES.txt', import.meta.url), 'utf8')
            });
        }
    }],
    server: {
        host: '127.0.0.1',
        port: 5173,
        strictPort: true,
        cors: false,
        allowedHosts: ['localhost'],
        hmr: false,
        fs: {
            strict: true,
            allow: [root],
            deny: ['.env', '.env.*', '*.{crt,pem,key}', '**/.git/**', '**/.tools/**', '**/.aws/**', '**/.codex/**', '**/.agents/**', '**/.npmrc', '**/tests/**', '**/AGENTS.md'],
        },
    },
    preview: {
        host: '127.0.0.1',
        port: 4173,
        strictPort: true,
        cors: false,
        allowedHosts: ['localhost'],
    },
    build: {
        target: 'es2022',
        outDir: 'dist',
        sourcemap: false,
        modulePreload: {polyfill: false},
    },
});
