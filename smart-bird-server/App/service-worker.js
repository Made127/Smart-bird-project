// PWA 缓存版本号；修改静态资源后提升版本可强制刷新缓存。
const CACHE_NAME = "birdlink-app-v4";

// 需要离线缓存的核心静态资源。
const ASSETS = [
  "./",
  "./index.html",
  "./styles.css",
  "./app.js",
  "./manifest.json",
  "./icon.svg",
  "./assets/bird-mascot.png"
];

// 安装阶段预缓存核心资源。
self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(ASSETS))
  );
  // 这行很重要：让新 Service Worker 安装后立即进入等待激活流程。
  self.skipWaiting();
});

// 激活阶段删除旧版本缓存，避免旧页面资源长期残留。
self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key)))
    )
  );
  // 这行很重要：立即接管已打开页面，避免刷新后仍由旧缓存控制。
  self.clients.claim();
});

// 拦截 GET 请求，优先返回缓存；离线时回退到首页。
self.addEventListener("fetch", (event) => {
  // 非 GET 请求通常是接口写操作，不能被静态缓存接管。
  if (event.request.method !== "GET") return;
  event.respondWith(
    caches.match(event.request).then((cached) =>
      cached || fetch(event.request).catch(() => caches.match("./index.html"))
    )
  );
});
