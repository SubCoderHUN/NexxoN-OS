/* ============================================================================
 * NexxoN OS - Web Rendering Proxy Server  (v2.2 1024x768 / Tiled-default)
 * ============================================================================
 * Thin-client architecture: NexxoN OS browser acts as a graphical terminal.
 * This proxy runs headless Chromium via Puppeteer, captures viewport
 * screenshots as JPEG, and streams them over raw TCP.  Input commands
 * (CLICK, SCROLL, KEY, Maps, BACK, REFRESH) arrive as ASCII lines.
 *
 * v2.2 changes:
 *   - Viewport bumped from 800x600 to 1024x768 (matches the NexxoN
 *     screen mode and gives large pages enough room to render without
 *     horizontal scrollbars).
 *   - Stream mode default flipped back to tiled.  Full-frame JPEGs of
 *     a 1024x768 reddit/facebook page routinely exceed 200 KiB, which
 *     blew past the kernel's 32 KiB TCP receive ring and caused the
 *     "browser stops working / restart proxy to recover" failure.
 *     Tiled mode (4x4 = 16 tiles of 256x192, each <= 50 KiB) plays
 *     nicely with the kernel's now-256 KiB ring.
 *   - JPEG quality nudged from 75 to 80 so chroma blocks stay tight
 *     enough that the kernel's nearest-neighbour scaler doesn't smear
 *     them into the rainbow-tile artefacts seen in the bug report.
 *   - Per-tile try/catch hardened so a single detached frame doesn't
 *     cancel the whole sendTiled loop.
 *
 * v2.0 changes:
 *   - Automated FFmpeg download on Windows (no manual PATH config needed).
 *   - Tiled screenshot streaming protocol (TILE header).
 *   - Graceful fallback to video-only if FFmpeg fails or is unavailable.
 *
 * Audio: ffmpeg captures the browser's audio output, encodes to 64 kbps
 * MP3, and streams AUD frames alongside IMG frames on the same socket.
 * Audio is optional; if ffmpeg is missing or fails, the server falls back
 * to video-only mode without crashing.
 *
 * Usage:
 *   npm install          (first time, installs puppeteer + bundled Chromium)
 *   node server.js       (listens on $PROXY_PORT or 9090)
 * ============================================================================ */

const net = require('net');
const dgram = require('dgram');
const puppeteer = require('puppeteer');
const { spawn } = require('child_process');
const os = require('os');
const path = require('path');
const fs = require('fs');
const https = require('https');
const zlib = require('zlib');

/* ---- Configuration --------------------------------------------------- */
const PROXY_PORT      = parseInt(process.env.PROXY_PORT, 10) || 9090;
/* LAN auto-discovery beacon: NexxoN listens on this UDP port and adopts
 * the beacon sender's address as the proxy IP - no manual IP entry.
 * Set DISCOVERY_PORT=0 to disable the beacon. */
const DISCOVERY_PORT  = parseInt(process.env.DISCOVERY_PORT, 10) || 9099;
const DISCOVERY_MS    = parseInt(process.env.DISCOVERY_MS, 10) || 2000;
/* Viewport bumped to 1024x768 to match the NexxoN screen mode and to
 * give large sites (reddit / facebook / news portals) enough horizontal
 * room to render without horizontal scrollbars.  The kernel mirrors
 * these dimensions in BR_FRAME_W / BR_FRAME_H. */
const VIEWPORT_W      = parseInt(process.env.VIEWPORT_W, 10) || 1024;
const VIEWPORT_H      = parseInt(process.env.VIEWPORT_H, 10) || 768;
/* Quality bumped to 92 (was 80).  At 80 the YCbCr 4:2:0 chroma blocks
 * were leaving the rainbow speckles around text the user reported on
 * index.hu — bumping past ~88 keeps fine colour detail intact for the
 * kernel's nearest-neighbour scaler.  PNG would be lossless but adds
 * 3-4x bandwidth, so we keep JPEG and just push the quality higher. */
/* 85 (was 92): the kernel decoder's 4:2:0 chroma path is correct now, so
 * we no longer need to force 4:4:4 via quality>=90 to dodge the rainbow
 * blocks -- and q85/4:2:0 frames are ~2.4x smaller, which is the single
 * biggest lever on end-to-end frame latency. */
const JPEG_QUALITY    = parseInt(process.env.JPEG_QUALITY, 10) || 85;
const TARGET_FPS      = parseInt(process.env.TARGET_FPS, 10) || 15;
const DEFAULT_URL     = process.env.DEFAULT_URL || 'https://www.google.com/webhp?igu=1';
const PROTOCOL_TIMEOUT = parseInt(process.env.PROTOCOL_TIMEOUT, 10) || 60000;

/* Streaming mode: FULL-frame by default again.  Tiled mode does 16
 * serial `page.screenshot({clip:...})` round-trips per frame which is
 * the "0.5-1 s per frame, top-left to bottom-right" lag the user saw
 * after scroll.  It also leaves 1-pixel-wide black gaps at certain
 * tile boundaries because of integer-division rounding when the
 * window content area isn't an exact multiple of the viewport — the
 * "black cross" artefact.  A single full-viewport screenshot avoids
 * both problems and now fits comfortably inside the kernel's enlarged
 * 256 KiB TCP receive ring + 1 MiB JPEG decode buffer.
 *
 * STREAM_MODE=tiled is still available for debugging if needed. */
const STREAM_MODE     = (process.env.STREAM_MODE || 'full').toLowerCase();
const TILE_COLS       = parseInt(process.env.TILE_COLS, 10) || 4;
const TILE_ROWS       = parseInt(process.env.TILE_ROWS, 10) || 4;
const TILE_W          = Math.ceil(VIEWPORT_W / TILE_COLS);
const TILE_H          = Math.ceil(VIEWPORT_H / TILE_ROWS);

/* FFmpeg path - auto-downloaded on Windows if missing. */
const BIN_DIR         = path.join(__dirname, 'bin');
const FFMPEG_EXE      = path.join(BIN_DIR, 'ffmpeg.exe');
const FFMPEG_PATH     = process.env.FFMPEG_PATH ||
                        (os.platform() === 'win32' && fs.existsSync(FFMPEG_EXE)
                         ? FFMPEG_EXE : 'ffmpeg');

/* ---- Helpers --------------------------------------------------------- */
function log(tag, msg) {
    process.stdout.write(`[proxy:${tag}] ${msg}\n`);
}

function sendRaw(sock, buf) {
    if (sock && !sock.destroyed) sock.write(buf);
}

function sendImage(sock, jpegBuf) {
    const hdr = Buffer.from(`IMG ${jpegBuf.length}\n`);
    sendRaw(sock, hdr);
    sendRaw(sock, jpegBuf);
}

function sendTile(sock, x, y, w, h, jpegBuf) {
    const hdr = Buffer.from(`TILE ${x} ${y} ${w} ${h} ${jpegBuf.length}\n`);
    sendRaw(sock, hdr);
    sendRaw(sock, jpegBuf);
}

/* FRAMEEND is a payload-less line the kernel uses to know "all tiles
 * for the current frame have been delivered, present the back-buffer
 * now".  Emitted at the end of every sendTiled() loop iteration so
 * the client never sees a partial top-to-bottom redraw. */
function sendFrameEnd(sock) {
    sendRaw(sock, Buffer.from('FRAMEEND\n'));
}

function sendAudio(sock, mp3Buf) {
    const hdr = Buffer.from(`AUD ${mp3Buf.length}\n`);
    sendRaw(sock, hdr);
    sendRaw(sock, mp3Buf);
}

/* ---- Page validity guard -------------------------------------------- */
function isPageValid() {
    if (!browser || !browser.isConnected()) return false;
    if (!page) return false;
    if (typeof page.isClosed === 'function' && page.isClosed()) return false;
    return true;
}

/* ---- Automated FFmpeg download (Windows) ---------------------------- */
async function ensureFfmpegWindows() {
    if (os.platform() !== 'win32') return false;
    if (fs.existsSync(FFMPEG_EXE)) {
        log('SYS', `FFmpeg found at ${FFMPEG_EXE}`);
        return true;
    }

    log('SYS', 'FFmpeg not found — downloading for Windows ...');
    try {
        fs.mkdirSync(BIN_DIR, { recursive: true });
    } catch (_) {}

    /* Try multiple known static build sources. */
    const sources = [
        {
            url: 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip',
            extract: 'bin/ffmpeg.exe',
        },
        {
            url: 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip',
            extract: 'bin/ffmpeg.exe',
        },
    ];

    for (const src of sources) {
        try {
            log('SYS', `Trying ${src.url} ...`);
            const zipPath = path.join(BIN_DIR, 'ffmpeg_dl.zip');
            await downloadFile(src.url, zipPath);
            log('SYS', 'Download complete — extracting ...');
            const exePath = await extractFile(zipPath, src.extract, FFMPEG_EXE);
            if (exePath) {
                fs.unlinkSync(zipPath);
                log('SYS', `FFmpeg installed at ${FFMPEG_EXE}`);
                return true;
            }
            log('SYS', 'Extraction failed — trying next source ...');
        } catch (err) {
            log('SYS', `Source failed: ${err.message}`);
        }
    }

    log('AUD', 'All FFmpeg download sources failed — video-only mode');
    return false;
}

function downloadFile(url, dest) {
    return new Promise((resolve, reject) => {
        const file = fs.createWriteStream(dest);
        https.get(url, { timeout: 30000 }, (res) => {
            if (res.statusCode === 302 || res.statusCode === 301) {
                file.close();
                fs.unlinkSync(dest);
                downloadFile(res.headers.location, dest).then(resolve).catch(reject);
                return;
            }
            if (res.statusCode !== 200) {
                file.close();
                fs.unlinkSync(dest);
                reject(new Error(`HTTP ${res.statusCode}`));
                return;
            }
            const total = parseInt(res.headers['content-length'], 10) || 0;
            let downloaded = 0;
            res.on('data', (chunk) => {
                downloaded += chunk.length;
                if (total > 0 && downloaded % (1024 * 1024) < chunk.length) {
                    process.stdout.write(`\r  Downloading: ${(downloaded / 1048576).toFixed(1)} MB`);
                }
            });
            res.pipe(file);
            file.on('finish', () => {
                file.close();
                process.stdout.write('\n');
                resolve();
            });
        }).on('error', (err) => {
            file.close();
            try { fs.unlinkSync(dest); } catch (_) {}
            reject(err);
        });
    });
}

function extractFile(zipPath, innerPath, destPath) {
    return new Promise((resolve) => {
        try {
            /* Use Node.js native unzip via child_process (no external dep). */
            const tmpDir = path.join(BIN_DIR, 'ffmpeg_tmp');
            fs.mkdirSync(tmpDir, { recursive: true });

            /* Try PowerShell Expand-Archive first (Windows native). */
            const ps = spawn('powershell', [
                '-NoProfile', '-Command',
                `Expand-Archive -Path '${zipPath.replace(/'/g, "''")}' -DestinationPath '${tmpDir.replace(/'/g, "''")}' -Force`,
            ], { stdio: 'ignore' });

            let settled = false;
            const settle = (result) => {
                if (settled) return;
                settled = true;
                resolve(result);
            };

            ps.on('close', (code) => {
                if (code !== 0) { settle(null); return; }
                /* Search for ffmpeg.exe in the extracted tree. */
                findFile(tmpDir, 'ffmpeg.exe', (found) => {
                    if (found) {
                        try {
                            fs.mkdirSync(path.dirname(destPath), { recursive: true });
                            fs.copyFileSync(found, destPath);
                            settle(destPath);
                        } catch (_) { settle(null); }
                    } else {
                        settle(null);
                    }
                    /* Cleanup tmp. */
                    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (_) {}
                });
            });

            ps.on('error', () => settle(null));
            setTimeout(() => {
                try { ps.kill(); } catch (_) {}
                settle(null);
            }, 60000);
        } catch (_) {
            resolve(null);
        }
    });
}

function findFile(dir, name, cb) {
    try {
        const entries = fs.readdirSync(dir, { withFileTypes: true });
        for (const e of entries) {
            const full = path.join(dir, e.name);
            if (e.isFile() && e.name.toLowerCase() === name.toLowerCase()) {
                cb(full);
                return;
            }
            if (e.isDirectory()) {
                findFile(full, name, cb);
            }
        }
        cb(null);
    } catch (_) { cb(null); }
}

/* ---- Screenshot / tiled video stream -------------------------------- */
/* Capture discipline (the "one page per proxy restart" fix):
 *
 * The old loop raced page.screenshot() against a 3 s timer; on timeout it
 * CLOSED the page and reset `capturingFrame` even though the underlying
 * CDP call was still pending.  The next interval tick then opened a new
 * page while Chromium was still busy, that screenshot timed out too, the
 * recycler closed it again — and the proxy never sent another frame until
 * a manual restart (the wedge is plainly visible in the logs as endless
 * "screenshot timeout - rebuilding" + "capture stuck - recycling" pairs).
 *
 * New rules:
 *   1. `captureInFlight` tracks the REAL screenshot promise; no new
 *      capture starts until it settles.  A slow capture is skipped, never
 *      duplicated.
 *   2. A timeout/transient error NEVER closes the page.  Only hard
 *      detach errors (target/session gone) recycle it — and never while
 *      a navigation is in flight (RESET/goto churn used to close pages
 *      mid-goto: "page closed unexpectedly").
 *   3. The stuck-recycler is the last resort (10 s): closing the page
 *      rejects the pending CDP call, which settles captureInFlight and
 *      unblocks the lane — properly serialized through rebuildPage. */
const CAPTURE_ABANDON_MS = 3000;   /* one screenshot may pend this long   */
const CAPTURE_FAILS_TO_RECYCLE = 3;/* consecutive abandons before recycle */
const BACKPRESSURE_BYTES = 512 * 1024;
let lastSuccessfulCaptureMs = Date.now();
let captureStartMs = 0;
let captureInFlight = null;
let captureGen = 0;                /* bump = pending capture is disowned  */
let captureTimeouts = 0;
let navInFlight = 0;
let backpressureSince = 0;

function isDetachError(msg) {
    return msg.includes('Not attached') ||
           msg.includes('Target closed') ||
           msg.includes('Session closed') ||
           msg.includes('Target.closeTarget') ||
           msg.includes('Most likely the page has been closed');
}

async function recyclePage(reason) {
    log('BRW', `recycling page (${reason})`);
    const dead = page;
    page = null;
    if (dead) {
        /* page.close() itself can hang when the CDP lane is wedged (a
         * pending captureScreenshot blocks Target.closeTarget on some
         * Chromium builds).  Give it 2 s, then move on and rebuild — a
         * leaked about:blank tab is acceptable collateral. */
        try {
            await Promise.race([
                dead.close().catch(() => {}),
                new Promise((r) => setTimeout(r, 2000)),
            ]);
        } catch (_) {}
    }
    await rebuildPageIfNeeded();
}

async function captureFrame(sock) {
    if (captureInFlight) {
        /* A capture is pending - never start a second one (parallel
         * Page.captureScreenshot calls were how the lane wedged).  But a
         * single CDP call can also hang FOREVER (cert-error interstitials
         * never present a frame; page.close doesn't always reject it), so
         * after CAPTURE_ABANDON_MS we DISOWN it: bump captureGen so its
         * late result is discarded, clear the in-flight slot, and let the
         * next tick try fresh.  Repeated abandons escalate to a page
         * recycle — never mid-navigation. */
        const elapsed = Date.now() - captureStartMs;
        if (elapsed > CAPTURE_ABANDON_MS) {
            captureTimeouts++;
            captureGen++;
            captureInFlight = null;
            log('BRW', `capture pending ${elapsed} ms — abandoned `
                     + `(${captureTimeouts}/${CAPTURE_FAILS_TO_RECYCLE})`);
            if (captureTimeouts >= CAPTURE_FAILS_TO_RECYCLE && navInFlight === 0) {
                captureTimeouts = 0;
                recyclePage('repeated capture timeouts').catch(() => {});
            }
        }
        return;
    }
    if (!browserReady) return;
    const ok = await rebuildPageIfNeeded();
    if (!ok) return;
    if (!isPageValid()) return;
    if (!sock || sock.destroyed) return;
    /* Backpressure guard: if the kernel's TCP receive ring is full, skip
     * this frame instead of queueing megabytes on the socket.  Log the
     * episode (once) — silence here hid the real cause for a long time. */
    if (sock.writableLength > BACKPRESSURE_BYTES) {
        if (!backpressureSince) {
            backpressureSince = Date.now();
            log('NET', `backpressure: ${sock.writableLength} B unsent, pausing frames`);
        }
        return;
    }
    if (backpressureSince) {
        log('NET', `backpressure cleared after ${Date.now() - backpressureSince} ms`);
        backpressureSince = 0;
    }

    captureStartMs = Date.now();
    const gen = ++captureGen;
    let p = null;
    if (STREAM_MODE === 'tiled') {
        p = sendTiled(sock)
            .then(() => {
                if (gen !== captureGen) return;     /* disowned */
                captureTimeouts = 0;
                lastSuccessfulCaptureMs = Date.now();
            })
            .catch((e) => {
                if (gen !== captureGen) return;
                const msg = e && e.message ? e.message : String(e);
                log('ERR', `tiled capture: ${msg.split('\n')[0]}`);
            })
            .finally(() => { if (captureInFlight === p) captureInFlight = null; });
        captureInFlight = p;
        return;
    }
    p = page.screenshot({ type: 'jpeg', quality: JPEG_QUALITY })
        .then((jpegBuf) => {
            if (gen !== captureGen) return;         /* disowned: drop stale frame */
            captureTimeouts = 0;
            lastSuccessfulCaptureMs = Date.now();
            if (activeSocket === sock && !sock.destroyed) {
                sendImage(sock, jpegBuf);
                frameCount++;
                if (frameCount % 30 === 0) log('VID', `${frameCount} frames sent`);
            }
        })
        .catch((e) => {
            if (gen !== captureGen) return;
            const msg = e && e.message ? e.message : String(e);
            if (isDetachError(msg)) {
                if (navInFlight === 0) {
                    log('BRW', `page detached (${msg.split('\n')[0]}) - rebuilding`);
                    return recyclePage('detach');
                }
                log('BRW', `capture failed during navigation (${msg.split('\n')[0]}) - skipped`);
            } else {
                log('ERR', `screenshot: ${msg.split('\n')[0]}`);
            }
        })
        .finally(() => { if (captureInFlight === p) captureInFlight = null; });
    captureInFlight = p;
}

async function sendTiled(sock) {
    /* Capture each tile directly via Puppeteer clip option.
     * No full-page screenshot needed — each tile is a separate capture
     * which also avoids the 28 KB threshold heuristic. */
    const docSize = await page.evaluate(() => ({
        width: document.documentElement.scrollWidth || document.body.scrollWidth,
        height: document.documentElement.scrollHeight || document.body.scrollHeight,
        scrollX: window.scrollX,
        scrollY: window.scrollY
    })).catch(() => ({ width: VIEWPORT_W, height: VIEWPORT_H, scrollX: 0, scrollY: 0 }));

    for (let row = 0; row < TILE_ROWS; row++) {
        for (let col = 0; col < TILE_COLS; col++) {
            if (!sock || sock.destroyed) return;
            if (!isPageValid()) return;
            const px = col * TILE_W;
            const py = row * TILE_H;
            const w = Math.min(TILE_W, VIEWPORT_W - px);
            const h = Math.min(TILE_H, VIEWPORT_H - py);
            
            const clipX = px + docSize.scrollX;
            const clipY = py + docSize.scrollY;
            
            let cw = w;
            let ch = h;
            if (clipX + cw > docSize.width) cw = docSize.width - clipX;
            if (clipY + ch > docSize.height) ch = docSize.height - clipY;
            if (cw <= 0 || ch <= 0) continue;

            try {
                const tileBuf = await page.screenshot({
                    type: 'jpeg',
                    quality: JPEG_QUALITY,
                    clip: { x: clipX, y: clipY, width: cw, height: ch },
                });
                sendTile(sock, px, py, cw, ch, tileBuf);
                frameCount++;
            } catch (e) {
                log('ERR', `tile ${col},${row}: ${e.message}`);
            }
        }
    }
    /* All tiles for this frame delivered - emit FRAMEEND so the
     * kernel-side back-buffer composer flips the user-visible frame
     * atomically rather than letting partial updates show through. */
    sendFrameEnd(sock);
}

/* ---- Audio capture (ffmpeg) ----------------------------------------- */
function buildFfmpegArgs() {
    const plat = os.platform();
    if (plat === 'linux') {
        return [
            '-f', 'pulse', '-i', 'default',
            '-f', 'mp3', '-b:a', '64k', '-ar', '44100', '-ac', '2', '-',
        ];
    }
    if (plat === 'win32') {
        return [
            '-f', 'dshow', '-i', 'audio=virtual-audio-cable',
            '-f', 'mp3', '-b:a', '64k', '-ar', '44100', '-ac', '2', '-',
        ];
    }
    if (plat === 'darwin') {
        return [
            '-f', 'avfoundation', '-i', ':0',
            '-f', 'mp3', '-b:a', '64k', '-ar', '44100', '-ac', '2', '-',
        ];
    }
    return null;
}

function probeFfmpeg(ffmpegPath) {
    return new Promise((resolve) => {
        try {
            const probe = spawn(ffmpegPath, ['-version'], {
                stdio: ['ignore', 'ignore', 'ignore'],
            });
            let settled = false;
            const settle = (result) => {
                if (settled) return;
                settled = true;
                resolve(result);
            };
            probe.on('error', () => settle(false));
            probe.on('close', (code) => settle(code === 0 || code === null));
            setTimeout(() => {
                try { probe.kill(); } catch (_) {}
                settle(false);
            }, 10000);
        } catch (_) {
            resolve(false);
        }
    });
}

async function startAudioCapture(sock) {
    if (audioProc || audioPipeActive || audioDisabled) return;

    const args = buildFfmpegArgs();
    if (!args) {
        log('AUD', 'platform not supported for audio capture - video only mode');
        audioDisabled = true;
        return;
    }

    const effectiveFfmpeg = (os.platform() === 'win32' && fs.existsSync(FFMPEG_EXE))
        ? FFMPEG_EXE : (process.env.FFMPEG_PATH || 'ffmpeg');

    try {
        const hasFfmpeg = await probeFfmpeg(effectiveFfmpeg);
        if (!hasFfmpeg) {
            log('AUD', 'Audio disabled: FFmpeg not found or invalid');
            log('AUD', `  Searched: ${effectiveFfmpeg}`);
            if (os.platform() === 'win32') {
                log('AUD', '  Auto-download will be attempted on next start');
            } else {
                log('AUD', '  Install via: apt install ffmpeg / brew install ffmpeg');
            }
            audioDisabled = true;
            return;
        }

        log('AUD', `spawning ffmpeg (${args.slice(0, 4).join(' ')} ...)`);

        audioProc = spawn(effectiveFfmpeg, args, { stdio: ['ignore', 'pipe', 'ignore'] });

        audioProc.stdout.on('data', (chunk) => {
            if (activeSocket === sock && !sock.destroyed && chunk.length > 0) {
                sendAudio(sock, chunk);
            }
        });

        audioProc.on('error', (err) => {
            log('AUD', `ffmpeg error: ${err.message}`);
            audioDisabled = true;
            stopAudioCapture();
        });

        audioProc.on('close', (code) => {
            /* Handle both clean exits and I/O errors.
             * Windows dshow without a virtual cable exits with code 1 or -5.
             * Unsigned -5 = 4294967291.  Use >>> 0 for proper unsigned cast. */
            const unsignedExit = (code >>> 0);
            log('AUD', `ffmpeg exited with code ${code} (unsigned: ${unsignedExit})`);
            if (unsignedExit !== 0 && code !== null) {
                if (unsignedExit === 4294967291 || code === -5 || code === 1) {
                    log('AUD', 'ffmpeg I/O error — no audio device available, video-only mode');
                } else {
                    log('AUD', `ffmpeg exited abnormally (${unsignedExit}) — disabling audio`);
                }
                audioDisabled = true;
            }
            stopAudioCapture();
        });

        audioPipeActive = true;
    } catch (err) {
        log('AUD', `Audio disabled: failed to initialise ffmpeg: ${err.message}`);
        audioDisabled = true;
        audioProc = null;
        audioPipeActive = false;
    }
}

function stopAudioCapture() {
    audioPipeActive = false;
    if (audioProc) {
        try { audioProc.kill(); } catch (_) {}
        audioProc = null;
    }
}

/* ---- Browser lifecycle ---------------------------------------------- */
let browser = null;
let page = null;
let cdpSession = null;
let activeSocket = null;
let frameTimer = null;
let audioProc = null;
let audioPipeActive = false;
let domChangePending = false;
let browserReady = false;
let audioDisabled = false;
let frameCount = 0;
let opChain = Promise.resolve();
/* Bumped on every client attach; queued ops from a previous connection
 * become no-ops instead of replaying a backlog of stale RESET/Maps
 * against the new session. */
let clientGen = 0;
/* Single-flight guard: the 15 fps capture timer and the command chain
 * both call rebuildPageIfNeeded; two concurrent newPage() calls used to
 * race, leaving `page` and `cdpSession` pointing at DIFFERENT pages —
 * every later screenshot failed with "Session closed" until restart. */
let rebuildInFlight = null;

function rebuildPageIfNeeded() {
    if (rebuildInFlight) return rebuildInFlight;
    if (browser && browser.isConnected() &&
        page && !(typeof page.isClosed === 'function' && page.isClosed())) {
        return Promise.resolve(true);
    }
    rebuildInFlight = (async () => {
        /* If the entire Chromium process has died (puppeteer.isConnected
         * goes false, or browser is null), relaunch it.  Without this the
         * proxy would happily run forever with a null page and the client
         * would never see another frame - which manifested as "I have to
         * restart the proxy after every other website" in the user
         * report. */
        if (!browser || !browser.isConnected()) {
            log('BRW', 'browser process unreachable - relaunching Chromium ...');
            try {
                if (browser) { try { await browser.close(); } catch (_) {} }
            } catch (_) {}
            browser = null;
            page = null;
            browserReady = false;
            try {
                await launchBrowser();
            } catch (err) {
                log('ERR', `relaunch failed: ${err.message}`);
                return false;
            }
        }
        if (page && !(typeof page.isClosed === 'function' && page.isClosed())) return true;
        try {
            log('BRW', 'rebuilding page context ...');
            const p = await browser.newPage();
            await p.setViewport({ width: VIEWPORT_W, height: VIEWPORT_H });
            await p.setUserAgent('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36');
            cdpSession = await p.createCDPSession();
            await cdpSession.send('Page.enable');
            cdpSession.on('Page.domContentEventFired', () => { domChangePending = true; });
            cdpSession.on('Page.loadEventFired', () => { domChangePending = true; });
            p.on('error', (err) => { log('BRW', `page error: ${err.message}`); });
            p.on('close', () => { log('BRW', 'page closed'); domChangePending = false; });
            page = p;        /* publish only once fully wired */
            return true;
        } catch (err) {
            log('ERR', `rebuild page failed: ${err.message}`);
            return false;
        }
    })().finally(() => { rebuildInFlight = null; });
    return rebuildInFlight;
}

function queuePageOp(gen, fn) {
    opChain = opChain.then(async () => {
        if (gen !== clientGen) return;      /* stale op from a dead client */
        if (!browserReady) return;
        const ok = await rebuildPageIfNeeded();
        if (!ok || !isPageValid()) return;
        await fn();
    }).catch((err) => {
        log('ERR', `queued op: ${err.message}`);
    });
    return opChain;
}

async function launchBrowser() {
    if (browser) return;
    log('BRW', 'launching headless Chromium ...');
    browser = await puppeteer.launch({
        headless: 'new',
        protocolTimeout: PROTOCOL_TIMEOUT,
        args: [
            '--no-sandbox',
            '--disable-setuid-sandbox',
            '--disable-gpu',
            '--disable-dev-shm-usage',
            '--autoplay-policy=no-user-gesture-required',
            '--disable-blink-features=AutomationControlled',
            '--disable-background-timer-throttling',
            '--disable-extensions',
            `--window-size=${VIEWPORT_W},${VIEWPORT_H}`,
        ],
    });

    page = await browser.newPage();
    await page.setViewport({ width: VIEWPORT_W, height: VIEWPORT_H });

    /* ---- Anti-bot / stealth patches (safe, no ReferenceErrors) ---- */
    await page.evaluateOnNewDocument(() => {
        try {
            Object.defineProperty(navigator, 'webdriver', { get: () => false });
        } catch (_) {}
        try {
            Object.defineProperty(navigator, 'plugins', { get: () => [1, 2, 3, 4, 5] });
        } catch (_) {}
        try {
            Object.defineProperty(navigator, 'languages', { get: () => ['en-US', 'en'] });
        } catch (_) {}
        try {
            if (window.navigator.permissions && typeof window.navigator.permissions.query === 'function') {
                const originalQuery = window.navigator.permissions.query.bind(window.navigator.permissions);
                window.navigator.permissions.query = (parameters) => {
                    if (parameters.name === 'notifications') {
                        try {
                            const perm = typeof Notification !== 'undefined' ? Notification.permission : 'default';
                            return Promise.resolve({ state: perm });
                        } catch (_) {
                            return Promise.resolve({ state: 'default' });
                        }
                    }
                    return originalQuery(parameters);
                };
            }
        } catch (_) {}
    });

    await page.setUserAgent('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36');

    cdpSession = await page.createCDPSession();
    await cdpSession.send('Page.enable');

    cdpSession.on('Page.domContentEventFired', () => { domChangePending = true; });
    cdpSession.on('Page.loadEventFired', () => { domChangePending = true; });

    page.on('error', (err) => { log('BRW', `page error: ${err.message}`); });
    page.on('close', () => {
        log('BRW', 'page closed unexpectedly');
        domChangePending = false;
    });

    browserReady = true;
    log('BRW', `viewport ${VIEWPORT_W}x${VIEWPORT_H} ready`);
}

/* ---- Command dispatch ----------------------------------------------- */
async function dispatchCommand(sock, line, gen) {
    if (!browserReady) return;
    const sp = line.indexOf(' ');
    const cmd = (sp >= 0 ? line.substring(0, sp) : line).toUpperCase();
    const arg = sp >= 0 ? line.substring(sp + 1).trim() : '';

    queuePageOp(gen, async () => {
    try {
        switch (cmd) {
            case 'MAPS': {
                let url = arg;
                if (!url) return;
                if (!/^https?:\/\//i.test(url)) url = 'https://' + url;
                log('CMD', `goto ${url}`);
                /* The old code closed the page here whenever a capture
                 * was in flight — that's what produced the "page closed
                 * unexpectedly" mid-goto churn.  page.goto() replaces
                 * the document on its own; an in-flight screenshot of
                 * the previous page is harmless (it settles and the
                 * single-flight guard starts the next one fresh). */
                navInFlight++;
                try {
                    /* Tight timeout - 8 s is the ceiling.  Pages that
                     * haven't loaded by then keep rendering in the
                     * background; the capture loop streams whatever
                     * has painted so far. */
                    await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 8000 });
                } catch (navErr) {
                    const m = (navErr && navErr.message) || '';
                    log('BRW', `goto warning: ${m.split('\n')[0]}`);
                    if (isDetachError(m)) {
                        await recyclePage('goto detach');
                        try {
                            await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 8000 });
                        } catch (_) {}
                    }
                    /* Net::ERR_TIMED_OUT / TimeoutError both leave us
                     * with a partially-loaded page that's still
                     * screenshot-able - don't kill the page just for
                     * the timeout. */
                } finally {
                    navInFlight--;
                }
                domChangePending = true;
                await new Promise((r) => setTimeout(r, 150));
                await captureFrame(sock);
                break;
            }
            case 'BACK': {
                log('CMD', 'goBack');
                navInFlight++;
                try {
                    await page.goBack({ waitUntil: 'domcontentloaded', timeout: 8000 }).catch(() => {});
                } finally {
                    navInFlight--;
                }
                domChangePending = true;
                await captureFrame(sock);
                break;
            }
            case 'REFRESH': {
                log('CMD', 'reload');
                navInFlight++;
                try {
                    await page.reload({ waitUntil: 'domcontentloaded', timeout: 8000 }).catch(() => {});
                } finally {
                    navInFlight--;
                }
                domChangePending = true;
                await captureFrame(sock);
                break;
            }
            case 'RESET': {
                /* Issued by the client right before a new MAPS.  It used
                 * to close the page whenever a capture was in flight,
                 * which killed gotos in progress and wedged the capture
                 * lane.  The single-flight capture guard made that
                 * unnecessary: just drop the pending dom-change marker;
                 * the MAPS that follows replaces the document anyway. */
                log('CMD', 'reset (client requested)');
                domChangePending = false;
                break;
            }
            case 'PING': {
                /* Cheap heartbeat - the kernel sends this so a stale
                 * connection surfaces via the frame watchdog instead
                 * of leaving the socket in a half-open zombie state.
                 * We simply re-capture; no PONG protocol byte is
                 * required because the next IMG frame IS our pong. */
                if (domChangePending) domChangePending = false;
                await captureFrame(sock);
                break;
            }
            case 'CLICK': {
                const p = arg.split(/\s+/);
                const x = parseInt(p[0], 10);
                const y = parseInt(p[1], 10);
                if (isNaN(x) || isNaN(y)) return;
                log('CMD', `click ${x},${y}`);
                await page.mouse.click(x, y);
                /* Force layout/paint before capture so the screenshot
                 * reflects the updated page state, not the previous frame. */
                try { await page.evaluate(() => document.body.offsetHeight); } catch (_) {}
                await captureFrame(sock);
                break;
            }
            case 'SCROLL': {
                const delta = parseInt(arg, 10);
                if (isNaN(delta)) return;
                log('CMD', `scroll ${delta}`);
                await page.evaluate((d) => window.scrollBy(0, d), delta);
                try { await page.evaluate(() => document.body.offsetHeight); } catch (_) {}
                await captureFrame(sock);
                break;
            }
            case 'KEY': {
                if (!arg) return;
                log('CMD', `key "${arg}"`);
                if (arg.length === 1) {
                    await page.keyboard.type(arg);
                } else {
                    const keyMap = {
                        'enter': 'Enter', 'backspace': 'Backspace', 'delete': 'Delete',
                        'tab': 'Tab', 'escape': 'Escape', 'esc': 'Escape',
                        'arrowup': 'ArrowUp', 'arrowdown': 'ArrowDown',
                        'arrowleft': 'ArrowLeft', 'arrowright': 'ArrowRight',
                        'home': 'Home', 'end': 'End', 'pageup': 'PageUp', 'pagedown': 'PageDown',
                        'f1': 'F1', 'f2': 'F2', 'f3': 'F3', 'f4': 'F4',
                        'f5': 'F5', 'f6': 'F6', 'f7': 'F7', 'f8': 'F8',
                        'f9': 'F9', 'f10': 'F10', 'f11': 'F11', 'f12': 'F12',
                    };
                    const normalized = arg.toLowerCase();
                    const puppeteerKey = keyMap[normalized] || arg;
                    await page.keyboard.press(puppeteerKey);
                }
                try { await page.evaluate(() => document.body.offsetHeight); } catch (_) {}
                await captureFrame(sock);
                break;
            }
            default:
                log('CMD', `unknown: ${cmd}`);
        }
    } catch (e) {
        if (e.message && (e.message.includes('Not attached') || e.message.includes('Target closed'))) {
            log('BRW', 'page detached during command - skipping');
        } else {
            log('ERR', `${cmd}: ${e.message}`);
        }
    }
    });
}

/* ---- Per-client state machine --------------------------------------- */
function attachClient(sock) {
    /* Single-client server: a new connection supersedes the old one.
     * Kill the previous socket EXPLICITLY — when the kernel's RST/FIN
     * never reaches us (lossy LAN, half-open zombie) the old socket
     * would otherwise linger in ESTABLISHED forever. */
    if (activeSocket && activeSocket !== sock && !activeSocket.destroyed) {
        log('NET', `superseding old client ${activeSocket.remotePort}`);
        try { activeSocket.destroy(); } catch (_) {}
    }
    activeSocket = sock;
    const gen = ++clientGen;
    backpressureSince = 0;
    let buf = '';

    /* Frame capture loop at TARGET_FPS. */
    if (frameTimer) clearInterval(frameTimer);
    frameTimer = setInterval(() => {
        if (activeSocket === sock && !sock.destroyed) {
            if (domChangePending) {
                domChangePending = false;
            }
            captureFrame(sock).catch((err) => {
                log('ERR', `frame capture loop: ${err.message}`);
            });
        }
    }, 1000 / TARGET_FPS);

    /* Start audio capture (best-effort, non-blocking). */
    startAudioCapture(sock).catch((err) => {
        log('AUD', `audio init failed: ${err.message}`);
        audioDisabled = true;
    });

    sock.on('data', (data) => {
        buf += data.toString('utf8');
        let nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            const line = buf.substring(0, nl).trim();
            buf = buf.substring(nl + 1);
            if (line) dispatchCommand(sock, line, gen);
        }
    });

    sock.on('close', () => {
        log('NET', `client ${sock.remotePort} disconnected`);
        if (activeSocket === sock) {
            activeSocket = null;
            if (frameTimer) { clearInterval(frameTimer); frameTimer = null; }
            stopAudioCapture();
        }
    });

    sock.on('error', (err) => {
        log('NET', `client ${sock.remotePort} socket error: ${err.message}`);
        if (activeSocket === sock) {
            activeSocket = null;
            if (frameTimer) { clearInterval(frameTimer); frameTimer = null; }
            stopAudioCapture();
        }
    });

    /* Initial frame for first client. Do not force default navigation here:
     * browser_open() sends MAPS on connect and that must remain authoritative. */
    (async () => {
        if (!isPageValid()) return;
        await captureFrame(sock);
    })();
}

/* ---- TCP server ----------------------------------------------------- */
const server = net.createServer((sock) => {
    log('NET', `connection from ${sock.remoteAddress}:${sock.remotePort}`);
    sock.setNoDelay(true);
    attachClient(sock);
});

server.on('error', (err) => {
    log('ERR', `server: ${err.message}`);
    process.exit(1);
});

/* ---- Startup sequence: auto-FFmpeg -> launch browser -> listen ------ */
(async () => {
    /* Step 1: Ensure FFmpeg on Windows. */
    if (os.platform() === 'win32') {
        await ensureFfmpegWindows();
    }

    /* Step 2: Launch headless Chromium. */
    try {
        await launchBrowser();
    } catch (err) {
        log('ERR', `failed to launch browser: ${err.message}`);
        process.exit(1);
    }

    /* Step 3: Start listening. */
    server.listen(PROXY_PORT, '0.0.0.0', () => {
        log('NET', `listening on 0.0.0.0:${PROXY_PORT}`);
        if (STREAM_MODE === 'tiled') {
            log('VID', `mode=tiled ${TILE_COLS}x${TILE_ROWS} tiles (${TILE_W}x${TILE_H} each)`);
        } else {
            log('VID', 'mode=full-frame');
        }
        log('VID', `JPEG quality: ${JPEG_QUALITY}`);
    });

    /* Step 4: LAN discovery beacon.  Broadcast "NEXXON-PROXY v1 <port>"
     * every DISCOVERY_MS so NexxoN finds us without a typed IP.  Sent to
     * 255.255.255.255 AND every interface's directed broadcast (some
     * home routers drop the all-ones form between switch ports). */
    if (DISCOVERY_PORT > 0) {
        const beacon = dgram.createSocket('udp4');
        beacon.bind(() => {
            beacon.setBroadcast(true);
            const msg = Buffer.from(`NEXXON-PROXY v1 ${PROXY_PORT}`);
            const targets = () => {
                const t = ['255.255.255.255'];
                const ifs = os.networkInterfaces();
                for (const name of Object.keys(ifs)) {
                    for (const a of ifs[name]) {
                        if (a.family !== 'IPv4' || a.internal) continue;
                        const ip = a.address.split('.').map(Number);
                        const nm = a.netmask.split('.').map(Number);
                        const bc = ip.map((o, i) => (o & nm[i]) | (~nm[i] & 0xFF));
                        t.push(bc.join('.'));
                    }
                }
                return [...new Set(t)];
            };
            setInterval(() => {
                for (const dst of targets()) {
                    beacon.send(msg, DISCOVERY_PORT, dst, () => {});
                }
            }, DISCOVERY_MS);
            log('NET', `discovery beacon on UDP :${DISCOVERY_PORT} every ${DISCOVERY_MS} ms`);
        });
        beacon.on('error', (e) => log('NET', `discovery beacon error: ${e.message}`));
    }
})();

/* Graceful shutdown. */
process.on('SIGINT', async () => {
    log('SYS', 'shutting down ...');
    server.close();
    if (browser) await browser.close();
    stopAudioCapture();
    process.exit(0);
});
